/*
 * Analysis context of a log (all streams on a common time axis, references) and the replay of
 * algorithms over the same raw samples. No DOM.
 *
 * Time axis: seconds since log.t0. The barometer samples (32 Hz) are the grid of the charts and
 * of the algorithm outputs; IMU (100 Hz) and GNSS (1 or 10 Hz) are reduced or interpolated onto it.
 */
import { calibrationAt, imuSample, isaAltitude, utcOf } from "./rawlog.js";
import { parseNmea, fixRate } from "./nmea.js";
import { interpolate, movingMean, rotationMean, slopeReference } from "./dsp.js";

export const REF_HALF_S = 2.0; /* ±2 s least-squares slope: smooth, no lag */
export const TURN_REF_RATE = 30; /* °/s: from here the reference window spans whole rotations */
export const TURN_PERIOD_MAX_S = 8;

/*
 * Rotation period per sample for the reference (0 = none): in a turn the pressure port sees a
 * modulation synchronous with the rotation (±5 m at 2-3 g in a spiral), which a least-squares
 * slope over ±2 s keeps in part. The altitude is first averaged, centred, over exactly one
 * period (rotation rate = gyro magnitude averaged over a second), which removes it without lag.
 */
export function referencePeriods(ctx) {
  const n = ctx.tS.length;
  const periods = new Float64Array(n);
  if (!ctx.gyroNorm || !ctx.turnRef) return periods;
  const rate = movingMean(ctx.gyroNorm, Math.round(ctx.baroRate) | 1);
  for (let i = 0; i < n; i++) {
    const r = rate[i];
    if (!Number.isFinite(r) || r < TURN_REF_RATE) continue;
    const period = 360 / r;
    if (period <= TURN_PERIOD_MAX_S) periods[i] = period;
  }
  ctx.turnRate = rate;
  return periods;
}

function referenceOf(ctx, half) {
  return slopeReference(ctx.tS, rotationMean(ctx.tS, ctx.altIsa, referencePeriods(ctx)), half);
}

