/*
 * Attitude of the pilot from the IMU of a raw log, for the glider on the 3D map: bank and pitch.
 * No DOM.
 *
 * The device sits somewhere on the pilot (cockpit, riser, helmet) in an unknown orientation, so the
 * pilot's axes are first found in the device's frame from the flight itself:
 *  - vertical M: the mean specific force in straight, calm flight, where the pilot hangs along it;
 *  - left L: in a steady turn the rotation vector is the true vertical, while the specific force
 *    stays along the pilot (a coordinated turn), so the true vertical leans to the pilot's left in a
 *    right turn and to the right in a left one; L is the mean of that lean, signed by the turn;
 *  - forward F = L × M.
 * Then a complementary filter tracks the true vertical u in the device's frame: the gyro turns it,
 * the accelerometer pulls it towards the specific force less the centripetal acceleration
 * ω × (V·F), V the GNSS speed. That is g·u in straight flight and in turns alike, where the
 * firmware's attitude (fusion.c) holds the apparent vertical and so shows gentle turns without bank.
 * In a fast, steady rotation (a spiral, beyond the accelerometer's gate) the rotation axis is the
 * vertical and pulls u instead; the gyro alone would let any error of u circle at the spin rate.
 * Bank and pitch are the angles of u in the pilot's axes. The heading is not estimated: the map
 * takes it from the GNSS track.
 */
import { calibrationAt, G0, imuSample } from "./rawlog.js";

const DEG = Math.PI / 180;
export const ATT_STEP_S = 0.05;   /* output grid */
const SMOOTH_S = 0.5;             /* means of a and ω for finding the axes */
const STILL_RATE = 3 * DEG;       /* rad/s: straight and calm, for M and the gyro bias */
const TURN_MIN = 8 * DEG, TURN_MAX = 80 * DEG; /* rad/s: turns, for L (in a spiral the nose is down, and the lean is fore and aft as much) */
const MIN_STRAIGHT_S = 20, MIN_TURN_S = 15;
const MIN_LEAN = 0.05;             /* the turns' mean lean (sin of the bank) must show a side */
const TAU_S = 3;                  /* the accelerometer's pull on u */
const GATE = 0.25;                /* ... while |a - centripetal| is within ±25 % of g */
const SPIN_RATE = 30 * DEG;       /* rad/s: from here a steady rotation (0.5 s mean) ... */
const SPIN_COHER = 0.85;          /* ... about a steady axis (|mean ω| / mean |ω|) ... */
const SPIN_TAU_S = 0.7;           /* ... pulls u towards its axis */

const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const unit = (a) => { const l = Math.hypot(a[0], a[1], a[2]); return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : null; };

/* the IMU samples of a log in time order, calibrated as on the device: fn(tSec, a, w) */
function eachImu(ctx, fn) {
  const log = ctx.log, n = log.imu.n;
  const a = [0, 0, 0], w = [0, 0, 0];
  let ic = 0, cal = calibrationAt(log, -Infinity);
  for (let j = 0; j < n; j++) {
    const tMs = log.imu.t[j];
    while (ic < log.calib.length && log.calib[ic].t <= tMs) cal = log.calib[ic++];
    imuSample(log, j, a, w, cal);
    fn((tMs - ctx.t0) / 1000, a, w);
  }
}

/* index into the barometer grid, advanced monotonically */
function gridCursor(tS) {
  let i = 0;
  return (t) => { while (i < tS.length - 1 && tS[i + 1] <= t) i++; return i; };
}

/*
 * The pilot's axes in the device's frame and the gyro bias, or null when the flight has too little
 * straight flight or too few turns to find them. inFlight(t) tells the airborne part.
 */
export function pilotAxes(ctx, inFlight) {
  const dtN = 1 / (ctx.imuRate || 100);
  const alpha = dtN / (SMOOTH_S + dtN);
  const fs = [0, 0, 0], ws = [0, 0, 0];
  let started = false;
  const bias = [0, 0, 0];
  let nBias = 0;
  const M = [0, 0, 0];
  let nM = 0;
  const turns = []; /* [f, ω] of steady turns, every 5th sample */
  let k = 0;
  eachImu(ctx, (t, a, w) => {
    if (!started) { fs.splice(0, 3, ...a); ws.splice(0, 3, ...w); started = true; }
    for (let d = 0; d < 3; d++) { fs[d] += alpha * (a[d] - fs[d]); ws[d] += alpha * (w[d] - ws[d]); }
    const an = Math.hypot(a[0], a[1], a[2]), rate = Math.hypot(w[0], w[1], w[2]);
    const flying = inFlight(t);
    if (!flying && rate < STILL_RATE && Math.abs(an / G0 - 1) < 0.03) {
      for (let d = 0; d < 3; d++) bias[d] += w[d];
      nBias++;
    }
    if (!flying) return;
    const fn = Math.hypot(fs[0], fs[1], fs[2]), wn = Math.hypot(ws[0], ws[1], ws[2]);
    if (wn < STILL_RATE && Math.abs(fn / G0 - 1) < 0.05) {
      for (let d = 0; d < 3; d++) M[d] += fs[d] / fn;
      nM++;
    } else if (wn > TURN_MIN && wn < TURN_MAX && Math.abs(fn / G0 - 1) < 0.5 && k++ % 5 === 0) {
      turns.push([fs[0], fs[1], fs[2], ws[0], ws[1], ws[2]]);
    }
  });
  const rate = ctx.imuRate || 100;
  if (nM < MIN_STRAIGHT_S * rate || turns.length * 5 < MIN_TURN_S * rate) return null;
  if (nBias) for (let d = 0; d < 3; d++) bias[d] /= nBias;
  const m = unit(M);
  // the true vertical (the rotation vector, turned to point up) leans to the pilot's left in a right turn
  const L = [0, 0, 0];
  for (const x of turns) {
    const f = [x[0], x[1], x[2]], w = unit([x[3] - bias[0], x[4] - bias[1], x[5] - bias[2]]);
    if (!w) continue;
    const right = dot(w, f) < 0; /* clockwise seen from above: the rotation vector points down */
    const up = right ? [-w[0], -w[1], -w[2]] : w;
    const along = dot(up, m), s = right ? 1 : -1;
    for (let d = 0; d < 3; d++) L[d] += s * (up[d] - along * m[d]);
  }
  const l = unit(L);
  if (!l) return null;
  const lean = Math.hypot(L[0], L[1], L[2]) / turns.length; /* sin of the mean bank in the turns */
  if (lean < MIN_LEAN) return null;
  return { M: m, L: l, F: cross(l, m), bias, lean, straightS: nM / rate, turnS: turns.length * 5 / rate };
}

