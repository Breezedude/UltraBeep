#!/usr/bin/env node
/*
 * Self-test of the analyzer core on a synthetic raw log (node test.mjs): parser, NMEA epochs,
 * replay of the firmware filters, metrics, health and the tone curve; flight states on a synthetic
 * IGC flight, the takeoff and landing scenarios of the firmware's logic tests, the 3D map's terrain fit.
 */
import { parseRawLog } from "./js/core/rawlog.js";
import { rotationMean, slopeReference } from "./js/core/dsp.js";
import { buildContext, runAlgorithms } from "./js/core/replay.js";
import { computeMetrics } from "./js/core/metrics.js";
import { analyzeHealth } from "./js/core/health.js";
import { builtin, defaultParams } from "./js/core/registry.js";
import { DEFAULT_CURVE, varioTone } from "./js/core/tones.js";
import { parseIgc, isIgcText } from "./js/core/igc.js";
import { buildTrackContext, contextSeries, fineManeuvers, flightStatesOf } from "./js/core/track.js";
import { FineManeuverDetector, FlightDetector, LANDING_DEFAULTS, ManeuverDetector, TAKEOFF_DEFAULTS } from "./js/core/flightstate.js";
import { drawnAltitude, liftPath, terrainOffset } from "./js/core/terrain.js";
import { attitudeAt, pilotAttitude } from "./js/core/attitude.js";

let pass = 0, fail = 0;
function check(cond, msg) {
  if (cond) pass++;
  else { fail++; console.log(`FAIL ${msg}`); }
}

/* ---- synthetic log: 60 s at rest, a climb of 2 m/s from 20 to 40 s (1 s ramps), a tap at 50 s ---- */
const T0 = 1000;
const climb = (t) => (t < 20 ? 0 : t < 21 ? 2 * (t - 20) : t < 40 ? 2 : t < 41 ? 2 * (41 - t) : 0);
const accel = (t) => (t >= 20 && t < 21 ? 2 : t >= 40 && t < 41 ? -2 : 0);
function height(t) {
  let h = 500;
  for (let s = 0; s < t; s += 0.001) h += climb(s) * 0.001;
  return h;
}
const pressure = (h) => 101325 * Math.pow(1 - h / 44330.77, 1 / 0.190263);

const chunks = [];
function record(type, payload) {
  chunks.push(Uint8Array.of(type, payload.length), payload);
}
{
  const p = new Uint8Array(84);
  const dv = new DataView(p.buffer);
  p.set([85, 66, 82, 87], 0); // UBRW
  dv.setUint8(4, 1); dv.setUint8(5, 32); dv.setUint16(6, 100, true);
  dv.setFloat32(8, 4096, true); dv.setFloat32(12, 16.4, true);
  dv.setUint32(64, T0, true);
  p.set(new TextEncoder().encode("test"), 68);
  record(0, p);
}
{
  // accelerometer calibration as the firmware logs it after the header: z reads 0.26 m/s² too much
  const p = new Uint8Array(28);
  const dv = new DataView(p.buffer);
  dv.setUint32(0, T0, true);
  dv.setFloat32(12, 0.26, true);
  for (let k = 0; k < 3; k++) dv.setFloat32(16 + 4 * k, 1, true);
  record(5, p);
}
const items = [];
for (let k = 0; k < 6000; k++) items.push({ t: T0 + k * 10, kind: "imu" });
for (let k = 0; k < 60 * 32; k++) items.push({ t: T0 + Math.round(k * 31.25), kind: "baro" });
for (let s = 0; s < 60; s++) items.push({ t: T0 + s * 1000 + 5, kind: "nmea", s });
items.push({ t: T0 + 50000, kind: "event", text: "tap 1.00 g (threshold 0.50 g)" });
items.sort((a, b) => a.t - b.t);
const hgt = new Map();
const nmeaLine = (body) => {
  let cs = 0;
  for (const ch of body) cs ^= ch.charCodeAt(0);
  return `$${body}*${cs.toString(16).toUpperCase().padStart(2, "0")}`;
};
for (const it of items) {
  const t = (it.t - T0) / 1000;
  if (it.kind === "imu") {
    const p = new Uint8Array(16);
    const dv = new DataView(p.buffer);
    dv.setUint32(0, it.t, true);
    dv.setInt16(8, Math.round(4096 * (1 + (accel(t) + 0.26) / 9.80665)), true); // az: g + vertical acceleration + 0.26 offset
    record(2, p);
  } else if (it.kind === "baro") {
    const p = new Uint8Array(28);
    const dv = new DataView(p.buffer);
    const tk = Math.round(t * 1000);
    if (!hgt.has(tk)) hgt.set(tk, height(t));
    dv.setUint32(0, it.t, true);
    dv.setFloat32(12, pressure(hgt.get(tk)), true);
    dv.setFloat32(16, 20, true);
    record(1, p);
  } else if (it.kind === "nmea") {
    const hh = `1200${String(it.s).padStart(2, "0")}.00`;
    const h = height(it.s).toFixed(1);
    for (const body of [`GNGGA,${hh},4612.3456,N,01120.7890,E,1,09,0.8,${h},M,46.4,M,,`,
      "GNGSA,A,3,01,02,03,04,05,06,07,08,09,,,,1.2,0.8,1.0,1", `GNRMC,${hh},A,4612.3456,N,01120.7890,E,0.0,0.0,081026,,,A`]) {
      const text = new TextEncoder().encode(nmeaLine(body));
      const p = new Uint8Array(4 + text.length);
      new DataView(p.buffer).setUint32(0, it.t, true);
      p.set(text, 4);
      record(3, p);
    }
  } else {
    const text = new TextEncoder().encode(it.text);
    const p = new Uint8Array(4 + text.length);
    new DataView(p.buffer).setUint32(0, it.t, true);
    p.set(text, 4);
    record(4, p);
  }
}
const size = chunks.reduce((s, c) => s + c.length, 0);
const file = new Uint8Array(size + 3);
let o = 0;
for (const c of chunks) { file.set(c, o); o += c.length; }
file.set([1, 28, 0], o); // a record cut off by a power loss

