/*
 * Per-second flight series for the flight-state detector, from an IGC track or from the analysis
 * context of a raw log, and an analysis context built from an IGC track so that the charts, the map
 * and the tone replay of the UI work on it. No DOM.
 */
import { slopeReference } from "./dsp.js";
import { detectFlightStates, FineManeuverDetector } from "./flightstate.js";

const G0 = 9.80665;
const M_PER_DEG = 111319;

function emptySeries(n) {
  return {
    n, t: new Float64Array(n), valid: new Uint8Array(n), lat: new Float64Array(n), lon: new Float64Array(n),
    alt: new Float64Array(n).fill(NaN), vz: new Float64Array(n).fill(NaN), ve: new Float64Array(n), vn: new Float64Array(n),
    accelG: new Float64Array(n).fill(NaN), hdop: new Float64Array(n).fill(NaN),
    motion: new Uint8Array(n), accMax: new Float64Array(n).fill(NaN), accMin: new Float64Array(n).fill(NaN),
    rateMax: new Float64Array(n).fill(NaN), source: "",
  };
}

/* least-squares slope of y over the points i-half..i+half (only finite ones) */
function slopeAt(t, y, i, half) {
  let st = 0, sy = 0, c = 0;
  for (let k = Math.max(0, i - half); k <= Math.min(y.length - 1, i + half); k++) if (Number.isFinite(y[k])) { st += t[k]; sy += y[k]; c++; }
  if (c < 3) return NaN;
  st /= c; sy /= c;
  let num = 0, den = 0;
  for (let k = Math.max(0, i - half); k <= Math.min(y.length - 1, i + half); k++) {
    if (!Number.isFinite(y[k])) continue;
    num += (t[k] - st) * (y[k] - sy); den += (t[k] - st) ** 2;
  }
  return den > 0 ? num / den : NaN;
}

/*
 * One entry per whole second from the first fix on. With the data records of UltraBeep
 * (LVBV) the GNSS velocity, the filtered altitude and climb rate and the acceleration come from
 * them; a plain IGC file gives the velocity from the positions and the climb rate from the
 * pressure altitude (whole metres: a ±2 s slope). An IGC file has no HDOP and no IMU peaks: the
 * fixes count as good, and of the landing rules only the calm one can apply.
 */
export function trackSeries(track) {
  let tEnd = 0; /* the latest B record: a broken one can carry an earlier time */
  for (let k = 0; k < track.n; k++) if (track.t[k] > tEnd) tEnd = track.t[k];
  const n = Math.floor(tEnd) + 1;
  const s = emptySeries(n);
  for (let i = 0; i < n; i++) s.t[i] = i;
  // the B record nearest each second
  const at = new Int32Array(n).fill(-1);
  for (let k = 0; k < track.n; k++) {
    const i = Math.round(track.t[k]);
    if (i >= 0 && i < n && (at[i] < 0 || Math.abs(track.t[k] - i) < Math.abs(track.t[at[i]] - i))) at[i] = k;
  }
  for (let i = 0; i < n; i++) {
    const k = at[i];
    if (k < 0) continue;
    s.lat[i] = track.lat[k]; s.lon[i] = track.lon[k];
    s.alt[i] = track.altPress[k];
    s.valid[i] = track.valid[k];
  }
  // velocity from the positions over 2 s, climb rate from the pressure altitude (±2 s slope)
  for (let i = 0; i < n; i++) {
    if (at[i] < 0) continue;
    const a = at[i - 1] >= 0 ? i - 1 : i, b = at[i + 1] >= 0 ? i + 1 : i;
    if (a === b) { s.valid[i] = 0; continue; }
    const dt = b - a;
    s.vn[i] = (s.lat[b] - s.lat[a]) * M_PER_DEG / dt;
    s.ve[i] = (s.lon[b] - s.lon[a]) * M_PER_DEG * Math.cos(s.lat[i] * Math.PI / 180) / dt;
    s.vz[i] = slopeAt(s.t, s.alt, i, 2);
  }
  s.source = "igc";
  const v = track.vbv;
  if (v && v.n) {
    // the sample nearest each second replaces the derived values
    const vat = new Int32Array(n).fill(-1);
    for (let k = 0; k < v.n; k++) {
      const i = Math.round(v.t[k]);
      if (i >= 0 && i < n && (vat[i] < 0 || Math.abs(v.t[k] - i) < Math.abs(v.t[vat[i]] - i))) vat[i] = k;
    }
    for (let i = 0; i < n; i++) {
      const k = vat[i];
      if (k < 0 || Math.abs(v.t[k] - i) > 0.6) continue;
      s.ve[i] = v.velE[k]; s.vn[i] = v.velN[k];
      s.alt[i] = v.alt[k]; s.vz[i] = v.vario[k];
      s.accelG[i] = v.accel[k] / G0;
      if (!(v.status[k] & 1)) s.valid[i] = 0;
    }
    s.source = "vbv";
  }
  return s;
}

