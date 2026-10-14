/* Signal helpers for the analyzer: references, windows, statistics, spectra. No DOM. */

/*
 * Non-causal climb reference: least-squares slope of y over t within ±half seconds around each
 * sample (prefix sums, O(n)). Smooth and without lag; for comparing filters, not for the tone.
 */
export function slopeReference(tSec, y, half) {
  const n = tSec.length;
  const out = new Float64Array(n);
  const st = new Float64Array(n + 1), sy = new Float64Array(n + 1), stt = new Float64Array(n + 1),
    sty = new Float64Array(n + 1);
  const origin = n ? tSec[0] : 0;
  for (let i = 0; i < n; i++) {
    const t = tSec[i] - origin;
    st[i + 1] = st[i] + t;
    sy[i + 1] = sy[i] + y[i];
    stt[i + 1] = stt[i] + t * t;
    sty[i + 1] = sty[i] + t * y[i];
  }
  let lo = 0, hi = 0;
  for (let i = 0; i < n; i++) {
    while (tSec[lo] < tSec[i] - half) lo++;
    while (hi < n && tSec[hi] <= tSec[i] + half) hi++;
    const m = hi - lo;
    if (m < 3) {
      out[i] = NaN;
      continue;
    }
    const a = st[hi] - st[lo], b = sy[hi] - sy[lo];
    const c = stt[hi] - stt[lo], d = sty[hi] - sty[lo];
    const den = m * c - a * a;
    out[i] = den > 1e-12 ? (m * d - a * b) / den : NaN;
  }
  return out;
}

/*
 * Centred mean of y over one rotation period per sample (periodSec[i], 0 = no averaging): a
 * boxcar over exactly one period removes a modulation synchronous with the rotation without lag
 * (a least-squares slope over whole periods would not: its end weights keep 3A/(ωT²) of it).
 */
export function rotationMean(tSec, y, periodSec) {
  const n = tSec.length;
  const out = new Float64Array(n);
  const sy = new Float64Array(n + 1);
  for (let i = 0; i < n; i++) sy[i + 1] = sy[i] + y[i];
  const lower = (x) => { let lo = 0, hi = n; while (lo < hi) { const m = (lo + hi) >> 1; if (tSec[m] < x) lo = m + 1; else hi = m; } return lo; };
  for (let i = 0; i < n; i++) {
    const period = periodSec[i];
    if (!(period > 0)) {
      out[i] = y[i];
      continue;
    }
    const lo = lower(tSec[i] - period / 2), hi = lower(tSec[i] + period / 2); /* [t - T/2, t + T/2) */
    out[i] = hi > lo ? (sy[hi] - sy[lo]) / (hi - lo) : y[i];
  }
  return out;
}

/* Centered moving mean over w samples (odd), NaN-aware. */
export function movingMean(x, w) {
  const n = x.length;
  const out = new Float64Array(n);
  const half = w >> 1;
  let s = 0, c = 0;
  for (let i = 0; i < Math.min(n, half); i++) if (Number.isFinite(x[i])) { s += x[i]; c++; }
  for (let i = 0; i < n; i++) {
    const add = i + half, rem = i - half - 1;
    if (add < n && Number.isFinite(x[add])) { s += x[add]; c++; }
    if (rem >= 0 && Number.isFinite(x[rem])) { s -= x[rem]; c--; }
    out[i] = c ? s / c : NaN;
  }
  return out;
}

/* Linear interpolation of (tSrc, ySrc) at tDst; NaN where the nearest source samples are more than maxGap apart. */
export function interpolate(tSrc, ySrc, tDst, maxGap = Infinity) {
  const out = new Float64Array(tDst.length).fill(NaN);
  let j = 0;
  for (let i = 0; i < tDst.length; i++) {
    const t = tDst[i];
    while (j + 1 < tSrc.length && tSrc[j + 1] <= t) j++;
    if (j + 1 >= tSrc.length || tSrc[j] > t) continue;
    const t0 = tSrc[j], t1 = tSrc[j + 1];
    if (t1 - t0 > maxGap) continue;
    const y0 = ySrc[j], y1 = ySrc[j + 1];
    if (!Number.isFinite(y0) || !Number.isFinite(y1)) continue;
    out[i] = t1 > t0 ? y0 + (y1 - y0) * (t - t0) / (t1 - t0) : y0;
  }
  return out;
}

