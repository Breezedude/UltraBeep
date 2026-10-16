/*
 * Sensor and log health: sample rates and gaps, barometer noise, spikes and steps, IMU clipping,
 * stuck values, scale and bias at rest, GNSS fix quality and jumps, module errors. No DOM.
 * Findings: {level: "error" | "warn" | "info", sensor, title, detail, times: [s]}.
 */
import { mean, percentile, std } from "./dsp.js";
import { calibrationAt } from "./rawlog.js";

const G0 = 9.80665;

function streamStats(t, nominalHz, toSec) {
  const n = t.length;
  if (n < 2) return { n, rate: 0, gaps: [], missing: 0, backwards: 0, jitterP99: NaN, maxGap: NaN };
  const period = 1000 / nominalHz;
  const gaps = [];
  let missing = 0, backwards = 0, maxGap = 0;
  const d = new Float64Array(n - 1);
  for (let i = 1; i < n; i++) {
    const dt = t[i] - t[i - 1];
    d[i - 1] = dt;
    if (dt <= 0) backwards++;
    if (dt > 2.5 * period) {
      gaps.push({ t: toSec(t[i - 1]), ms: dt });
      missing += Math.round(dt / period) - 1;
    }
    if (dt > maxGap) maxGap = dt;
  }
  return {
    n,
    rate: (n - 1) / ((t[n - 1] - t[0]) / 1000),
    gaps, missing, backwards, maxGap,
    jitterP99: percentile(d, 99) - period,
  };
}

function median5(x, i) {
  const a = [];
  for (let k = i - 2; k <= i + 2; k++) if (k >= 0 && k < x.length) a.push(x[k]);
  a.sort((u, v) => u - v);
  return a[a.length >> 1];
}