export function buildContext(log, opts = {}) {
  const t0 = log.t0;
  const b = log.baro;
  const nb = b.n;
  const tS = new Float64Array(nb);
  const altIsa = new Float64Array(nb);
  for (let i = 0; i < nb; i++) {
    tS[i] = (b.t[i] - t0) / 1000;
    altIsa[i] = isaAltitude(b.p[i]);
  }

  // IMU reduced onto the barometer grid: mean |a|, mean and max |ω| of the samples since the previous
  // barometer sample, and the signed turn rate: |ω| with the sign of the rotation about the load axis
  // (the specific force direction, 0.5 s mean), clockwise seen from above positive as the firmware's
  // yaw rate and the GNSS course rate. A wingover shows as an alternating turn rate with the load
  // swinging in step; the per-second series of the detectors average that away.
  const accNorm = new Float32Array(nb).fill(NaN);
  const gyroNorm = new Float32Array(nb).fill(NaN);
  const gyroMax = new Float32Array(nb).fill(NaN);
  const yawRate = new Float32Array(nb).fill(NaN);
  const imuT = new Float64Array(log.imu.n);
  const imuAcc = new Float32Array(log.imu.n);
  const imuGyro = new Float32Array(log.imu.n);
  const imuTurn = new Float32Array(log.imu.n); /* |w| signed by its direction about the specific force (sensors.c turn) */
  const a = [0, 0, 0], w = [0, 0, 0];
  const aDir = [0, 0, 0]; // specific force direction, 0.5 s mean
  const DIR_ALPHA = 0.02;
  let j = 0;
  for (let i = 0; i < nb; i++) {
    let sa = 0, sg = 0, mg = 0, c = 0, sWx = 0, sWy = 0, sWz = 0;
    while (j < log.imu.n && log.imu.t[j] <= b.t[i]) {
      imuSample(log, j, a, w, calibrationAt(log, log.imu.t[j]));
      const an = Math.hypot(a[0], a[1], a[2]);
      const gn = Math.hypot(w[0], w[1], w[2]) * 180 / Math.PI;
      imuT[j] = (log.imu.t[j] - t0) / 1000;
      imuAcc[j] = an;
      imuGyro[j] = gn;
      if (an > 0) for (let k = 0; k < 3; k++) aDir[k] += DIR_ALPHA * (a[k] / an - aDir[k]);
      imuTurn[j] = w[0] * aDir[0] + w[1] * aDir[1] + w[2] * aDir[2] < 0 ? gn : -gn;
      sa += an; sg += gn; if (gn > mg) mg = gn; c++;
      sWx += w[0]; sWy += w[1]; sWz += w[2];
      j++;
    }
    if (c) {
      accNorm[i] = sa / c; gyroNorm[i] = sg / c; gyroMax[i] = mg;
      const along = sWx * aDir[0] + sWy * aDir[1] + sWz * aDir[2];
      yawRate[i] = along < 0 ? sg / c : -sg / c; // right-hand rule about "up" is counter-clockwise
    }
  }
  for (; j < log.imu.n; j++) {
    imuSample(log, j, a, w, calibrationAt(log, log.imu.t[j]));
    imuT[j] = (log.imu.t[j] - t0) / 1000;
    imuAcc[j] = Math.hypot(a[0], a[1], a[2]);
    imuGyro[j] = Math.hypot(w[0], w[1], w[2]) * 180 / Math.PI;
  }

  const nmea = parseNmea(log.nmea);
  const fx = nmea.fixes;
  const fixT = new Float64Array(fx.n);
  for (let i = 0; i < fx.n; i++) fixT[i] = (fx.t[i] - t0) / 1000;
  const fixGap = 2.5; // s: no interpolation across longer gaps
  const validOnly = (arr) => arr.map((v, i) => (fx.valid[i] ? v : NaN));
  const gnssClimb = interpolate(fixT, validOnly(fx.vd).map((v) => -v), tS, fixGap);
  const gnssAlt = interpolate(fixT, validOnly(fx.altMsl), tS, fixGap);
  const gnssSpeed = interpolate(fixT, validOnly(fx.speed), tS, fixGap);

  // ground track turn rate from consecutive fixes (deg/s), on the grid
  const courseRate = new Float64Array(fx.n).fill(NaN);
  for (let i = 1; i < fx.n; i++) {
    if (!fx.valid[i] || !fx.valid[i - 1] || fx.speed[i] < 2 || fx.speed[i - 1] < 2) continue;
    let d = fx.course[i] - fx.course[i - 1];
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    const dt = fixT[i] - fixT[i - 1];
    if (dt > 0 && dt < 2.5) courseRate[i] = d / dt;
  }
  const trackRate = interpolate(fixT, courseRate, tS, fixGap);

  const events = log.events.map((e) => ({ t: (e.t - t0) / 1000, text: e.text }));
  const messages = nmea.messages.map((m) => ({ t: (m.t - t0) / 1000, text: m.text }));

  const ctx = {
    log, t0, tS, altIsa,
    baroRate: log.header.baroRate || 32,
    imuRate: log.header.imuRate || 100,
    loggedVz: b.vz, loggedAlt: b.alt,
    accNorm, gyroNorm, gyroMax, yawRate, imuT, imuAcc, imuGyro, imuTurn,
    nmea, fixes: fx, fixT, fixRate: fixRate(fx),
    gnssClimb, gnssAlt, gnssSpeed, trackRate,
    events, messages,
    refHalf: opts.refHalf ?? REF_HALF_S,
    turnRef: opts.turnRef ?? true, /* the altitude is averaged over one rotation in turns */
    turnRate: null,
    ref: null,
    utc: (tSec) => utcOf(log, t0 + tSec * 1000),
  };
  ctx.ref = referenceOf(ctx, ctx.refHalf);

  // Handling (taps, button presses) comes with pressure pulses that the reference follows too:
  // these windows are left out of the error metrics and judged against a robust reference instead.
  ctx.handlingMask = new Uint8Array(nb);
  for (const e of events) {
    if (!(e.text.startsWith("tap ") || e.text.startsWith("button "))) continue;
    let i = lowerBound(tS, e.t - HANDLING_BEFORE_S);
    for (; i < nb && tS[i] <= e.t + HANDLING_AFTER_S; i++) ctx.handlingMask[i] = 1;
  }
  ctx.refRobust = slopeReference(tS, movingMedian(altIsa, Math.round(4 * ctx.baroRate) | 1), 10);
  return ctx;
}

export const HANDLING_BEFORE_S = 1;
export const HANDLING_AFTER_S = 4;

function lowerBound(t, x) {
  let lo = 0, hi = t.length;
  while (lo < hi) { const m = (lo + hi) >> 1; if (t[m] < x) lo = m + 1; else hi = m; }
  return lo;
}

/* Centered moving median (w odd): removes short pressure pulses, keeps lasting changes. */
function movingMedian(x, w) {
  const n = x.length, half = w >> 1;
  const out = new Float64Array(n);
  const win = []; // sorted values of x[i - half .. i + half]
  const pos = (v) => {
    let lo = 0, hi = win.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (win[m] < v) lo = m + 1; else hi = m; }
    return lo;
  };
  for (let k = 0; k < Math.min(n, half); k++) win.splice(pos(x[k]), 0, x[k]);
  for (let i = 0; i < n; i++) {
    const add = i + half, rem = i - half - 1;
    if (add < n) win.splice(pos(x[add]), 0, x[add]);
    if (rem >= 0) win.splice(pos(x[rem]), 1);
    out[i] = win[win.length >> 1];
  }
  return out;
}