export function finite(x) {
  const out = [];
  for (const v of x) if (Number.isFinite(v)) out.push(v);
  return out;
}

export function mean(x) {
  let s = 0, c = 0;
  for (const v of x) if (Number.isFinite(v)) { s += v; c++; }
  return c ? s / c : NaN;
}

export function rms(x) {
  let s = 0, c = 0;
  for (const v of x) if (Number.isFinite(v)) { s += v * v; c++; }
  return c ? Math.sqrt(s / c) : NaN;
}

export function std(x) {
  const m = mean(x);
  let s = 0, c = 0;
  for (const v of x) if (Number.isFinite(v)) { s += (v - m) ** 2; c++; }
  return c > 1 ? Math.sqrt(s / (c - 1)) : NaN;
}

export function percentile(x, p) {
  const a = finite(x).sort((u, v) => u - v);
  if (!a.length) return NaN;
  const k = Math.min(a.length - 1, Math.max(0, Math.round((p / 100) * (a.length - 1))));
  return a[k];
}

/* Index range [i0, i1) of the sorted times t within [t0, t1]. */
export function indexRange(t, t0, t1) {
  let lo = 0, hi = t.length;
  while (lo < hi) { const m = (lo + hi) >> 1; if (t[m] < t0) lo = m + 1; else hi = m; }
  const i0 = lo;
  hi = t.length;
  while (lo < hi) { const m = (lo + hi) >> 1; if (t[m] <= t1) lo = m + 1; else hi = m; }
  return [i0, lo];
}

/*
 * Delay of a behind b (seconds, positive = a lags), by the maximum correlation of a(t) with
 * b(t - lag) for lags 0..maxLag on a regular grid of step dt. Both arrays on the same grid.
 */
export function lagByCorrelation(a, b, dt, maxLag) {
  const n = a.length;
  const maxK = Math.round(maxLag / dt);
  let best = -Infinity, bestK = 0;
  const corrAt = (k) => {
    let sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, c = 0;
    for (let i = k; i < n; i++) {
      const x = a[i], y = b[i - k];
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y; c++;
    }
    if (c < 10) return -Infinity;
    const cov = sab - sa * sb / c, va = saa - sa * sa / c, vb = sbb - sb * sb / c;
    return va > 0 && vb > 0 ? cov / Math.sqrt(va * vb) : -Infinity;
  };
  for (let k = 0; k <= maxK; k++) {
    const r = corrAt(k);
    if (r > best) { best = r; bestK = k; }
  }
  // parabolic refinement around the peak
  let lag = bestK * dt;
  if (bestK > 0 && bestK < maxK) {
    const r0 = corrAt(bestK - 1), r2 = corrAt(bestK + 1);
    const den = r0 - 2 * best + r2;
    if (den < 0) lag += 0.5 * (r0 - r2) / den * dt;
  }
  return { lag, corr: best };
}

/* In-place radix-2 FFT (re, im of length 2^k). */
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b = a + len / 2;
        const tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

/* Welch amplitude spectral density (unit/√Hz, mean removed per segment, Hann window). Returns {f, amp}. */
export function spectrum(x, fs, segment = 1024) {
  const n = 1 << Math.round(Math.log2(Math.max(64, Math.min(segment, x.length))));
  const win = new Float64Array(n);
  let wsum = 0;
  for (let i = 0; i < n; i++) { win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1)); wsum += win[i] ** 2; }
  const acc = new Float64Array(n / 2 + 1);
  let segs = 0;
  for (let s = 0; s + n <= x.length; s += n >> 1) {
    let m = 0, c = 0;
    for (let i = 0; i < n; i++) if (Number.isFinite(x[s + i])) { m += x[s + i]; c++; }
    if (c < n * 0.9) continue;
    m /= c;
    const re = new Float64Array(n), im = new Float64Array(n);
    for (let i = 0; i < n; i++) re[i] = (Number.isFinite(x[s + i]) ? x[s + i] - m : 0) * win[i];
    fft(re, im);
    for (let k = 0; k <= n / 2; k++) acc[k] += (re[k] ** 2 + im[k] ** 2);
    segs++;
  }
  const f = new Float64Array(n / 2 + 1), amp = new Float64Array(n / 2 + 1);
  for (let k = 0; k <= n / 2; k++) {
    f[k] = k * fs / n;
    amp[k] = segs ? Math.sqrt(2 * acc[k] / segs / (fs * wsum)) : NaN;
  }
  return { f, amp, segments: segs };
}