/*
 * Bank (right wing down positive) and pitch (nose up positive) in degrees on a grid of
 * ATT_STEP_S, from a raw log with an IMU; null without one or when the axes cannot be found.
 * Returns {t, bank, pitch, axes}.
 */
export function pilotAttitude(ctx, { takeoffT = null, landingT = null } = {}) {
  if (!ctx.log || !ctx.log.imu || ctx.log.imu.n < 100 || ctx.kind === "track") return null;
  const speedAt = gridCursor(ctx.tS);
  const inFlight = takeoffT != null
    ? (t) => t >= takeoffT && (landingT == null || t <= landingT)
    : (t) => ctx.gnssSpeed[speedAt(t)] > 5;
  const axes = pilotAxes(ctx, inFlight);
  if (!axes) return null;
  const { M, L, F, bias } = axes;
  const cursor = gridCursor(ctx.tS);
  const tEnd = (ctx.log.imu.t[ctx.log.imu.n - 1] - ctx.t0) / 1000;
  const n = Math.max(0, Math.floor(tEnd / ATT_STEP_S) + 1);
  const t = new Float32Array(n), bank = new Float32Array(n).fill(NaN), pitch = new Float32Array(n).fill(NaN);
  for (let i = 0; i < n; i++) t[i] = i * ATT_STEP_S;
  let u = null, last = null, next = 0, V = 0;
  const ws = [0, 0, 0];
  let rs = 0;
  eachImu(ctx, (tS, a, w) => {
    const dt = last == null ? 0 : Math.min(0.1, Math.max(0, tS - last));
    last = tS;
    const wc = [w[0] - bias[0], w[1] - bias[1], w[2] - bias[2]];
    const alpha = dt / (SMOOTH_S + dt);
    for (let d = 0; d < 3; d++) ws[d] += alpha * (wc[d] - ws[d]);
    rs += alpha * (Math.hypot(wc[0], wc[1], wc[2]) - rs);
    // the speed along the path (GNSS, held through gaps): in a spiral most of it is the sink
    const ig = cursor(tS), vh = ctx.gnssSpeed[ig], vz = ctx.gnssClimb[ig];
    if (Number.isFinite(vh)) V = Math.hypot(vh, Number.isFinite(vz) ? vz : 0);
    const cen = cross(wc, [V * F[0], V * F[1], V * F[2]]);
    const fc = [a[0] - cen[0], a[1] - cen[1], a[2] - cen[2]];
    const fn = Math.hypot(fc[0], fc[1], fc[2]);
    if (!u) {
      if (fn > 0.5 * G0) u = [fc[0] / fn, fc[1] / fn, fc[2] / fn];
      return;
    }
    // a vector fixed in the earth frame turns against the device: du/dt = u × ω
    const du = cross(u, wc);
    for (let d = 0; d < 3; d++) u[d] += du[d] * dt;
    if (Math.abs(fn / G0 - 1) < GATE) {
      const k = dt / TAU_S;
      for (let d = 0; d < 3; d++) u[d] += k * (fc[d] / fn - u[d]);
    }
    const wn = Math.hypot(ws[0], ws[1], ws[2]);
    if (wn > SPIN_RATE && wn > SPIN_COHER * rs) {
      const k = dt / SPIN_TAU_S, sg = dot(ws, u) < 0 ? -1 : 1; /* the axis, turned to the side of u: up */
      for (let d = 0; d < 3; d++) u[d] += k * (sg * ws[d] / wn - u[d]);
    }
    u = unit(u);
    while (next < n && t[next] <= tS) {
      const uL = dot(u, L), uM = dot(u, M), uF = dot(u, F);
      bank[next] = Math.atan2(uL, uM) / DEG;
      pitch[next] = Math.atan2(uF, Math.hypot(uL, uM)) / DEG;
      next++;
    }
  });
  return { t, bank, pitch, axes };
}

/* bank and pitch at a time, interpolated; null outside the data */
export function attitudeAt(att, tSec) {
  if (!att) return null;
  const x = tSec / ATT_STEP_S, i = Math.floor(x);
  if (i < 0 || i >= att.t.length - 1) return null;
  const f = x - i;
  const b0 = att.bank[i], b1 = att.bank[i + 1];
  if (!Number.isFinite(b0) || !Number.isFinite(b1)) return null;
  let db = b1 - b0;
  if (db > 180) db -= 360;
  if (db < -180) db += 360;
  return { bank: b0 + db * f, pitch: att.pitch[i] + (att.pitch[i + 1] - att.pitch[i]) * f };
}