/* ---- checks ---- */
const log = parseRawLog(file, "synthetic");
check(log.baro.n === 1920 && log.imu.n === 6000 && log.events.length === 1, `record counts ${log.baro.n} ${log.imu.n} ${log.events.length}`);
check(log.nmea.length === 180, `NMEA lines ${log.nmea.length}`);
check(log.stats.truncatedBytes === 3, `cut-off end ${log.stats.truncatedBytes}`);
check(log.header.firmware === "test" && log.header.imuRate === 100, "header");

const ctx = buildContext(log);
check(ctx.fixes.n === 60 && ctx.fixRate === 1, `fixes ${ctx.fixes.n}, rate ${ctx.fixRate}`);
const fixAt30 = ctx.fixes.vd[30];
check(Math.abs(fixAt30 + 2) < 0.05, `GNSS vertical speed at 30 s ${fixAt30}`);
check(Math.abs(ctx.ref[Math.round(30 * 32)] - 2) < 0.02, `reference at 30 s ${ctx.ref[Math.round(30 * 32)]}`);
check(ctx.handlingMask[Math.round(50 * 32)] === 1 && ctx.handlingMask[Math.round(45 * 32)] === 0, "handling window");

/* turn-aware reference: a sink of 10 m/s with a ±5 m modulation of 2.5 s period (a spiral) */
{
  const n = 32 * 40;
  const tS = new Float64Array(n), alt = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    tS[i] = i / 32;
    alt[i] = 1000 - 10 * tS[i] + 5 * Math.sin(2 * Math.PI * tS[i] / 2.5);
  }
  const plain = slopeReference(tS, alt, 2);
  const whole = slopeReference(tS, rotationMean(tS, alt, new Float64Array(n).fill(2.5)), 2);
  let ePlain = 0, eWhole = 0;
  for (let i = 32 * 5; i < 32 * 35; i++) {
    ePlain = Math.max(ePlain, Math.abs(plain[i] + 10));
    eWhole = Math.max(eWhole, Math.abs(whole[i] + 10));
  }
  check(ePlain > 0.3, `plain reference keeps the modulation (max error ${ePlain.toFixed(2)} m/s)`);
  check(eWhole < 0.05, `reference on the one-rotation mean cancels it (max error ${eWhole.toFixed(3)} m/s)`);
}

const specs = builtin.map((algo) => ({ algo, params: defaultParams(algo), label: algo.id }));
const runs = runAlgorithms(ctx, specs);
for (const r of runs) {
  let s = 0, c = 0;
  for (let i = 25 * 32; i < 39 * 32; i++) { s += r.vz[i]; c++; }
  check(Math.abs(s / c - 2) < 0.03, `${r.label}: mean climb 25-39 s ${(s / c).toFixed(3)}`);
  const quiet = Math.abs(r.vz[Math.round(55 * 32)]);
  check(quiet < 0.1, `${r.label}: climb at 55 s ${quiet.toFixed(3)}`);
}
const fusion = runs.find((r) => r.label === "fusion");
check(fusion.diag.rest[Math.round(10 * 32)] === 1, "fusion: at rest at 10 s");
check(log.calib.length === 1 && Math.abs(log.calib[0].offset[2] - 0.26) < 1e-6, "calibration record");
check(Math.abs(fusion.diag.gRef[Math.round(30 * 32)] - 9.80665) < 0.01, `calibrated g ${fusion.diag.gRef[Math.round(30 * 32)]}`);
check(Math.abs(fusion.diag.tilt[Math.round(10 * 32)]) < 1, "fusion: level");
const mF = computeMetrics(ctx, fusion), mB = computeMetrics(ctx, runs.find((r) => r.label === "baro_kf"));
check(mF.lag < mB.lag, `fusion faster than barometer only (${mF.lag.toFixed(2)} vs ${mB.lag.toFixed(2)} s)`);
check(mF.handlingCount === 1, "handling event counted");