/*
 * From a raw log's context, as the firmware's flight step sees it once a second: the first valid fix
 * of the second (the module's placeholder epochs at 10 Hz are skipped as on the device; without a
 * valid one the first epoch), the barometric altitude and the logged climb rate at that moment, the
 * IMU peaks since the previous step (sensors_take_motion) and the mean |a|/g of the second.
 */
export function contextSeries(ctx) {
  const fx = ctx.fixes;
  const tEnd = ctx.tS[ctx.tS.length - 1];
  const n = Math.floor(tEnd) + 1;
  const s = emptySeries(n);
  for (let i = 0; i < n; i++) s.t[i] = i;
  const good = (k) => fx.valid[k] && fx.fixType[k] === 3;
  const at = new Int32Array(n).fill(-1);
  for (let k = 0; k < fx.n; k++) {
    const i = Math.floor(ctx.fixT[k]);
    if (i >= 0 && i < n && (at[i] < 0 || (good(k) && !good(at[i])))) at[i] = k;
  }
  const imuT = ctx.imuT || [], imuAcc = ctx.imuAcc, imuGyro = ctx.imuGyro;
  let jb = 0, ja = 0, m = 0;
  for (let i = 0; i < n; i++) {
    const k = at[i];
    if (k >= 0) {
      s.lat[i] = fx.lat[k]; s.lon[i] = fx.lon[k];
      s.ve[i] = fx.ve[k]; s.vn[i] = fx.vn[k];
      s.valid[i] = good(k) ? 1 : 0;
      s.hdop[i] = fx.hdop[k];
    }
    const step = k >= 0 ? ctx.fixT[k] : i + 1;
    // barometer grid: altitude and climb rate at the step
    while (jb + 1 < ctx.tS.length && ctx.tS[jb + 1] <= step) jb++;
    if (ctx.tS[jb] <= step) { s.alt[i] = ctx.altIsa[jb]; s.vz[i] = ctx.loggedVz[jb]; }
    // mean |a|/g of the second
    let sa = 0, ca = 0;
    while (ja < ctx.tS.length && ctx.tS[ja] < i + 1) {
      if (Number.isFinite(ctx.accNorm[ja])) { sa += ctx.accNorm[ja]; ca++; }
      ja++;
    }
    s.accelG[i] = ca ? sa / ca / G0 : NaN;
    // IMU peaks since the previous step
    let aMax = 0, aMin = Infinity, wMax = 0, c = 0;
    for (; m < imuT.length && imuT[m] <= step; m++) {
      const g = imuAcc[m] / G0;
      if (g > aMax) aMax = g;
      if (g < aMin) aMin = g;
      if (imuGyro[m] > wMax) wMax = imuGyro[m];
      c++;
    }
    if (c) { s.motion[i] = 1; s.accMax[i] = aMax; s.accMin[i] = aMin; s.rateMax[i] = wMax; }
  }
  s.source = "raw";
  return s;
}

/*
 * Manoeuvres of a raw log at the device's rate: |a| and |ω| smoothed over 0.1 s at the IMU rate
 * (sensors.c FAST_ALPHA), the detector ticked every 50 ms (system.c update_maneuver) between the
 * takeoff and the landing, where the device starts it afresh and reads its peak load for the summary.
 * Returns the finished manoeuvres and that peak.
 */
