/*
 * Figures of merit of a climb trace against references, for tuning and for comparing algorithms.
 * There is no ground truth in a log, so the metrics use:
 *   ref   non-causal least-squares slope of the barometric altitude (±refHalf s): no lag, smooth
 *   gnss  GNSS vertical speed (ellipsoid height over about 1 s): independent of the barometer, noisy
 * Lag = the shift with the best correlation against ref. The rest is measured after removing
 * that lag (noise) or as the listener hears it (false lift, tone switches).
 */
import { indexRange, lagByCorrelation, mean, movingMean, rms, percentile } from "./dsp.js";
import { DEFAULT_CURVE, toneStats } from "./tones.js";

export const TURN_BINS = [[0, 5], [5, 15], [15, 30], [30, 60], [60, Infinity]];      // °/s (gyro magnitude)
export const SPEED_BINS = [[0, 1], [1, 4], [4, 8], [8, 13], [13, Infinity]];          // m/s GNSS ground speed
export const ACC_BINS = [[0, 0.3], [0.3, 1], [1, 2], [2, 4], [4, Infinity]];          // m/s², | |a| - g |

function bins(edges, key, err, i0, i1) {
  return edges.map(([lo, hi]) => {
    let s = 0, s2 = 0, c = 0;
    for (let i = i0; i < i1; i++) {
      const k = key[i], e = err[i];
      if (!Number.isFinite(k) || !Number.isFinite(e) || k < lo || k >= hi) continue;
      s += e; s2 += e * e; c++;
    }
    return { lo, hi, n: c, mean: c ? s / c : NaN, rms: c ? Math.sqrt(s2 / c) : NaN };
  });
}

/*
 * opts: t0, t1 (s, default whole log), lift (m/s, default from the curve), margin (m/s),
 * curve (tone curve), maxLag (s)
 */
export function computeMetrics(ctx, run, opts = {}) {
  const t0 = opts.t0 ?? -Infinity, t1 = opts.t1 ?? Infinity;
  const [i0, i1] = indexRange(ctx.tS, t0, t1);
  const curve = opts.curve || DEFAULT_CURVE;
  const lift = opts.lift ?? curve.lift / 100;
  const margin = opts.margin ?? 0.2;
  const dt = 1 / ctx.baroRate;
  const n = i1 - i0;
  if (n < ctx.baroRate * 5) return { label: run.label, n, tooShort: true };
  const vz = run.vz.subarray(i0, i1);
  // the reference is unreliable around handling (pressure pulses): those windows do not count
  const ref = Float64Array.from(ctx.ref.subarray(i0, i1));
  const mask = ctx.handlingMask.subarray(i0, i1);
  let masked = 0;
  for (let i = 0; i < n; i++) if (mask[i]) { ref[i] = NaN; masked++; }

  const { lag, corr } = lagByCorrelation(vz, ref, dt, opts.maxLag ?? 3);
  const k = Math.round(lag / dt);
  const err = new Float64Array(n), errAligned = new Float64Array(n).fill(NaN);
  for (let i = 0; i < n; i++) {
    err[i] = vz[i] - ref[i];
    if (i - k >= 0) errAligned[i] = vz[i] - ref[i - k];
  }
  const smooth = movingMean(vz, Math.round(ctx.baroRate) | 1);
  const hf = new Float64Array(n);
  for (let i = 0; i < n; i++) hf[i] = vz[i] - smooth[i];

  let falseLift = 0, missedLift = 0, liftRef = 0, quiet = 0;
  for (let i = 0; i < n; i++) {
    const r = ref[i];
    if (!Number.isFinite(r)) continue;
    if (r < lift - margin) { quiet++; if (vz[i] >= lift) falseLift++; }
    if (r >= lift + margin) { liftRef++; if (vz[i] < lift) missedLift++; }
  }

  // GNSS: both averaged over 1 s (the GNSS vertical speed is noisy)
  const g = ctx.gnssClimb.subarray(i0, i1);
  const gErr = new Float64Array(n).fill(NaN);
  const vzS = movingMean(vz, Math.round(ctx.baroRate) | 1);
  for (let i = 0; i < n; i++) if (Number.isFinite(g[i])) gErr[i] = vzS[i] - g[i];

  // handling: largest deviation within 3 s after taps and button presses, against a reference
  // that ignores short pressure pulses (moving median, ±10 s slope)
  const handling = [];
  for (const e of ctx.events) {
    if (e.t < t0 || e.t > t1 || !(e.text.startsWith("tap ") || e.text.startsWith("button down"))) continue;
    const [a0, a1] = indexRange(ctx.tS, e.t, e.t + 3);
    let m = 0;
    for (let i = a0; i < a1; i++) {
      const d = Math.abs(run.vz[i] - ctx.refRobust[i]);
      if (Number.isFinite(d) && d > m) m = d;
    }
    handling.push(m);
  }

  const absAcc = new Float64Array(n);
  for (let i = 0; i < n; i++) absAcc[i] = Math.abs(ctx.accNorm[i0 + i] - 9.80665);

  const tone = toneStats(curve, ctx.tS, run.vz, i0, i1);
  return {
    label: run.label,
    seconds: n * dt,
    excludedSeconds: masked * dt,
    lag, corr,
    rmsRef: rms(err),
    rmsAligned: rms(errAligned),
    bias: mean(err),
    p95Aligned: percentile(Array.from(errAligned, Math.abs), 95),
    noiseHF: rms(hf),
    falseLiftPct: quiet ? 100 * falseLift / quiet : NaN,
    missedLiftPct: liftRef ? 100 * missedLift / liftRef : NaN,
    rmsGnss: rms(gErr),
    biasGnss: mean(gErr),
    toneLiftPct: 100 * tone.liftShare,
    toneSwitchesPerMin: tone.switchesPerMin,
    handlingMean: handling.length ? mean(handling) : NaN,
    handlingMax: handling.length ? Math.max(...handling) : NaN,
    handlingCount: handling.length,
    turnBins: bins(TURN_BINS, ctx.gyroNorm.subarray(i0, i1), errAligned, 0, n),
    speedBins: bins(SPEED_BINS, ctx.gnssSpeed.subarray(i0, i1), errAligned, 0, n),
    accBins: bins(ACC_BINS, absAcc, errAligned, 0, n),
  };
}

export function computeAll(ctx, runs, opts = {}) {
  return runs.map((r) => computeMetrics(ctx, r, opts));
}

/*
 * One number to minimise in parameter sweeps (lower is better): lag and noise in m/s-equivalents
 * plus false lift. Weights can be passed; the defaults favour a fast, quiet tone.
 */
export function score(m, w = { lag: 1.0, rmsAligned: 2.0, noiseHF: 2.0, falseLift: 0.05, handling: 0.2 }) {
  if (!m || m.tooShort) return NaN;
  return w.lag * m.lag + w.rmsAligned * m.rmsAligned + w.noiseHF * m.noiseHF +
    w.falseLift * (m.falseLiftPct || 0) + w.handling * (Number.isFinite(m.handlingMean) ? m.handlingMean : 0);
}