/* ---- the manoeuvre modes of the fusion: a spiral, handling, and the g reference after a bad first sample ---- */
/* Raw log from functions of the time: imu(t) -> {a: [m/s²], w: [rad/s]}, height(t) -> m. */
function synthRawLog(seconds, imu, height) {
  const parts = [];
  const rec = (type, payload) => parts.push(Uint8Array.of(type, payload.length), payload);
  const hdr = new Uint8Array(84);
  const hv = new DataView(hdr.buffer);
  hdr.set([85, 66, 82, 87], 0);
  hv.setUint8(4, 1); hv.setUint8(5, 32); hv.setUint16(6, 100, true);
  hv.setFloat32(8, 4096, true); hv.setFloat32(12, 16.4, true);
  hv.setUint32(64, T0, true);
  rec(0, hdr);
  const list = [];
  for (let k = 0; k < seconds * 100; k++) list.push({ t: T0 + k * 10, imu: true });
  for (let k = 0; k < seconds * 32; k++) list.push({ t: T0 + Math.round(k * 31.25), imu: false });
  list.sort((x, y) => x.t - y.t || (x.imu ? 1 : -1));
  for (const it of list) {
    const t = (it.t - T0) / 1000;
    if (it.imu) {
      const { a, w } = imu(t);
      const p = new Uint8Array(16);
      const dv = new DataView(p.buffer);
      dv.setUint32(0, it.t, true);
      for (let k = 0; k < 3; k++) dv.setInt16(4 + 2 * k, Math.round(4096 * a[k] / 9.80665), true);
      for (let k = 0; k < 3; k++) dv.setInt16(10 + 2 * k, Math.round(16.4 * w[k] / DEG), true);
      rec(2, p);
    } else {
      const p = new Uint8Array(28);
      const dv = new DataView(p.buffer);
      dv.setUint32(0, it.t, true);
      dv.setFloat32(12, pressure(height(t)), true);
      dv.setFloat32(16, 20, true);
      rec(1, p);
    }
  }
  const out = new Uint8Array(parts.reduce((n, c) => n + c.length, 0));
  let pos = 0;
  for (const c of parts) { out.set(c, pos); pos += c.length; }
  return parseRawLog(out, "synthetic");
}
const DEG = Math.PI / 180;
const fusionAlgo = builtin.find((a) => a.id === "fusion");
/* the firmware fusion against its own behaviour of 0.42 and earlier (no manoeuvre modes) */
const OLD = { SPIRAL_G: 0, FAST_RATE: 0, HANDLING_RATE: 0, FREEZE_ON_HOLD: 0, REACQ_S: 0, BC_MAX: 100, REST_VS_GREF: 1 };
const replay2 = (log, extra = {}) => runAlgorithms(buildContext(log), [
  { algo: fusionAlgo, params: { ...defaultParams(fusionAlgo), ...OLD }, label: "fusion 0.42" },
  { algo: fusionAlgo, params: { ...defaultParams(fusionAlgo), ...extra }, label: "fusion" },
]);
const rmsOver = (vz, t0, t1, want) => {
  let s = 0, c = 0;
  for (let i = Math.round(t0 * 32); i < Math.round(t1 * 32); i++) { s += (vz[i] - want) ** 2; c++; }
  return Math.sqrt(s / c);
};
{
  // 20 s at rest, a 40 s spiral (3 g, 100 deg/s about one axis, sink 10 m/s with a +-8 m pressure
  // modulation at the rotation period), 20 s level flight at 1 g
  const period = 3.6;
  const imu = (t) => (t < 20 || t >= 60
    ? { a: [0, 0, 9.80665], w: [0, 0, 0] }
    : { a: [0, 2.8 * 9.80665, 9.80665], w: [0, 0, 100 * DEG] });
  const height = (t) => (t < 20 ? 1500 : t < 60 ? 1500 - 10 * (t - 20) + 8 * Math.sin(2 * Math.PI * (t - 20) / period) : 1100);
  const spiralLog = synthRawLog(80, imu, height);
  const spiralCtx = buildContext(spiralLog);
  const yaw = spiralCtx.yawRate;
  check(Math.abs(yaw[Math.round(40 * 32)] + 100) < 2 && Math.abs(yaw[Math.round(10 * 32)]) < 1, `gyro turn rate per sample: ${yaw[Math.round(40 * 32)].toFixed(1)} °/s in the spiral (left turn), ${yaw[Math.round(10 * 32)].toFixed(1)} at rest`);
  // maneuver.c at the device's rate finds the spiral from the gyro (the per-second port needs the GNSS course)
  const fm = fineManeuvers(spiralCtx);
  check(fm.maneuvers.length === 1 && fm.maneuvers[0].kind === "spiral" && Math.abs(fm.maneuvers[0].nMax - 3) < 0.05 &&
    Math.abs(fm.maneuvers[0].t0 - 20) < 1 && Math.abs(fm.maneuvers[0].t1 - 60) < 1 && Math.abs(fm.maxLoad - 3) < 0.05,
  `manoeuvres at the device's rate: ${JSON.stringify(fm.maneuvers)}, peak ${fm.maxLoad.toFixed(2)} g`);
  const [fw, f2] = replay2(spiralLog);
  const modeAt = (t) => f2.diag.mode[Math.round(t * 32)];
  check(modeAt(19) === 0 && modeAt(25) === 1 && modeAt(59) === 1, `spiral mode ${modeAt(19)} ${modeAt(25)} ${modeAt(59)}`);
  check(modeAt(75) === 0, `back to normal after the spiral (mode ${modeAt(75)})`);
  const eFw = rmsOver(fw.vz, 30, 58, -10), eF2 = rmsOver(f2.vz, 30, 58, -10);
  check(eFw > 3, `fusion 0.42 keeps the modulation in the spiral (rms ${eFw.toFixed(2)} m/s)`);
  check(eF2 < 0.7, `the spiral mode cancels it (rms ${eF2.toFixed(2)} m/s)`);
  check(Math.abs(f2.diag.period[Math.round(50 * 32)] - period) < 0.2, `window is one rotation (${f2.diag.period[Math.round(50 * 32)].toFixed(2)} s)`);
  check(rmsOver(f2.vz, 70, 80, 0) < 0.3, `quiet after the spiral (rms ${rmsOver(f2.vz, 70, 80, 0).toFixed(2)} m/s)`);
  check(Math.abs(f2.diag.bias[Math.round(79 * 32)] - f2.diag.bias[Math.round(19 * 32)]) < 0.1, "bias state unchanged by the spiral");
}
{
  // a wingover as maneuver.c sees it at 50 ms: unloaded to 0.4 g for half a second, 2.2 g a second later;
  // the per-second detector sees only 1 s means and never the unloaded moment
  const fine = new FineManeuverDetector(), coarse = new ManeuverDetector();
  const loadAt = (t) => (t >= 2 && t < 2.5 ? 0.4 : t >= 3.5 && t < 4.5 ? 2.2 : 1);
  let fineDone = null, coarseDone = null;
  for (let k = 0; k < 200; k++) {
    const d = fine.update(k * 0.05, loadAt(k * 0.05), 30, 0.05);
    if (d) fineDone = d;
  }
  for (let i = 0; i < 10; i++) {
    let s = 0;
    for (let k = 0; k < 20; k++) s += loadAt(i + k * 0.05);
    const d = coarse.update(i, s / 20, 30, -2, true);
    if (d) coarseDone = d;
  }
  check(fineDone && fineDone.kind === "wingover" && Math.abs(fineDone.nMax - 2.2) < 0.1 && Math.abs(fineDone.t1 - 4.75) < 0.3, // ends 2 s after the smoothed load fell below 1.3 g
    `wingover at the device's rate: ${JSON.stringify(fineDone)}`);
  check(!coarseDone, `the per-second detector misses it (${JSON.stringify(coarseDone)})`);
}
{
  // 10 s at rest, 6 s of handling (fast rotations about changing axes, the load swinging), 14 s at rest
  const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 1]];
  const imu = (t) => {
    if (t < 10 || t >= 16) return { a: [0, 0, 9.80665], w: [0, 0, 0] };
    const ax = axes[Math.floor(t * 5) % axes.length];
    const n = Math.hypot(...ax);
    const sgn = Math.floor(t * 5) % 2 ? 1 : -1;
    const g = 9.80665 * (1 + 0.8 * Math.sin(2 * Math.PI * t * 1.7));
    return { a: [0.3 * g, 0.2 * g, g], w: ax.map((x) => sgn * 300 * DEG * x / n) };
  };
  const [fw, f2] = replay2(synthRawLog(30, imu, () => 1500));
  const modeAt = (t) => f2.diag.mode[Math.round(t * 32)];
  check(modeAt(13) === 2, `handling mode (${modeAt(13)})`);
  check(modeAt(14) !== 1, "no spiral mode in handling");
  check(rmsOver(f2.vz, 11, 16, 0) < 0.5, `stays near zero while handled (rms ${rmsOver(f2.vz, 11, 16, 0).toFixed(2)} m/s)`);
  check(rmsOver(f2.vz, 22, 30, 0) < 0.2, `quiet afterwards (rms ${rmsOver(f2.vz, 22, 30, 0).toFixed(2)} m/s)`);
  check(rmsOver(f2.vz, 11, 16, 0) < rmsOver(fw.vz, 11, 16, 0), "handling: below fusion 0.42");
}
{
  // the first sample 8 % below g (device in the hand at power-on), then at rest: the firmware's rest
  // test never passes again (g_ref stuck at the first sample); now it recovers
  const imu = (t) => ({ a: [0, 0, t < 0.015 ? 0.92 * 9.80665 : 9.80665], w: [0, 0, 0] });
  const [fw, f2] = replay2(synthRawLog(20, imu, () => 1500));
  const i = Math.round(19 * 32);
  check(fw.diag.rest[i] === 0 && Math.abs(fw.diag.gRef[i] - 0.92 * 9.80665) < 0.05, `fusion 0.42 stuck (rest ${fw.diag.rest[i]}, g ${fw.diag.gRef[i].toFixed(2)})`);
  check(f2.diag.rest[i] === 1 && Math.abs(f2.diag.gRef[i] - 9.80665) < 0.1, `recovers (rest ${f2.diag.rest[i]}, g ${f2.diag.gRef[i].toFixed(2)})`); // g_ref learns with a 5 s time constant
}