export function fineManeuvers(ctx, t0 = -Infinity, t1 = Infinity) {
  const det = new FineManeuverDetector();
  const out = [];
  const FAST_ALPHA = 0.1, TURN_ALPHA = 0.02, TICK_S = 0.05;
  let fastA = 0, fastW = 0, turn = 0, started = false, lastTick = -Infinity;
  for (let k = 0; k < ctx.imuT.length; k++) {
    const g = ctx.imuAcc[k] / G0, w = ctx.imuGyro[k];
    if (!started) { fastA = g; started = true; } else fastA += FAST_ALPHA * (g - fastA);
    fastW += FAST_ALPHA * (w - fastW);
    if (ctx.imuTurn) turn += TURN_ALPHA * (ctx.imuTurn[k] - turn);
    const t = ctx.imuT[k];
    if (t < t0 || t > t1 || t - lastTick < TICK_S) continue;
    const dt = lastTick === -Infinity ? TICK_S : t - lastTick;
    lastTick = t;
    const done = det.update(t, fastA, fastW, dt, turn);
    if (done) out.push(done);
  }
  return { maneuvers: out, maxLoad: det.gMaxFlight };
}

/* The flight states of a context (raw log or track), on its per-second series. */
export function flightStatesOf(ctx) {
  if (ctx.kind === "track") {
    const series = trackSeries(ctx.track);
    return { series, states: detectFlightStates(series) };
  }
  const series = contextSeries(ctx);
  // first pass for the takeoff and the landing, then the manoeuvres at the device's rate in between
  const first = detectFlightStates(series);
  if (!(ctx.imuT && ctx.imuT.length) || first.summary.takeoffT == null) return { series, states: first };
  const fine = fineManeuvers(ctx, first.summary.takeoffT, first.summary.landingT ?? Infinity);
  const states = detectFlightStates(series, undefined, { maneuvers: fine.maneuvers, maxLoad: fine.maxLoad });
  return { series, states };
}

/*
 * A context for an IGC track with the fields the UI reads: the grid is the LVBV samples when the
 * file has them, else the B records; fixes from the same; no IMU streams.
 */