export function analyzeHealth(ctx) {
  const log = ctx.log;
  const findings = [];
  const add = (level, sensor, title, detail, times = []) => findings.push({ level, sensor, title, detail, times });
  const toSec = (ms) => (ms - log.t0) / 1000;
  const h = log.header;
  const summary = {};

  // ---- log integrity
  const st = log.stats;
  summary.log = {
    firmware: h.firmware, format: h.format, durationS: (log.tEnd - log.t0) / 1000, sizeBytes: log.size,
    records: st.records, unknown: st.unknown, badLength: st.badLength, truncatedBytes: st.truncatedBytes,
    clockSet: !!h.unixTime,
    settings: log.settings || [],
  };
  if (st.unknown || st.badLength) add("warn", "log", "Damaged records", `${st.unknown} of unknown type, ${st.badLength} with a wrong length`);
  if (st.truncatedBytes) add("info", "log", "Cut-off end", `${st.truncatedBytes} bytes at the end (power off while writing)`);
  if (!h.unixTime) add("info", "log", "Clock not set at the start", "times are relative; UTC only from GNSS");

  // ---- streams
  const sb = streamStats(log.baro.t, h.baroRate || 32, toSec);
  const si = streamStats(log.imu.t, h.imuRate || 100, toSec);
  summary.baroStream = sb;
  summary.imuStream = si;
  for (const [name, s, nominal] of [["barometer", sb, h.baroRate], ["IMU", si, h.imuRate]]) {
    if (s.n < 2) { add("error", name, "No samples", ""); continue; }
    if (Math.abs(s.rate - nominal) > nominal * 0.03) add("warn", name, "Sample rate off", `${s.rate.toFixed(2)} Hz instead of ${nominal} Hz`);
    if (s.gaps.length) {
      add(s.missing > s.n * 0.001 ? "warn" : "info", name, `${s.gaps.length} gaps`,
        `about ${s.missing} samples missing, longest ${s.maxGap.toFixed(0)} ms (raw log ring full or task late)`,
        s.gaps.slice(0, 50).map((g) => g.t));
    }
    if (s.backwards) add("error", name, "Time stamps go backwards", `${s.backwards} times`);
  }

  // ---- barometer
  const alt = ctx.altIsa;
  const nb = alt.length;
  const fs = ctx.baroRate;
  const win = Math.round(10 * fs);
  const noise = [];
  const noiseT = [];
  for (let s = 0; s + win <= nb; s += win) {
    const d = [];
    for (let i = s + 1; i < s + win; i++) d.push(alt[i] - alt[i - 1]);
    noise.push(std(d) / Math.SQRT2);
    noiseT.push(ctx.tS[s + (win >> 1)]);
  }
  const spikes = [];
  const steps = [];
  for (let i = 2; i < nb - 2; i++) {
    const w = Math.max(0.5, 8 * (noise[Math.min(noise.length - 1, Math.floor(i / win))] || 0.05));
    if (Math.abs(alt[i] - median5(alt, i)) > w) spikes.push(ctx.tS[i]);
    if (Math.abs(log.baro.p[i] - log.baro.p[i - 1]) > 20) steps.push(ctx.tS[i]);
  }
  let tMin = Infinity, tMax = -Infinity, rawSat = 0;
  for (let i = 0; i < nb; i++) {
    const tc = log.baro.temp[i];
    if (tc < tMin) tMin = tc;
    if (tc > tMax) tMax = tc;
    if (Math.abs(log.baro.pRaw[i]) >= (1 << 23) - 1) rawSat++;
  }
  // noise by ground speed (airflow over the sensor)
  const bySpeed = [[0, 1], [1, 4], [4, 8], [8, 13], [13, Infinity]].map(([lo, hi]) => {
    const vals = [];
    noise.forEach((v, k) => {
      const i = Math.min(nb - 1, Math.round(noiseT[k] * fs));
      const sp = ctx.gnssSpeed[i];
      if (Number.isFinite(sp) && sp >= lo && sp < hi) vals.push(v);
    });
    return { lo, hi, n: vals.length, noise: vals.length ? mean(vals) : NaN };
  });
  summary.baro = {
    noiseMedian: percentile(noise, 50), noiseP95: percentile(noise, 95), noiseBySpeed: bySpeed,
    spikes: spikes.length, steps: steps.length, tempMin: tMin, tempMax: tMax, rawSaturated: rawSat,
    noiseSeries: { t: noiseT, v: noise },
  };
  if (spikes.length) add("warn", "barometer", `${spikes.length} spikes`, "single samples far off their neighbours", spikes.slice(0, 100));
  if (steps.length) add("warn", "barometer", `${steps.length} pressure steps > 20 Pa`, "between two samples (≈ 1.7 m); knocks, pressure pulses (doors) or airflow", steps.slice(0, 100));
  if (rawSat) add("error", "barometer", "Raw values saturated", `${rawSat} samples`);
  if (percentile(noise, 95) > 0.3) add("warn", "barometer", "High barometer noise", `p95 ${percentile(noise, 95).toFixed(2)} m per sample: airflow over the sensor?`);

  // ---- IMU
  const raw = log.imu.raw;
  const ni = log.imu.n;
  const clip = [0, 0, 0, 0, 0, 0];
  let stuckRuns = 0, run = 0;
  for (let i = 0; i < ni; i++) {
    const o = i * 6;
    for (let k = 0; k < 6; k++) if (raw[o + k] >= 32700 || raw[o + k] <= -32700) clip[k]++;
    if (i > 0) {
      let same = true;
      for (let k = 0; k < 6; k++) if (raw[o + k] !== raw[o - 6 + k]) { same = false; break; }
      run = same ? run + 1 : 0;
      if (run === 10) stuckRuns++;
    }
  }
  // rest windows: 2 s with low rotation and steady |a|
  const restWin = Math.round(2 * (h.imuRate || 100));
  const rests = [];
  const ka = G0 / h.accelLsbG, kg = 1 / h.gyroLsbDps;
  for (let s = 0; s + restWin <= ni; s += restWin) {
    const an = [];
    const gb = [0, 0, 0];
    const am = [0, 0, 0];
    for (let i = s; i < s + restWin; i++) {
      const o = i * 6;
      an.push(Math.hypot(raw[o] * ka, raw[o + 1] * ka, raw[o + 2] * ka));
      for (let k = 0; k < 3; k++) am[k] += raw[o + k] * ka / restWin;
      gb[0] += raw[o + 3] * kg; gb[1] += raw[o + 4] * kg; gb[2] += raw[o + 5] * kg;
    }
    const gm = gb.map((v) => v / restWin);
    if (Math.hypot(gm[0], gm[1], gm[2]) > 10 || std(an) > 0.05) continue; // turning or moving
    let dev = 0;
    for (let i = s; i < s + restWin && dev < 2; i++) {
      const o = i * 6;
      dev = Math.max(dev, Math.abs(raw[o + 3] * kg - gm[0]), Math.abs(raw[o + 4] * kg - gm[1]), Math.abs(raw[o + 5] * kg - gm[2]));
    }
    if (dev < 2) {
      const c = calibrationAt(log, log.imu.t[s]);
      const gc = c ? Math.hypot(...am.map((v, k) => (v - c.offset[k]) * c.scale[k])) : mean(an);
      rests.push({ t: ctx.imuT[s], g: mean(an), gCal: gc, mean: am, bias: gm });
    }
  }
  const restG = rests.map((r) => r.g);
  const calibrated = log.calib.some((c) => c.offset.some((v) => v !== 0) || c.scale.some((v) => v !== 1));
  summary.imu = {
    clipping: { accel: clip.slice(0, 3), gyro: clip.slice(3) }, stuckRuns,
    restWindows: rests.length,
    restG: restG.length ? mean(restG) : NaN,
    restGCal: calibrated && rests.length ? mean(rests.map((r) => r.gCal)) : NaN,
    calibrated,
    calibration: log.calib.length ? log.calib[log.calib.length - 1] : null,
    restGyroBias: rests.length ? [0, 1, 2].map((k) => mean(rests.map((r) => r.bias[k]))) : null,
    restGyroBiasSpread: rests.length > 1 ? [0, 1, 2].map((k) => std(rests.map((r) => r.bias[k]))) : null,
    accMax: ctx.imuAcc.reduce((m, v) => (v > m ? v : m), 0),
    rests,
  };
  if (clip[0] + clip[1] + clip[2]) add("warn", "IMU", "Accelerometer clipping", `${clip.slice(0, 3).join(" / ")} samples at full scale (x/y/z)`);
  if (clip[3] + clip[4] + clip[5]) add("warn", "IMU", "Gyro clipping", `${clip.slice(3).join(" / ")} samples at full scale (x/y/z)`);
  if (stuckRuns) add("warn", "IMU", "Repeated identical samples", `${stuckRuns} runs of ≥ 10 identical readings (FIFO or bus problem)`);
  if (restG.length) {
    const scale = mean(restG) / G0 - 1;
    if (calibrated) {
      const sc = summary.imu.restGCal / G0 - 1;
      add(Math.abs(sc) > 0.01 ? "warn" : "info", "IMU", "Accelerometer at rest (calibrated)",
        `|a| = ${summary.imu.restGCal.toFixed(3)} m/s² (${(sc * 100).toFixed(2)} % from g; raw ${(scale * 100).toFixed(1)} %) in ${rests.length} rest windows`);
    } else {
      add(Math.abs(scale) > 0.02 ? "warn" : "info", "IMU", "Accelerometer scale at rest",
        `|a| = ${mean(restG).toFixed(3)} m/s² (${(scale * 100).toFixed(1)} % from g) in ${rests.length} rest windows; not calibrated (configurator: System → Sensors)`);
    }
    const b = summary.imu.restGyroBias;
    add("info", "IMU", "Gyro bias at rest", `x ${b[0].toFixed(2)}, y ${b[1].toFixed(2)}, z ${b[2].toFixed(2)} °/s` +
      (summary.imu.restGyroBiasSpread ? `, spread ${summary.imu.restGyroBiasSpread.map((v) => v.toFixed(2)).join(" / ")} °/s` : ""));
  }

  // ---- GNSS
  const fx = ctx.fixes;
  let firstValid = -1, losses = 0, jumps = 0, altJumps = 0;
  const jumpT = [];
  for (let i = 0; i < fx.n; i++) {
    if (fx.valid[i] && firstValid < 0) firstValid = i;
    if (i > 0 && fx.valid[i - 1] && !fx.valid[i]) losses++;
    if (i > 0 && fx.valid[i] && fx.valid[i - 1]) {
      const dt = (fx.t[i] - fx.t[i - 1]) / 1000;
      if (dt <= 0 || dt > 2) continue;
      const dy = (fx.lat[i] - fx.lat[i - 1]) * 111195;
      const dx = (fx.lon[i] - fx.lon[i - 1]) * 111195 * Math.cos(fx.lat[i] * Math.PI / 180);
      const v = Math.hypot(dx, dy) / dt;
      if (Math.abs(v - fx.speed[i]) > 15) { jumps++; jumpT.push(ctx.fixT[i]); }
      if (Math.abs(fx.altMsl[i] - fx.altMsl[i - 1]) > 15 * dt + 5) { altJumps++; jumpT.push(ctx.fixT[i]); }
    }
  }
  const validIdx = [];
  for (let i = 0; i < fx.n; i++) if (fx.valid[i]) validIdx.push(i);
  /* "no fix" epochs between two fixes within 0.35 s: the module skipped a cycle (10 Hz with all systems) */
  let placeholders = 0;
  for (let i = 1; i + 1 < fx.n; i++) {
    if (fx.quality[i] === 0 && fx.quality[i - 1] > 0 && fx.quality[i + 1] > 0 && fx.t[i + 1] - fx.t[i - 1] < 350) placeholders++;
  }
  losses = Math.max(0, losses - placeholders);
  const sats = ctx.nmea.sats;
  // barometric vs GNSS altitude: weather drift and GNSS height noise
  const off = [];
  for (let i = 0; i < nb; i++) if (Number.isFinite(ctx.gnssAlt[i])) off.push({ t: ctx.tS[i], d: ctx.gnssAlt[i] - alt[i] });
  let drift = NaN;
  if (off.length > 2 * 60 * fs) {
    const first = mean(off.slice(0, 60 * fs).map((o) => o.d));
    const last = mean(off.slice(-60 * fs).map((o) => o.d));
    drift = last - first;
  }
  summary.gnss = {
    epochs: fx.n, rate: ctx.fixRate, valid: validIdx.length,
    ttffS: firstValid >= 0 ? ctx.fixT[firstValid] : NaN,
    losses, placeholders, jumps, altJumps,
    hdopMedian: percentile(validIdx.map((i) => fx.hdop[i]), 50),
    hdopP95: percentile(validIdx.map((i) => fx.hdop[i]), 95),
    satsMedian: percentile(validIdx.map((i) => fx.sats[i]), 50),
    satsMin: validIdx.length ? validIdx.reduce((m, i) => Math.min(m, fx.sats[i]), Infinity) : NaN,
    goodSatsMedian: sats.n ? percentile(Array.from(sats.good), 50) : NaN,
    top4SnrMedian: sats.n ? percentile(Array.from(sats.top4), 50) : NaN,
    checksumErrors: ctx.nmea.checksumErrors,
    sentences: ctx.nmea.counts,
    baroGnssDriftM: drift,
    baroGnssOffsetSd: off.length ? std(off.map((o) => o.d)) : NaN,
  };
  if (!fx.n) add("warn", "GNSS", "No NMEA epochs", "the GPS was off or did not answer");
  else if (firstValid < 0) add("warn", "GNSS", "No 3D fix in the whole log", "");
  else add("info", "GNSS", "First 3D fix", `${ctx.fixT[firstValid].toFixed(1)} s after the log start; ${ctx.fixRate} fixes per second`);
  if (losses) add("warn", "GNSS", `${losses} fix losses`, "valid → invalid for longer than one epoch");
  if (placeholders > 10) {
    add("warn", "GNSS", `${placeholders} placeholder epochs`, `invalid epochs between valid ones (${(100 * placeholders / fx.n).toFixed(0)} % of all): ` +
      "the module cannot keep the rate with all its satellite systems; the firmware skips them, the real fix rate is lower");
  }
  if (jumps + altJumps) add("warn", "GNSS", `${jumps + altJumps} position or height jumps`, "between consecutive fixes, beyond the reported speed", jumpT.slice(0, 100));
  if (ctx.nmea.checksumErrors) add("warn", "GNSS", "NMEA checksum errors", `${ctx.nmea.checksumErrors} sentences (UART bytes lost?)`);
  if (Number.isFinite(drift) && Math.abs(drift) > 10) add("info", "barometer", "Barometer vs GNSS drift", `${drift.toFixed(1)} m over the log (weather)`);
  const errs = ctx.messages.filter((m) => /\] Err/.test(m.text));
  if (errs.length) add("info", "GNSS", `${errs.length} commands refused by the module`, errs.slice(0, 8).map((m) => m.text).join("; "), errs.map((m) => m.t));

  // ---- events
  const count = (re) => ctx.events.filter((e) => re.test(e.text));
  const underruns = count(/^voice underrun/);
  if (underruns.length) add("warn", "audio", `${underruns.length} voice underruns`, underruns.slice(0, 5).map((e) => e.text).join("; "), underruns.map((e) => e.t));
  const taps = count(/^tap /), buttons = count(/^button down/);
  summary.events = { total: ctx.events.length, taps: taps.length, buttons: buttons.length, underruns: underruns.length };
  const lowBat = count(/^battery/);
  if (lowBat.length) add("info", "power", "Battery warnings", lowBat.map((e) => e.text).join("; "), lowBat.map((e) => e.t));

  const order = { error: 0, warn: 1, info: 2 };
  findings.sort((a, b) => order[a.level] - order[b.level]);
  return { summary, findings };
}