const health = analyzeHealth(ctx);
check(!health.findings.some((f) => f.level === "error"), "no health errors");
check(health.summary.baroStream.gaps.length === 0 && health.summary.imuStream.gaps.length === 0, "no gaps");
check(Math.abs(health.summary.imu.restG - 10.06665) < 0.01, `raw |a| at rest ${health.summary.imu.restG}`);

const tone = varioTone(DEFAULT_CURVE, 2.0);
check(tone.sound && tone.freq === 751 && tone.period === 442, `tone at 2 m/s ${JSON.stringify(tone)}`);
check(!varioTone(DEFAULT_CURVE, 0.05).sound && varioTone(DEFAULT_CURVE, -3).sound, "lift and sink thresholds");


/* ---- synthetic IGC flight: 90 s on the ground, a straight glide north, 150 s of circling with lift, a glide, a landing ---- */
{
  const lines = ["AXXXTEST", "HFDTE220726", "HFPLTPILOTINCHARGE:Test Pilot", "HFGTYGLIDERTYPE:Test Wing", "I013638FXA"];
  let lat = 46.5, lon = 7.5, alt = 1500, heading = 0;
  const fmt = (v, digits) => String(Math.round(v)).padStart(digits, "0");
  const brec = (sec, lat, lon, alt) => {
    const la = Math.abs(lat), lo = Math.abs(lon);
    const laD = Math.floor(la), laM = (la - laD) * 60, loD = Math.floor(lo), loM = (lo - loD) * 60;
    const h = Math.floor(sec / 3600), m = Math.floor(sec / 60) % 60, se = sec % 60;
    return `B${fmt(h, 2)}${fmt(m, 2)}${fmt(se, 2)}${fmt(laD, 2)}${fmt(laM * 1000, 5)}N${fmt(loD, 3)}${fmt(loM * 1000, 5)}EA${fmt(alt, 5)}${fmt(alt + 47, 5)}010`;
  };
  const step = (speed, hdg, climb) => {
    lat += speed * Math.cos(hdg / 57.29578) / 111319;
    lon += speed * Math.sin(hdg / 57.29578) / (111319 * Math.cos(lat / 57.29578));
    alt += climb;
  };
  let sec = 10 * 3600;
  for (let i = 0; i < 90; i++) lines.push(brec(sec++, lat, lon, alt));
  for (let i = 0; i < 120; i++) { step(10, 0, -1.0); lines.push(brec(sec++, lat, lon, alt)); }
  for (let i = 0; i < 150; i++) { heading += 18; step(10, heading, 1.5); lines.push(brec(sec++, lat, lon, alt)); }
  for (let i = 0; i < 120; i++) { step(10, 0, -1.0); lines.push(brec(sec++, lat, lon, alt)); }
  for (let i = 0; i < 8; i++) { heading += 70; step(6, heading, -12.0); lines.push(brec(sec++, lat, lon, alt)); } /* a spiral: 1 Hz fixes jitter, the sink shows */
  for (let i = 0; i < 20; i++) { step(10, 0, -1.0); lines.push(brec(sec++, lat, lon, alt)); }
  for (let i = 0; i < 90; i++) lines.push(brec(sec++, lat, lon, alt));
  lines.push("GXXXXXXXX");
  const text = lines.join("\r\n");
  check(isIgcText(text), "IGC text recognised");
  const track = parseIgc(text, "test.igc");
  check(track.n === 598 && track.header.pilot === "Test Pilot" && track.header.date === Date.UTC(2026, 6, 22), `IGC parsed: ${track.n} fixes, ${track.header.pilot}`);
  check(Math.abs(track.t[track.n - 1] - 597) < 1e-6 && track.altPress[0] === 1500 && Number.isFinite(track.fxa[0]), "IGC times, altitudes and extensions");
  const tctx = buildTrackContext(track);
  check(tctx.kind === "track" && tctx.tS.length === 598 && tctx.fixes.n === 598 && Math.abs(tctx.fixes.speed[150] - 10) < 1.5, `track context: speed ${tctx.fixes.speed[150].toFixed(1)} m/s`);
  const { series, states } = flightStatesOf(tctx);
  const s = states.summary;
  check(series.source === "igc" && series.n === 598, `series ${series.source} ${series.n} s`);
  check(s.takeoffT != null && s.takeoffT >= 85 && s.takeoffT <= 100, `takeoff at ${s.takeoffT}`);
  check(s.landingT != null && s.landingT >= 506 && s.landingT <= 510, `landing at ${s.landingT}`);
  check(s.thermals >= 1 && s.thermalSeconds >= 100 && s.thermalSeconds <= 160, `thermal: ${s.thermals}, ${s.thermalSeconds} s`);
  check(s.bestThermalClimb > 1.2 && s.bestThermalClimb < 1.8, `thermal climb ${s.bestThermalClimb}`);
  const kinds = states.segments.map((g) => g.kind);
  check(kinds[0] === "ground" && kinds.includes("glide") && kinds.includes("thermal") && kinds[kinds.length - 1] === "ground", `segments ${kinds.join(",")}`);
  const texts = states.events.map((e) => e.text);
  check(texts.includes("takeoff") && texts.includes("landing") && texts.some((t) => t.startsWith("thermal +1.")), `events: ${texts.slice(0, 6).join(" | ")}`);
  check(texts.some((t) => t.startsWith("glide ratio")) && texts.some((t) => t.startsWith("thermal exit")), "glide ratio and thermal exit announced");
  check(s.spirals === 1 && texts.some((t) => t.startsWith("spiral: sink")), `spiral from the sink: ${s.spirals}, ${texts.filter((t) => t.startsWith("spiral")).join("|")}`);
  const mid = states.per;
  const i = 90 + 120 + 100;
  check(states.kinds[mid.state[i]] === "thermal" && Number.isFinite(mid.bank[i]) && mid.bank[i] > 10 && mid.bank[i] < 40, `bank while circling ${mid.bank[i]?.toFixed(0)}°`);
  check(Number.isFinite(mid.glide[90 + 60]) && mid.glide[90 + 60] > 8 && mid.glide[90 + 60] < 12, `glide ratio on the leg ${mid.glide[90 + 60]?.toFixed(1)}`);
}