export function buildTrackContext(track, opts = {}) {
  const v = track.vbv;
  const useVbv = !!(v && v.n > 10);
  const nb = useVbv ? v.n : track.n;
  const tS = Float64Array.from(useVbv ? v.t : track.t);
  const altIsa = new Float64Array(nb);
  const loggedVz = new Float32Array(nb), loggedAlt = new Float32Array(nb);
  if (useVbv) {
    for (let i = 0; i < nb; i++) { altIsa[i] = v.baroAlt[i]; loggedVz[i] = v.vario[i]; loggedAlt[i] = v.alt[i]; }
  } else {
    for (let i = 0; i < nb; i++) { altIsa[i] = track.altPress[i]; loggedAlt[i] = track.altPress[i]; loggedVz[i] = slopeAt(track.t, track.altPress, i, 2); }
  }
  const nf = nb;
  const fixes = {
    n: nf, t: new Float64Array(nf), utc: new Float64Array(nf), lat: new Float64Array(nf), lon: new Float64Array(nf),
    altMsl: new Float32Array(nf), geoid: new Float32Array(nf), speed: new Float32Array(nf), course: new Float32Array(nf),
    vn: new Float32Array(nf), ve: new Float32Array(nf), vd: new Float32Array(nf), fixType: new Uint8Array(nf),
    sats: new Float32Array(nf).fill(NaN), hdop: new Float32Array(nf).fill(NaN), valid: new Uint8Array(nf),
  };
  const fixT = new Float64Array(nf);
  for (let i = 0; i < nf; i++) {
    fixT[i] = tS[i];
    fixes.t[i] = track.utc0 + tS[i] * 1000;
    fixes.utc[i] = fixes.t[i];
    if (useVbv) {
      fixes.lat[i] = v.lat[i]; fixes.lon[i] = v.lon[i]; fixes.altMsl[i] = v.hMsl[i]; fixes.geoid[i] = v.hEll[i] - v.hMsl[i];
      fixes.vn[i] = v.velN[i]; fixes.ve[i] = v.velE[i]; fixes.vd[i] = v.velD[i];
      fixes.speed[i] = Math.hypot(v.velN[i], v.velE[i]);
      let c = Math.atan2(v.velE[i], v.velN[i]) * 180 / Math.PI; if (c < 0) c += 360;
      fixes.course[i] = c;
      fixes.sats[i] = v.sats[i];
      fixes.valid[i] = v.status[i] & 1 ? 1 : 0;
      fixes.fixType[i] = fixes.valid[i] ? 3 : 1;
    } else {
      fixes.lat[i] = track.lat[i]; fixes.lon[i] = track.lon[i]; fixes.altMsl[i] = track.altGps[i];
      fixes.sats[i] = track.siu[i];
      fixes.valid[i] = track.valid[i];
      fixes.fixType[i] = track.valid[i] ? 3 : 1;
      const a = Math.max(0, i - 1), b = Math.min(nf - 1, i + 1), dt = track.t[b] - track.t[a];
      if (dt > 0) {
        fixes.vn[i] = (track.lat[b] - track.lat[a]) * M_PER_DEG / dt;
        fixes.ve[i] = (track.lon[b] - track.lon[a]) * M_PER_DEG * Math.cos(track.lat[i] * Math.PI / 180) / dt;
        fixes.vd[i] = -(track.altGps[b] - track.altGps[a]) / dt;
        fixes.speed[i] = Math.hypot(fixes.vn[i], fixes.ve[i]);
        let c = Math.atan2(fixes.ve[i], fixes.vn[i]) * 180 / Math.PI; if (c < 0) c += 360;
        fixes.course[i] = c;
      }
    }
  }
  // grid series the charts read
  const gnssClimb = new Float64Array(nb), gnssAlt = new Float64Array(nb), gnssSpeed = new Float64Array(nb), trackRate = new Float64Array(nb).fill(NaN);
  for (let i = 0; i < nb; i++) {
    gnssClimb[i] = fixes.valid[i] ? -fixes.vd[i] : NaN;
    gnssAlt[i] = fixes.valid[i] ? fixes.altMsl[i] : NaN;
    gnssSpeed[i] = fixes.valid[i] ? fixes.speed[i] : NaN;
    if (i > 0 && fixes.valid[i] && fixes.valid[i - 1] && fixes.speed[i] >= 2 && fixes.speed[i - 1] >= 2) {
      let d = fixes.course[i] - fixes.course[i - 1];
      while (d > 180) d -= 360;
      while (d < -180) d += 360;
      const dt = tS[i] - tS[i - 1];
      if (dt > 0 && dt < 2.5) trackRate[i] = d / dt;
    }
  }
  const nan = new Float32Array(nb).fill(NaN);
  const rate = nb > 1 ? Math.round((nb - 1) / (tS[nb - 1] - tS[0]) * 100) / 100 : 1;
  const events = track.markers.map((m) => ({ t: m.t, text: m.text }));
  const ctx = {
    kind: "track", track, log: null, name: track.name, t0: track.utc0, tS, altIsa,
    baroRate: rate, imuRate: 0,
    loggedVz, loggedAlt,
    accNorm: nan, gyroNorm: nan, gyroMax: nan, imuT: new Float64Array(0), imuAcc: new Float32Array(0), imuGyro: new Float32Array(0),
    nmea: null, fixes, fixT, fixRate: Math.round(rate),
    gnssClimb, gnssAlt, gnssSpeed, trackRate,
    events, messages: [],
    refHalf: opts.refHalf ?? 2,
    ref: null, refRobust: null,
    handlingMask: new Uint8Array(nb),
    utc: (tSec) => track.utc0 + tSec * 1000,
  };
  ctx.ref = slopeReference(tS, altIsa, ctx.refHalf);
  ctx.refRobust = ctx.ref;
  return ctx;
}