export function setReference(ctx, half, turnRef = ctx.turnRef) {
  ctx.refHalf = half;
  ctx.turnRef = turnRef;
  ctx.ref = referenceOf(ctx, half);
}

/*
 * Replays every spec ({algo, params, label}) over the raw samples in time order. Order at equal
 * time stamps as on the device: barometer, IMU, event, GNSS fix. Returns one run per spec with
 * vz and h on the barometer grid and the algorithm's diagnostics.
 */
export function runAlgorithms(ctx, specs) {
  const log = ctx.log;
  const b = log.baro, imu = log.imu;
  const nb = b.n, ni = imu.n;
  const fx = ctx.fixes;
  const events = log.events;
  const dtImu = 1 / ctx.imuRate;
  const runs = specs.map((spec) => {
    const inst = spec.algo.create({ ...spec.params }, { header: log.header });
    inst.baroDt = 1 / ctx.baroRate;
    const diag = {};
    for (const d of spec.algo.diagnostics || []) diag[d.key] = new Float32Array(nb).fill(NaN);
    return { spec, inst, vz: new Float32Array(nb), h: new Float32Array(nb), diag, diagKeys: Object.keys(diag), ms: 0 };
  });
  const insts = runs.map((r) => r.inst);
  const a = [0, 0, 0], w = [0, 0, 0];
  const fixObj = {};
  const diagOut = {};
  let ib = 0, ii = 0, ie = 0, iff = 0, ic = 0;
  let cal = null;
  const started = Date.now();
  for (;;) {
    const tb = ib < nb ? b.t[ib] : Infinity;
    const ti = ii < ni ? imu.t[ii] : Infinity;
    const te = ie < events.length ? events[ie].t : Infinity;
    const tf = iff < fx.n ? fx.t[iff] : Infinity;
    const tMin = Math.min(tb, ti, te, tf);
    if (tMin === Infinity) break;
    if (tb === tMin) {
      const alt = ctx.altIsa[ib];
      for (let k = 0; k < runs.length; k++) {
        const r = runs[k];
        r.inst.baro(tb, alt, b.p[ib]);
        r.vz[ib] = r.inst.vz;
        r.h[ib] = r.inst.h;
        if (r.diagKeys.length) {
          r.inst.diag(diagOut);
          for (const key of r.diagKeys) r.diag[key][ib] = diagOut[key];
        }
      }
      ib++;
    } else if (ti === tMin) {
      while (ic < log.calib.length && log.calib[ic].t <= ti) cal = log.calib[ic++];
      imuSample(log, ii, a, w, cal); // calibrated as on the device
      for (let k = 0; k < insts.length; k++) insts[k].imu(ti, w, a, dtImu);
      ii++;
    } else if (te === tMin) {
      for (let k = 0; k < insts.length; k++) insts[k].event(te, events[ie].text);
      ie++;
    } else {
      fixObj.utc = fx.utc[iff]; fixObj.lat = fx.lat[iff]; fixObj.lon = fx.lon[iff];
      fixObj.altMsl = fx.altMsl[iff]; fixObj.geoid = fx.geoid[iff]; fixObj.speed = fx.speed[iff];
      fixObj.course = fx.course[iff]; fixObj.vn = fx.vn[iff]; fixObj.ve = fx.ve[iff]; fixObj.vd = fx.vd[iff];
      fixObj.fixType = fx.fixType[iff]; fixObj.sats = fx.sats[iff]; fixObj.hdop = fx.hdop[iff];
      fixObj.valid = !!fx.valid[iff];
      for (let k = 0; k < insts.length; k++) insts[k].fix(tf, fixObj);
      iff++;
    }
  }
  const ms = Date.now() - started;
  return runs.map((r) => ({
    label: r.spec.label || r.spec.algo.id,
    algoId: r.spec.algo.id,
    params: { ...r.spec.params },
    color: r.spec.color,
    vz: r.vz, h: r.h, diag: r.diag,
    diagnostics: r.spec.algo.diagnostics || [],
    ms,
  }));
}

/* The device's own output from the log (climb rate and altitude of the filter it used). */
export function loggedRun(ctx) {
  return {
    label: "device (logged)", algoId: "logged", params: {}, color: "#9aa4ad",
    vz: ctx.loggedVz, h: ctx.loggedAlt, diag: {}, diagnostics: [], ms: 0,
  };
}