/* ---- terrain fit of the 3D map: one offset from the lowest ground phase, the track never under the terrain ---- */
{
  const M = 111319.49, lat0 = 46, lon0 = 11, kx = M * Math.cos((lat0 * Math.PI) / 180);
  const at = (x, y) => [lon0 + x / kx, lat0 + y / M];
  /* a slope rising 0.1 m per metre to the north and a 30 m ridge across the way at 1000 m east */
  const dem = (lon, lat) => {
    const x = (lon - lon0) * kx, y = (lat - lat0) * M;
    return 1000 + 0.1 * y + Math.max(0, 30 - 0.003 * (x - 1000) ** 2);
  };
  const fixes = [];
  for (let t = 0; t < 20; t++) { const [lon, lat] = at(0, 0); fixes.push({ t, lon, lat, alt: dem(lon, lat) + 12 }); }
  for (let t = 20; t < 220; t++) { const [lon, lat] = at((t - 20) * 10, 0); fixes.push({ t, lon, lat, alt: 1400 - (t - 20) }); }
  for (let t = 220; t < 240; t++) { const [lon, lat] = at(2000, 0); fixes.push({ t, lon, lat, alt: dem(lon, lat) + 8 }); }
  /* the lower ground phase sets the offset: the landing (8 m) lies on the surface, the launch 4 m above it */
  const fit = terrainOffset(fixes, [19], [221], dem);
  const phaseText = fit.phases.map((p) => `${p.kind} ${p.offset.toFixed(1)}`).join(", ");
  check(Math.abs(fit.offset - 8) < 0.01 && fit.ground.kind === "landing" && fit.phases.length === 2 &&
    Math.abs(fit.phases.find((p) => p.kind === "takeoff").offset - 12) < 0.01, `terrain offset ${fit.offset} (${phaseText})`);
  const above = [fixes[5], fixes[235]].map((f) => f.alt - (dem(f.lon, f.lat) + fit.offset));
  check(Math.abs(above[0] - 4) < 0.01 && Math.abs(above[1]) < 0.01, `launch and landing over the shifted terrain: ${above.map((d) => d.toFixed(2))}`);
  check(Math.abs(terrainOffset(fixes.slice(0, 120), [19], [], dem).offset - 12) < 0.01, "track ending in the air: takeoff offset only");
  check(Math.abs(terrainOffset(fixes, [19], [], dem).offset - 8) < 0.01, "no landing detected, the track ends on the ground: its end counts");
  check(Math.abs(terrainOffset(fixes, [], [], dem).offset - 8) < 0.01, "no takeoff: start and end");
  const [aLon, aLat] = at(900, 0), [bLon, bLat] = at(1100, 0);
  const low = liftPath([[aLon, aLat, dem(aLon, aLat) + fit.offset - 20], [bLon, bLat, dem(bLon, bLat) + fit.offset - 20]], dem, fit.offset);
  check(low.every((p) => p[2] >= 2), "points under the terrain lifted onto it");
  /* across the ridge 5 m above its feet: the drawn line (terrain + offset + height at the points, straight between) */
  const cross = liftPath([[aLon, aLat, dem(aLon, aLat) + fit.offset + 5], [bLon, bLat, dem(bLon, bLat) + fit.offset + 5]], dem, fit.offset);
  let under = 0;
  for (let k = 1; k < cross.length; k++) {
    const p = cross[k - 1], q = cross[k];
    const zp = dem(p[0], p[1]) + fit.offset + p[2], zq = dem(q[0], q[1]) + fit.offset + q[2];
    for (let f = 0; f <= 1.0001; f += 0.05) {
      if (zp + (zq - zp) * f < dem(p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f) + fit.offset) under++;
    }
  }
  check(cross.length > 10 && under === 0, `ridge crossing: ${cross.length} points, ${under} samples under the terrain`);
  check(liftPath([[aLon, aLat, 3000], [bLon, bLat, 3000]], dem, fit.offset).length === 2, "high above the terrain: no extra points");
  check(drawnAltitude(aLon, aLat, 0, dem, fit.offset) === dem(aLon, aLat) + fit.offset + 2, "drawn altitude on the terrain");
}

