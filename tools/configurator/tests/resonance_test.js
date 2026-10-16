"use strict";

/*
 * Self-test of js/resonance.js under Node: a synthetic sweep recording of a piezo with resonances at 3.2 and
 * 9 kHz, driven by a square wave, with the Bluetooth delay varying per tone and one tone missing.
 *   node tools/configurator/tests/resonance_test.js
 */
const path = require("node:path");
const { RES, resonanceSteps, toneAmplitude, aWeightDb, findPeaks, resonanceFundamentals, analyzeSweep, wavBytes } =
  require(path.join(__dirname, "..", "js", "resonance.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);

/* basics */
const rate = 48000;
const sine = new Float32Array(rate);
for (let i = 0; i < sine.length; i++) sine[i] = 0.5 * Math.sin((2 * Math.PI * 1234 * i) / rate + 0.3);
const a = toneAmplitude(sine, 1000, 7680, 1234, rate);
check(Math.abs(a - 0.5) < 0.005, `amplitude of a 0.5 sine: ${a.toFixed(4)}`);
check(Math.abs(aWeightDb(1000)) < 0.1 && Math.abs(aWeightDb(100) + 19.1) < 0.2,
  `A-weighting 1 kHz ${aWeightDb(1000).toFixed(2)} dB, 100 Hz ${aWeightDb(100).toFixed(2)} dB`);
const steps12 = resonanceSteps(12), steps24 = resonanceSteps(24);
check(steps12.length === 56 && steps12[0] === 200 && steps12[55] <= 5000 && steps24.length === 112,
  `sweep steps: ${steps12.length} (${steps12[0]}..${steps12[55]} Hz), fine ${steps24.length}`);
const fs = resonanceFundamentals(3200);
check(fs.map((s) => s.f).join(",") === "1067,640,457", `fundamentals for 3.2 kHz: ${fs.map((s) => s.f).join(", ")}`);
const pk = findPeaks([0, 1, 5, 1, 0, 2, 3, 2, 0].map((db, i) => ({ f: 1000 + i * 100, db })), 2);
check(pk.length === 2 && pk[0].f === 1200 && pk[1].f === 1600 && pk[1].prom === 3,
  `peaks with prominence: ${pk.map((p) => `${p.f}/${p.prom}`).join(" ")}`);

/* the piezo: rising by 12 dB per octave up to its resonance at 3.2 kHz (Q 6), falling above, and the
 * housing's resonance at 9 kHz (Q 10) */
const reso = (f, fr, q) => 1 / Math.sqrt((1 - (f / fr) ** 2) ** 2 + (f / (fr * q)) ** 2);
const H = (f) => ((f / 3200) ** 2 / (1 + (f / 3200) ** 2)) * reso(f, 3200, 6) + 0.15 * reso(f, 9000, 10);
const freqs = resonanceSteps(12);
const missing = freqs[20]; /* the device stays silent at this step */
const toneLen = Math.round((RES.toneMs / 1000) * rate);
const harmonics = (f) => {
  const out = [];
  for (let n = 1; n <= 25 && n * f < 23000; n += 2) out.push({ n, amp: H(n * f) / n, ph: rnd() * 6.28 });
  return out;
};
let peak = 0;
for (const f of freqs) peak = Math.max(peak, harmonics(f).reduce((s, h) => s + h.amp, 0));
const scale = 0.5 / peak;
const stepLen = Math.round(((RES.toneMs + RES.gapMs) / 1000 + 0.06) * rate); /* plus the command's round trip */
const total = Math.round(RES.noiseS * rate) + freqs.length * stepLen + rate;
const x = new Float32Array(total);
for (let i = 0; i < total; i++) x[i] = (rnd() - 0.5) * 2e-4; /* background around -80 dB */
const marks = [];
let t = Math.round(RES.noiseS * rate);
for (const f of freqs) {
  marks.push(t);
  const start = t + Math.round((0.03 + 0.4 * rnd()) * rate); /* Bluetooth and audio input delay (a phone: up to 0.4 s) */
  if (f !== missing) {
    for (const h of harmonics(f)) {
      const w = (2 * Math.PI * h.n * f) / rate, amp = h.amp * scale;
      for (let i = 0; i < toneLen; i++) x[start + i] += amp * Math.sin(w * i + h.ph);
    }
  }
  t += stepLen;
}

const t0 = Date.now();
const r = analyzeSweep(x, rate, marks, freqs);
console.log(`     analysis of ${(total / rate).toFixed(0)} s: ${Date.now() - t0} ms`);
check(r.steps.length === freqs.length && r.steps.every(Boolean), `every step analysed (${r.steps.length})`);
check(r.peaks.length >= 2 && Math.abs(r.peaks[0].f / 3200 - 1) < 0.05,
  `main resonance: ${r.peaks.map((p) => `${p.f} Hz (${p.prom.toFixed(1)} dB)`).join(", ")}`);
check(r.peaks.some((p) => Math.abs(p.f / 9000 - 1) < 0.06), "the 9 kHz resonance from the harmonics");
check(r.harmonicOffsetDb !== null && Math.abs(r.harmonicOffsetDb) < 1.5 && r.harmonicPairs > 10,
  `harmonics agree with the fundamentals: ${r.harmonicOffsetDb && r.harmonicOffsetDb.toFixed(2)} dB over ${r.harmonicPairs} bins`);
/* the response follows H within a few dB where measured (relative to its value at 2 kHz) */
const at = (f) => r.curve.reduce((b, p) => (Math.abs(Math.log(p.f / f)) < Math.abs(Math.log(b.f / f)) ? p : b));
const rel = (f) => at(f).db - at(2000).db, relH = (f) => 20 * Math.log10(H(at(f).f) / H(at(2000).f));
check([500, 1000, 3200, 6000, 9000].every((f) => Math.abs(rel(f) - relH(f)) < 2.5),
  `response vs. the model: ${[500, 1000, 3200, 6000, 9000].map((f) => `${f}: ${rel(f).toFixed(1)}/${relH(f).toFixed(1)}`).join(", ")}`);
check(!r.points.some((p) => p.n === 1 && p.f === missing), `the silent step ${missing} Hz gives no point`);
check(r.loudest.length > 0 && r.loudest.some((p) => [1067, 640, 457].some((f) => Math.abs(p.f / f - 1) < 0.06)),
  `loudest tones: ${r.loudest.map((p) => p.f).join(", ")} Hz`);
check(r.clippedShare === 0 && r.bestSnrDb > 40, `no clipping, best SNR ${r.bestSnrDb.toFixed(0)} dB`);
check(r.bandHz === null && r.usableHz === RES.respMaxHz, "a full-band microphone: no band limit");

/* the same through a Bluetooth headset in hands-free mode: nothing above 8 kHz, the background white noise through
 * a steep low-pass at 7.2 kHz (2026-10-16: the response showed peaks at 9.6 and 11 kHz that were none) */
const xb = new Float32Array(total);
const TAPS = 301, fc = 7200 / rate;
const fir = Array.from({ length: TAPS }, (_, k) => {
  const m = k - (TAPS - 1) / 2, sinc = m === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * m) / (Math.PI * m);
  return sinc * (0.42 - 0.5 * Math.cos((2 * Math.PI * k) / (TAPS - 1)) + 0.08 * Math.cos((4 * Math.PI * k) / (TAPS - 1)));
});
const white = Float32Array.from({ length: total + TAPS }, () => (rnd() - 0.5) * 4e-4);
for (let i = 0; i < total; i++) { let v = 0; for (let k = 0; k < TAPS; k++) v += fir[k] * white[i + k]; xb[i] = v; }
t = Math.round(RES.noiseS * rate);
for (const f of freqs) {
  const start = t + Math.round((0.03 + 0.2 * rnd()) * rate);
  for (const h of harmonics(f)) {
    if (h.n * f >= 7900) continue;
    const w = (2 * Math.PI * h.n * f) / rate, amp = h.amp * scale;
    for (let i = 0; i < toneLen; i++) xb[start + i] += amp * Math.sin(w * i + h.ph);
  }
  t += stepLen;
}
const rb = analyzeSweep(xb, rate, marks, freqs);
check(rb.bandHz > 7000 && rb.bandHz < 8800 && rb.usableHz < 8000 && rb.points.every((q) => q.f <= rb.usableHz),
  `band-limited microphone found: ${rb.bandHz} Hz, response to ${Math.round(rb.usableHz)} Hz`);
check(Math.abs(rb.peaks[0].f / 3200 - 1) < 0.05 && rb.peaks.every((q) => q.f < 8000),
  `band-limited: peaks ${rb.peaks.map((q) => q.f).join(", ")} Hz`);

const wav = wavBytes(x.subarray(0, 1000), rate);
const dv = new DataView(wav.buffer);
check(String.fromCharCode(...wav.slice(0, 4)) === "RIFF" && dv.getUint32(24, true) === rate && wav.length === 44 + 2000,
  `WAV header (${wav.length} bytes)`);

console.log(failed ? `${failed} FAILED` : "all passed");
process.exitCode = failed ? 1 : 0;