/* ---- flight.c port: the scenarios of firmware/tests/logic_tests.c (test_flight, test_landing_motion) ---- */
{
  const COSLAT = 0.69466; /* 46 degrees */
  const sim = (landing = LANDING_DEFAULTS) => ({ d: new FlightDetector(TAKEOFF_DEFAULTS, landing), t: 1000, x: 0, y: 0, alt: 1500 });
  /* one second moved by (ve, vn) m and climbed vz; o overrides the reported speed and HDOP or gives the IMU peaks */
  const step = (s, ve, vn, vz, fix, o = {}) => {
    s.t++; s.x += ve; s.y += vn; s.alt += vz;
    return s.d.update({
      t: s.t, fix, lat: 46 + s.y / 111319, lon: 11 + s.x / (111319 * COSLAT), speed: o.speed ?? Math.hypot(ve, vn),
      alt: s.alt, vz, hdop: o.hdop ?? 1.0, motion: !!o.motion, accMaxG: o.amax ?? 0, accMinG: o.amin ?? 0, rateMaxDps: o.rate ?? 0,
    });
  };
  const run = (s, seconds, ve, vn, vz, fix = true, o) => {
    for (let k = 0; k < seconds; k++) {
      const ev = step(s, ve, vn, vz, fix, o);
      if (ev) return { ev, at: s.t };
    }
    return { ev: null, at: 0 };
  };
  const runGps = (s, seconds, ve, speed, hdop) => run(s, seconds, ve, 0, 0, true, { speed, hdop });
  const runM = (s, seconds, ve, vz, fix, amax, amin, rate) => run(s, seconds, ve, 0, vz, fix, { motion: true, amax, amin, rate, speed: Math.abs(ve) });
  const airborne = (fix) => {
    const s = sim();
    run(s, 60, 0, 0, 0);
    run(s, 60, 10, 0, -1);
    runM(s, 60, 9, -1.2, fix, 1.25, 0.9, 40);
    return s;
  };

  let s = sim(), r;
  check(!run(s, 120, 0, 0, 0).ev, "flight.c: standing");
  r = { ev: null };
  for (let k = 0; k < 10 && !r.ev; k++) {
    r = run(s, 10, 1.5, 0, 0);
    if (!r.ev) r = run(s, 10, -1.5, 0, 0);
    if (!r.ev) r = run(s, 20, 0, 0, 0);
  }
  check(!r.ev, `flight.c: walking around at the launch (takeoff at ${r.at})`);
  for (let k = 0; k < 4 && !r.ev; k++) {
    r = run(s, 30, 1.33, 0, 0);
    if (!r.ev) r = run(s, 90, 0, 0, 0);
  }
  check(!r.ev, `flight.c: short walks with breaks (takeoff at ${r.at})`);

  s = sim();
  run(s, 60, 0, 0, 0);
  s.x += 100;
  r = run(s, 10, 0, 0, 0);
  s.x -= 100;
  if (!r.ev) r = run(s, 30, 0, 0, 0);
  check(!r.ev, "flight.c: GPS jump");
  check(!runGps(s, 200, 0.4, 0, 1.0).ev, "flight.c: wandering position");
  check(!runGps(s, 200, 1.5, 1.5, 5.0).ev, "flight.c: poor fixes");

  s = sim();
  run(s, 60, 0, 0, 0);
  let begin = s.t + 1;
  r = run(s, 60, 1.5, 0, 0);
  check(r.ev === "takeoff" && s.d.takeoffS === begin && s.d.rules.includes("launch point"),
    `flight.c: walking away (${r.ev}, dated ${s.d.takeoffS - begin} s, ${s.d.rules})`);

  s = sim();
  run(s, 60, 0, 0, 0);
  begin = s.t + 1;
  run(s, 3, 3, 0, 0);
  r = run(s, 60, 10, 0, -1);
  check(r.ev === "takeoff" && r.at - begin <= 8 && s.d.takeoffS === begin, `flight.c: launch (${r.ev} after ${r.at - begin} s, dated ${s.d.takeoffS - begin} s)`);
  s.d.landing = { ...LANDING_DEFAULTS, calmS: 30 };
  run(s, 120, 8, 2, -1);
  let touchdown = s.t + 1;
  r = run(s, 60, 0, 0, 0);
  check(r.ev === "landing" && r.at - touchdown >= 29 && r.at - touchdown <= 31 && s.d.state === "landed" && s.d.rules[0] === "calm",
    `flight.c: calm landing after ${r.at - touchdown} s`);
  check(!run(s, 200, 1.5, 0, 0).ev, "flight.c: walking away after the landing");
  run(s, 3, 3, 0, 0);
  check(run(s, 30, 10, 0, -1).ev === "takeoff", "flight.c: relaunch after a top landing");

  s = sim();
  run(s, 60, 0, 0, 0);
  begin = s.t + 1;
  r = run(s, 60, 0.3, 0, 1.0);
  check(r.ev === "takeoff" && r.at - begin <= 16 && s.d.takeoffS <= begin + 2, `flight.c: vertical departure (${r.at - begin} s, dated ${s.d.takeoffS - begin} s)`);

  s = sim();
  run(s, 60, 0, 0, 0);
  run(s, 60, 10, 0, -1);
  touchdown = s.t + 1;
  r = run(s, 120, 0, 0, 0);
  check(r.ev === "landing" && r.at - touchdown >= 59 && r.at - touchdown <= 61, `flight.c: default landing after ${r.at - touchdown} s`);
  s = sim({ ...LANDING_DEFAULTS, enabled: false });
  run(s, 60, 0, 0, 0);
  run(s, 60, 10, 0, -1);
  check(!run(s, 600, 0, 0, 0).ev && s.d.state === "airborne", "flight.c: landing switched off");

  s = sim();
  run(s, 60, 0, 0, 0);
  run(s, 60, 10, 0, -1);
  check(!run(s, 45, 0.3, 0, 0.1).ev, "flight.c: hovering 45 s");
  s = sim();
  run(s, 60, 0, 0, 0);
  check(run(s, 60, 0, 3, 1).ev === "takeoff", "flight.c: ridge soaring");
  s = sim();
  check(!run(s, 120, 10, 0, -1, false).ev, "flight.c: no takeoff without a fix");

  /* landing rules with the IMU peaks */
  s = airborne(true);
  runM(s, 1, 3, -1, true, 7.3, 0.15, 860);
  touchdown = s.t;
  r = runM(s, 60, 1.5, 0, true, 1.4, 0.8, 130);
  check(r.ev === "landing" && r.at - touchdown <= 18 && s.d.landingS === touchdown && s.d.rules[0] === "impact",
    `flight.c: impact and walking (${r.ev} after ${r.at - touchdown} s)`);
  check(!runM(s, 60, 1.5, 0, true, 1.4, 0.8, 130).ev && !run(s, 60, 0, 0, 0).ev && s.d.state === "landed",
    "flight.c: no takeoff right after an impact landing (the approach in the window)");
  s = airborne(true);
  s.d.landing = { ...LANDING_DEFAULTS, impact: false, handling: false };
  runM(s, 1, 3, -1, true, 7.3, 0.15, 860);
  check(!runM(s, 120, 1.5, 0, true, 1.4, 0.8, 130).ev, "flight.c: impact rule switched off");
  s = airborne(true);
  check(!runM(s, 20, 15, -15, true, 4.2, 3.5, 170).ev && !runM(s, 30, 2, 0, true, 1.2, 0.9, 40).ev, "flight.c: a spiral is no impact");
  s = airborne(true);
  runM(s, 1, 9, -2, true, 5.0, 0.3, 200);
  check(!runM(s, 60, 9, -1.2, true, 1.25, 0.9, 40).ev, "flight.c: a turbulence shock in flight");
  s = airborne(true);
  runM(s, 1, 1, -1, true, 5.0, 0.3, 200);
  r = { ev: null };
  for (let k = 0; k < 6 && !r.ev; k++) r = runM(s, 15, 0.5, k % 2 ? 1 : -1, true, 1.3, 0.8, 40);
  check(!r.ev, "flight.c: hovering with lift and sink after a shock");
  s = airborne(false);
  runM(s, 1, 0, -1, false, 6.5, 0.2, 900);
  touchdown = s.t;
  r = runM(s, 60, 0, 0, false, 1.4, 0.8, 130);
  check(r.ev === "landing" && r.at - touchdown <= 18, `flight.c: impact without GNSS after ${r.at - touchdown} s`);
  const handling = (landing) => {
    const h = airborne(true);
    if (landing) h.d.landing = landing;
    runM(h, 5, 1, 0, true, 1.5, 0.8, 60);
    const ground = h.t - 4;
    let x = { ev: null };
    for (let k = 0; k < 12 && !x.ev; k++) {
      x = runM(h, 1, 1.5, 0, true, 3.0, 0.3, 450);
      if (!x.ev) x = runM(h, 4, 1.5, 0, true, 1.4, 0.8, 120);
    }
    return { ...x, ground, rule: h.d.rules[0] };
  };
  r = handling();
  check(r.ev === "landing" && r.at - r.ground <= 30 && r.rule === "ground handling", `flight.c: ground handling (${r.ev} after ${r.at - r.ground} s)`);
  check(!handling({ ...LANDING_DEFAULTS, handling: false }).ev, "flight.c: handling rule switched off");
}

/* ---- the flight step on a raw log: first valid fix of a second, the values at that moment, the IMU peaks ---- */
{
  const tS = [], altIsa = [], loggedVz = [], accNorm = [];
  for (let k = 0; k <= 3 * 32; k++) { tS.push(k / 32); altIsa.push(1000 + k / 32); loggedVz.push(k / 32); accNorm.push(9.80665); }
  const fixes = {
    n: 6, lat: [NaN, 46.1, 46.2, NaN, 46.3, 46.4], lon: [NaN, 11, 11, NaN, 11, 11], ve: [0, 1, 1, 0, 1, 1], vn: [0, 0, 0, 0, 0, 0],
    valid: [0, 1, 1, 0, 1, 1], fixType: [1, 3, 3, 1, 3, 3], hdop: [NaN, 0.9, 1.1, NaN, 2.0, 1.0],
  };
  const ctx = {
    fixes, fixT: [0.05, 0.15, 0.95, 1.05, 1.15, 2.5], tS, altIsa, loggedVz, accNorm,
    imuT: [0.1, 0.12, 0.5, 1.1, 1.12, 1.3, 2.4], imuAcc: [9.8, 50, 9.8, 2, 9.8, 9.8, 9.8], imuGyro: [0, 300, 0, 0, 0, 10, 0],
  };
  const se = contextSeries(ctx);
  check(se.n === 4 && se.valid[0] === 1 && se.lat[0] === 46.1 && se.hdop[0] === 0.9 && se.valid[1] === 1 && se.lat[1] === 46.3 && se.valid[2] === 1,
    `placeholder epochs skipped: valid ${Array.from(se.valid)}, lat ${Array.from(se.lat)}`);
  check(se.alt[0] === 1000 + 4 / 32 && se.vz[0] === 4 / 32, `altitude and climb at the fix: ${se.alt[0]} ${se.vz[0]}`);
  check(se.motion[0] === 1 && Math.abs(se.accMax[0] - 50 / 9.80665) < 1e-9 && Math.abs(se.accMin[0] - 9.8 / 9.80665) < 1e-9 && se.rateMax[0] === 300,
    `IMU peaks up to the first step: ${se.accMax[0]} ${se.accMin[0]} ${se.rateMax[0]}`);
  check(Math.abs(se.accMin[1] - 2 / 9.80665) < 1e-9 && se.rateMax[1] === 0 && se.rateMax[2] === 10 && se.motion[3] === 0,
    `IMU peaks between the steps: ${se.accMin[1]} ${se.rateMax[1]} ${se.rateMax[2]} ${se.motion[3]}`);
}

/* ---- the pilot's attitude for the 3D map's glider: a device mounted at a slant, coordinated turns both ways ---- */
{
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const unit = (a) => { const l = Math.hypot(...a); return a.map((x) => x / l); };
  const angle = (a, b) => Math.acos(Math.min(1, Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))) / DEG;
  // the pilot's axes in the device's frame
  const Md = unit([0.3, 0.1, 0.95]), Ld = unit(cross(Md, [0.2, -1, 0.1])), Fd = cross(Ld, Md);
  const V = 10, G = 9.80665;
  // turn rate (deg/s, clockwise positive): ground 0-20 s, straight, right 60-100 s, straight, left 130-170 s, straight to 190 s
  const ramp = (t, t0, t1, r) => (t < t0 || t > t1 ? 0 : r * Math.min(1, (t - t0) / 2, (t1 - t) / 2));
  const rateAt = (t) => ramp(t, 60, 100, 25) + ramp(t, 130, 170, -40);
  const bankAt = (t) => Math.atan(V * rateAt(t) * DEG / G);
  const imu = (t) => {
    const psi = 0, phi = bankAt(t), r = rateAt(t) * DEG; /* the heading drops out of the body-frame vectors */
    const dphi = (bankAt(t + 0.005) - bankAt(t - 0.005)) / 0.01;
    // body axes in the earth frame (east, north, up) and the earth vectors in the body frame
    const f0 = [Math.sin(psi), Math.cos(psi), 0], r0 = [Math.cos(psi), -Math.sin(psi), 0], z = [0, 0, 1];
    const u2 = z.map((v, k) => v * Math.cos(phi) + r0[k] * Math.sin(phi)), l2 = r0.map((v, k) => -v * Math.cos(phi) + z[k] * Math.sin(phi));
    const fE = [r0[0] * V * r * (t >= 20 ? 1 : 0), r0[1] * V * r * (t >= 20 ? 1 : 0), G];
    const wE = [-0 + f0[0] * dphi, f0[1] * dphi, -r];
    const toDev = (e) => {
      const cF = e[0] * f0[0] + e[1] * f0[1] + e[2] * f0[2], cL = e[0] * l2[0] + e[1] * l2[1] + e[2] * l2[2], cM = e[0] * u2[0] + e[1] * u2[1] + e[2] * u2[2];
      return [0, 1, 2].map((k) => Fd[k] * cF + Ld[k] * cL + Md[k] * cM);
    };
    return { a: toDev(fE), w: toDev(wE) };
  };
  const log = synthRawLog(190, imu, () => 1000);
  const ctx = buildContext(log);
  ctx.gnssSpeed = ctx.tS.map((t) => (t >= 20 ? V : 0));
  ctx.gnssClimb = ctx.tS.map(() => 0);
  const att = pilotAttitude(ctx, { takeoffT: 20, landingT: 190 });
  check(att && angle(att.axes.M, Md) < 2 && angle(att.axes.L, Ld) < 5 && angle(att.axes.F, Fd) < 5,
    `pilot axes found in the device's frame: ${att ? `M ${angle(att.axes.M, Md).toFixed(1)}°, L ${angle(att.axes.L, Ld).toFixed(1)}°, F ${angle(att.axes.F, Fd).toFixed(1)}° off` : "none"}`);
  const at = (t) => attitudeAt(att, t) || { bank: NaN, pitch: NaN };
  const want = (t) => bankAt(t) / DEG;
  check(Math.abs(at(90).bank - want(90)) < 3 && Math.abs(at(160).bank - want(160)) < 3,
    `bank in a right turn ${at(90).bank.toFixed(1)}° (${want(90).toFixed(1)}), in a left one ${at(160).bank.toFixed(1)}° (${want(160).toFixed(1)})`);
  check(Math.abs(at(55).bank) < 2 && Math.abs(at(120).bank) < 2 && Math.abs(at(185).bank) < 2 && Math.abs(at(90).pitch) < 3 && Math.abs(at(55).pitch) < 3,
    `level in straight flight: bank ${at(55).bank.toFixed(1)} ${at(120).bank.toFixed(1)} ${at(185).bank.toFixed(1)}°, pitch ${at(55).pitch.toFixed(1)} ${at(90).pitch.toFixed(1)}°`);
  // without turns the side cannot be found: no attitude (the map then estimates the bank from the track)
  const straight = buildContext(synthRawLog(80, (t) => ({ a: Md.map((v) => v * G), w: [0, 0, 0] }), () => 1000));
  check(pilotAttitude(straight, { takeoffT: 20, landingT: 80 }) === null, "no attitude without turns");
}

console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
