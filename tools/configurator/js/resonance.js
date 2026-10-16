"use strict";

/*
 * Piezo resonance measurement (Status and tests). The device plays a stepped sweep with the TONE test
 * command, the browser records it with a microphone and measures, per step, the level of the tone's
 * fundamental and of its odd harmonics.
 *
 * The H-bridge drives the piezo with a symmetric square wave (the piezo holds its charge in the dead time),
 * whose n-th harmonic has 1/n of the fundamental's amplitude: the level at n*f plus 20*log10(n) is the
 * response of piezo and housing at n*f, so the curve reaches beyond the 5 kHz the device plays. Two results:
 * where piezo and housing resonate (a tone whose 3rd, 5th or 7th harmonic falls there sounds loud and full),
 * and how loud each tone is as heard (all harmonics, A-weighted).
 *
 * The analysis is pure (tests/resonance_test.js); the view part at the bottom needs the browser.
 */

const RES = {
  f0: 200, f1: 5000,   /* the device plays 10..5000 Hz (firmware piezo_start) */
  toneMs: 250,         /* each step */
  gapMs: 300,          /* silence after it */
  slackS: 0.3,         /* a tone starts up to this long after its command was sent (Bluetooth, audio input); <= gap */
  windowS: 0.16,       /* analysis window inside a tone */
  hopS: 0.01,
  noiseS: 0.8,         /* silence recorded before the first tone */
  maxHarmonic: 15,     /* heard loudness: odd harmonics up to this */
  responseHarmonics: [1, 3, 5, 7],
  minSnrDb: 6,         /* response points need this much above the noise */
  binsPerOctave: 24,
  respMaxHz: 16000,
};

/* The sweep's frequencies: perOctave steps per octave from f0 to f1. */
function resonanceSteps(perOctave = 12, f0 = RES.f0, f1 = RES.f1) {
  const n = Math.floor(Math.log2(f1 / f0) * perOctave + 1e-9);
  const out = [];
  for (let i = 0; i <= n; i++) out.push(Math.round(f0 * 2 ** (i / perOctave)));
  return out;
}

const hannCache = new Map();
function hann(len) {
  let w = hannCache.get(len);
  if (!w) {
    w = { v: new Float32Array(len), sum: 0 };
    for (let i = 0; i < len; i++) {
      w.v[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (len - 1));
      w.sum += w.v[i];
    }
    hannCache.set(len, w);
  }
  return w;
}

/* Amplitude of the sine at freq in x[start, start + len) (Goertzel, Hann window; 1 = a full-scale sine). */
function toneAmplitude(x, start, len, freq, rate) {
  const w = hann(len);
  const k = (2 * Math.PI * freq) / rate;
  const c = 2 * Math.cos(k);
  let s1 = 0, s2 = 0;
  for (let i = 0; i < len; i++) {
    const s0 = x[start + i] * w.v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const re = s1 - s2 * Math.cos(k), im = s2 * Math.sin(k);
  return (2 * Math.sqrt(re * re + im * im)) / w.sum;
}

const dbOf = (a) => 20 * Math.log10(Math.max(a, 1e-9));

/* A-weighting in dB (IEC 61672). */
function aWeightDb(f) {
  const f2 = f * f;
  const ra = (12194 ** 2 * f2 * f2) /
    ((f2 + 20.6 ** 2) * Math.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194 ** 2));
  return 20 * Math.log10(ra) + 2.0;
}

/* Local maxima of points [{f, db}] standing at least minProm dB above the lower of their two sides
 * (each side: its lowest point before a higher one or the end), highest first. */
function findPeaks(points, minProm, fLo = 0, fHi = Infinity) {
  const v = points.map((p) => p.db);
  const out = [];
  for (let i = 0; i < v.length; i++) {
    if ((i > 0 && v[i - 1] >= v[i]) || (i < v.length - 1 && v[i + 1] > v[i])) continue;
    let left = v[i];
    for (let j = i - 1; j >= 0 && v[j] <= v[i]; j--) left = Math.min(left, v[j]);
    let right = v[i];
    for (let j = i + 1; j < v.length && v[j] <= v[i]; j++) right = Math.min(right, v[j]);
    const prom = v[i] - Math.max(left, right);
    if (prom >= minProm && points[i].f >= fLo && points[i].f <= fHi) out.push({ f: points[i].f, db: v[i], prom });
  }
  return out.sort((a, b) => b.db - a.db);
}

/* Fundamentals whose 3rd, 5th or 7th harmonic hits a resonance at fr, in the range of vario tones. */
function resonanceFundamentals(fr, lo = 300, hi = 1500) {
  return [3, 5, 7].map((n) => ({ n, f: Math.round(fr / n) })).filter((s) => s.f >= lo && s.f <= hi);
}

/*
 * The recording x (mono, rate) with marks[i] = the sample count when the command of freqs[i] was sent.
 * Returns per step the levels of its odd harmonics, the response of piezo and housing (1/binsPerOctave
 * bins, 0 dB = its maximum), the loudness of each tone as heard (0 dB = the loudest), peaks and checks.
 */
function analyzeSweep(x, rate, marks, freqs, opt = {}) {
  const o = { ...RES, ...opt };
  const W = Math.round(o.windowS * rate), hop = Math.max(1, Math.round(o.hopS * rate));
  const nyq = 0.95 * (rate / 2);
  const regionLen = Math.round((o.toneMs / 1000 + o.slackS) * rate);

  /* noise: the silence before the first tone, up to 4 windows */
  const quietEnd = Math.min(marks.length ? marks[0] : x.length, x.length);
  const quietStart = Math.round(0.1 * rate);
  const quietWins = [];
  for (let p = quietStart; p + W <= quietEnd && quietWins.length < 4; p += W) quietWins.push(p);
  const noiseAmp = (f) => {
    if (!quietWins.length) return 0;
    let pw = 0;
    for (const p of quietWins) pw += toneAmplitude(x, p, W, f, rate) ** 2;
    return Math.sqrt(pw / quietWins.length);
  };

  let clipped = 0, inTones = 0;
  const steps = freqs.map((f, i) => {
    const s0 = marks[i], s1 = Math.min(x.length, s0 + regionLen);
    if (!(s1 - s0 >= W)) return null;
    const hs = [];
    for (let n = 1; n <= o.maxHarmonic && n * f < nyq; n += 2) hs.push(n);
    const locate = hs.slice(0, 4);
    let best = -1, pos = s0;
    for (let p = s0; p + W <= s1; p += hop) {
      let pw = 0;
      for (const n of locate) pw += toneAmplitude(x, p, W, n * f, rate) ** 2;
      if (pw > best) { best = pw; pos = p; }
    }
    for (let k = pos; k < pos + W; k++) {
      if (Math.abs(x[k]) > 0.98) clipped++;
    }
    inTones += W;
    let heard = 0;
    const levels = hs.map((n) => {
      const a = toneAmplitude(x, pos, W, n * f, rate), na = noiseAmp(n * f);
      heard += Math.max(a * a - na * na, 0) * 10 ** (aWeightDb(n * f) / 10);
      return { n, f: n * f, db: dbOf(a), noiseDb: dbOf(na) };
    });
    return { f, pos, levels, heardDb: 10 * Math.log10(Math.max(heard, 1e-18)) };
  });

  /* the response: harmonic n at n*f, plus 20*log10(n) for the square wave's 1/n */
  const points = [];
  for (const s of steps) {
    if (!s) continue;
    for (const l of s.levels) {
      if (!o.responseHarmonics.includes(l.n) || l.f > o.respMaxHz || l.db - l.noiseDb < o.minSnrDb) continue;
      points.push({ f: l.f, db: l.db + 20 * Math.log10(l.n), n: l.n });
    }
  }
  const binOf = (f) => Math.round(Math.log2(f / o.f0) * o.binsPerOctave);
  const bins = new Map();
  for (const p of points) {
    const b = binOf(p.f);
    if (!bins.has(b)) bins.set(b, []);
    bins.get(b).push(p);
  }
  const keys = [...bins.keys()].sort((a, b) => a - b);
  let curve = keys.map((b) => {
    const ps = bins.get(b);
    return { f: Math.round(o.f0 * 2 ** (b / o.binsPerOctave)), db: ps.reduce((s, p) => s + p.db, 0) / ps.length };
  });
  /* light smoothing over neighbouring bins that are there */
  curve = curve.map((p, i) => {
    const near = curve.filter((q, j) => Math.abs(j - i) <= 1 && Math.abs(binOf(q.f) - binOf(p.f)) <= 1);
    return { f: p.f, db: near.reduce((s, q) => s + q.db, 0) / near.length };
  });
  const top = curve.length ? Math.max(...curve.map((p) => p.db)) : 0;
  curve = curve.map((p) => ({ f: p.f, db: p.db - top }));
  const normPoints = points.map((p) => ({ ...p, db: p.db - top }));

  /* do the harmonics agree with the fundamentals where both are measured (the 1/n assumption)? */
  const diffs = [];
  for (const b of keys) {
    const ps = bins.get(b);
    const one = ps.filter((p) => p.n === 1), more = ps.filter((p) => p.n > 1);
    if (one.length && more.length) {
      diffs.push(more.reduce((s, p) => s + p.db, 0) / more.length - one.reduce((s, p) => s + p.db, 0) / one.length);
    }
  }
  diffs.sort((a, b) => a - b);
  const harmonicOffsetDb = diffs.length ? diffs[Math.floor(diffs.length / 2)] : null;

  const valid = steps.filter(Boolean);
  const heardTop = valid.length ? Math.max(...valid.map((s) => s.heardDb)) : 0;
  const heard = valid.map((s) => ({ f: s.f, db: s.heardDb - heardTop }));
  const snr = valid.map((s) => s.levels.reduce((m, l) => Math.max(m, l.db - l.noiseDb), -Infinity));
  return {
    rate, freqs, marks, steps, points: normPoints, curve, heard,
    peaks: findPeaks(curve, 6, 500, 12000).slice(0, 3),
    loudest: findPeaks(heard, 2, 300, 1500).slice(0, 3),
    harmonicOffsetDb, harmonicPairs: diffs.length,
    clippedShare: inTones ? clipped / inTones : 0,
    bestSnrDb: snr.length ? Math.max(...snr) : 0,
  };
}

/* 16-bit PCM WAV of a mono recording. */
function wavBytes(x, rate) {
  const n = x.length, buf = new ArrayBuffer(44 + 2 * n), v = new DataView(buf);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, "RIFF"); v.setUint32(4, 36 + 2 * n, true); str(8, "WAVE");
  str(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, rate, true); v.setUint32(28, rate * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  str(36, "data"); v.setUint32(40, 2 * n, true);
  for (let i = 0; i < n; i++) {
    const s = Math.max(-1, Math.min(1, x[i]));
    v.setInt16(44 + 2 * i, s < 0 ? s * 32768 : s * 32767, true);
  }
  return new Uint8Array(buf);
}

if (typeof module !== "undefined") {
  module.exports = { RES, resonanceSteps, toneAmplitude, aWeightDb, findPeaks, resonanceFundamentals, analyzeSweep, wavBytes };
}

/* ---------------------------------------------------------------- view (browser) */

let resonanceRun = null; /* the running measurement: { abort, rec } */

/* Ends a running measurement and frees the microphone (leaving the view). */
function resonanceStop() {
  if (resonanceRun) resonanceRun.abort = true;
}

class MicRecorder {
  async start() {
    this.stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false,
      noiseSuppression: false, autoGainControl: false, channelCount: 1 } });
    this.ctx = new AudioContext();
    await this.ctx.resume();
    this.rate = this.ctx.sampleRate;
    this.chunks = [];
    this.count = 0;
    this.src = this.ctx.createMediaStreamSource(this.stream);
    /* ScriptProcessor: no worklet module to load; it only collects the samples */
    this.proc = this.ctx.createScriptProcessor(2048, 1, 1);
    this.proc.onaudioprocess = (e) => {
      const d = e.inputBuffer.getChannelData(0);
      this.chunks.push(new Float32Array(d));
      this.count += d.length;
    };
    this.src.connect(this.proc);
    this.proc.connect(this.ctx.destination); /* outputs silence; needed for the processor to run */
    const track = this.stream.getAudioTracks()[0];
    const s = track.getSettings ? track.getSettings() : {};
    this.info = { label: track.label || "microphone", processing: !!(s.echoCancellation || s.noiseSuppression || s.autoGainControl) };
  }

  samples() {
    const x = new Float32Array(this.count);
    let o = 0;
    for (const c of this.chunks) { x.set(c, o); o += c.length; }
    return x;
  }

  stop() {
    try { this.proc && this.proc.disconnect(); } catch (e) { /* closed */ }
    if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
    if (this.ctx) this.ctx.close().catch(() => {});
  }
}

const resSleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fmtHz = (f) => (f >= 2000 ? `${(f / 1000).toFixed(f >= 10000 ? 0 : 1)} kHz` : `${Math.round(f)} Hz`);

/* A chart over a logarithmic frequency axis; 0 dB at the top. */
function resonanceChart({ fMin, fMax, series, marks = [], band = null, dbMin = -40 }) {
  const Wd = 720, H = 240, L = 40, R = 14, T = 24, B = 28;
  const X = (f) => L + (Math.log(f / fMin) / Math.log(fMax / fMin)) * (Wd - L - R);
  const Y = (db) => T + (Math.min(0, Math.max(dbMin, db)) / dbMin) * (H - T - B);
  const s = svg("svg", { viewBox: `0 0 ${Wd} ${H}`, class: "res-svg", role: "img" });
  s.append(svg("rect", { x: L, y: T, width: Wd - L - R, height: H - T - B, class: "plot" }));
  if (band) {
    const x0 = X(Math.max(fMin, band.f0)), x1 = X(Math.min(fMax, band.f1));
    s.append(svg("rect", { x: x0, y: T, width: Math.max(1, x1 - x0), height: H - T - B, class: "band" }));
    s.append(svg("text", { x: (x0 + x1) / 2, y: T + 13, class: "band-text", "text-anchor": "middle" }, band.label));
  }
  for (let db = 0; db >= dbMin; db -= 10) {
    s.append(svg("line", { x1: L, x2: Wd - R, y1: Y(db), y2: Y(db), class: "grid" }));
    s.append(svg("text", { x: L - 6, y: Y(db) + 4, class: "tick", "text-anchor": "end" }, db));
  }
  for (const f of [200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000]) {
    if (f < fMin || f > fMax) continue;
    s.append(svg("line", { x1: X(f), x2: X(f), y1: T, y2: H - B, class: "grid" }));
    s.append(svg("text", { x: X(f), y: H - B + 16, class: "tick", "text-anchor": "middle" },
      f >= 1000 ? `${f / 1000}k` : f));
  }
  s.append(svg("text", { x: L - 6, y: T - 10, class: "tick", "text-anchor": "end" }, "dB"));
  for (const se of series) {
    const pts = se.points.filter((p) => p.f >= fMin && p.f <= fMax);
    if (se.kind === "line" && pts.length > 1) {
      s.append(svg("polyline", { points: pts.map((p) => `${X(p.f).toFixed(1)},${Y(p.db).toFixed(1)}`).join(" "),
        class: `line ${se.cls}` }));
    } else {
      for (const p of pts) s.append(svg("circle", { cx: X(p.f), cy: Y(p.db), r: 2.2, class: `dot ${se.cls}` }));
    }
  }
  for (const m of marks) {
    if (m.f < fMin || m.f > fMax) continue;
    s.append(svg("line", { x1: X(m.f), x2: X(m.f), y1: T, y2: H - B, class: "mark" }));
    s.append(svg("text", { x: X(m.f) + 4, y: Y(m.db) - 6, class: "mark-text" }, fmtHz(m.f)));
  }
  return s;
}

function resonanceResult(r, mic, profileBand) {
  const out = [];
  const warn = [];
  if (r.clippedShare > 0.001) warn.push("The recording clipped: move the microphone further away or choose a lower volume.");
  if (r.bestSnrDb < 20) warn.push(`The tones stand only ${Math.round(r.bestSnrDb)} dB above the background: bring the ` +
    "microphone closer or make the room quieter.");
  if (mic.processing) warn.push("The browser kept a voice processing of the microphone on (echo or noise suppression, " +
    "automatic gain): levels may be off.");
  if (r.harmonicOffsetDb !== null && Math.abs(r.harmonicOffsetDb) > 4) {
    warn.push(`The harmonics come out ${r.harmonicOffsetDb > 0 ? "louder" : "quieter"} than the fundamentals at the same ` +
      `frequency by ${Math.abs(r.harmonicOffsetDb).toFixed(1)} dB: the drive is no clean square wave, read the curve ` +
      "above 5 kHz with care.");
  }
  for (const w of warn) out.push(el("p", { class: "hint warn" }, w));

  out.push(el("div", { class: "subhead" }, "Response of piezo and housing"));
  out.push(resonanceChart({ fMin: 200, fMax: Math.min(20000, r.rate / 2), marks: r.peaks, series: [
    { kind: "line", cls: "resp", points: r.curve },
    { kind: "dots", cls: "h1", points: r.points.filter((p) => p.n === 1) },
    { kind: "dots", cls: "h3", points: r.points.filter((p) => p.n === 3) },
    { kind: "dots", cls: "h5", points: r.points.filter((p) => p.n === 5) },
    { kind: "dots", cls: "h7", points: r.points.filter((p) => p.n === 7) }] }));
  out.push(el("p", { class: "hint res-legend" },
    el("span", { class: "key h1" }, "●"), " fundamental ", el("span", { class: "key h3" }, "●"), " 3rd ",
    el("span", { class: "key h5" }, "●"), " 5th ", el("span", { class: "key h7" }, "●"), " 7th harmonic, ",
    "the line their mean. Up to 5 kHz from the tones themselves, above from their harmonics (the square wave has ",
    "1/3, 1/5, 1/7 of the fundamental's amplitude there; added back). Peaks are resonances."));
  if (r.peaks.length) {
    out.push(el("ul", { class: "res-list" }, r.peaks.map((p) => {
      const fs = resonanceFundamentals(p.f);
      return el("li", {}, el("b", {}, fmtHz(p.f)), ` (${Math.round(p.prom)} dB above its surroundings)`,
        fs.length ? `: tones at ${fs.map((s) => `${s.f} Hz (${s.n}${s.n === 3 ? "rd" : "th"})`).join(", ")} put a harmonic on it` : "");
    })));
  } else {
    out.push(el("p", { class: "hint" }, "No clear resonance between 500 Hz and 12 kHz."));
  }

  out.push(el("div", { class: "subhead" }, "Loudness of each tone as heard"));
  out.push(resonanceChart({ fMin: 200, fMax: 5000, band: profileBand, marks: r.loudest,
    series: [{ kind: "line", cls: "heard", points: r.heard }] }));
  out.push(el("p", { class: "hint" }, "All harmonics of the tone, A-weighted like the ear, 0 dB = the loudest tone. " +
    (profileBand ? "The band is the frequency range of the selected profile's tone curve. " : "") +
    (r.loudest.length ? `Loud spots for vario tones: ${r.loudest.map((p) => fmtHz(p.f)).join(", ")}.` : "")));
  return out;
}

function resonanceFieldset() {
  let vol = 60, perOctave = 12, last = null;
  const volBox = slider("60", "60", (v) => { vol = parseInt(v, 10); }, null, PIEZO_STEPS);
  const resSel = input({ type: "select", options: [["12", "Quick: 1/12 octave, about 35 s"],
    ["24", "Fine: 1/24 octave, about 70 s"]] }, "12", (v) => { perOctave = parseInt(v, 10); });
  const status = el("p", { class: "hint" }, "");
  const result = el("div", { class: "res-result" });
  const downloads = el("div", { class: "buttons" });
  const button = el("button", { disabled: !deviceClient() || !navigator.mediaDevices, onclick: () => run() }, "▶ Measure");

  /* the frequency range of the selected profile's tone curve (profile 1 outside a profile) */
  const profileBand = () => {
    try {
      const n = state.profile && profileExists(state.profile) ? state.profile : 1;
      if (!profileExists(n)) return null;
      const fs = readTones(n).points.map((p) => p.freq).filter((f) => f >= 10);
      return fs.length ? { f0: Math.min(...fs), f1: Math.max(...fs), label: `profile ${n}` } : null;
    } catch (e) {
      return null; /* no configuration loaded */
    }
  };

  async function run() {
    if (resonanceRun) { resonanceRun.abort = true; return; }
    const freqs = resonanceSteps(perOctave);
    const rec = new MicRecorder();
    const me = { abort: false, rec };
    try {
      await rec.start();
    } catch (e) {
      rec.stop();
      setMessage(`No microphone: ${e.message}`, true);
      return;
    }
    resonanceRun = me;
    button.textContent = "■ Stop";
    result.replaceChildren();
    downloads.replaceChildren();
    const marks = [];
    let ok = true;
    try {
      status.textContent = "Listening to the background…";
      await resSleep(RES.noiseS * 1000);
      for (let i = 0; i < freqs.length && !me.abort; i++) {
        status.textContent = `Tone ${i + 1} of ${freqs.length}: ${freqs[i]} Hz`;
        marks.push(rec.count);
        if (!(await deviceTest(`TONE ${freqs[i]} ${RES.toneMs} ${vol}`))) { ok = false; break; }
        await resSleep(RES.toneMs + RES.gapMs);
      }
      await resSleep(200);
    } finally {
      rec.stop();
      resonanceRun = null;
      button.textContent = "▶ Measure";
    }
    if (me.abort || !ok) {
      status.textContent = me.abort ? "Stopped." : "The device refused a tone.";
      return;
    }
    status.textContent = "Analysing…";
    await resSleep(20);
    const x = rec.samples();
    const r = analyzeSweep(x, rec.rate, marks, freqs);
    last = { x, r };
    status.textContent = `${freqs.length} tones at ${PIEZO_STEPS.find(([s]) => s === vol)[1].toLowerCase()} volume, ` +
      `${rec.info.label}, ${rec.rate / 1000} kHz.`;
    result.replaceChildren(...resonanceResult(r, rec.info, profileBand()));
    const stamp = new Date().toISOString().slice(0, 19).replace(/[-:T]/g, "");
    downloads.replaceChildren(
      el("button", { onclick: () => downloadBytes(`piezo-sweep-${stamp}.wav`, wavBytes(last.x, last.r.rate)) },
        "Download recording (WAV)"),
      el("button", { onclick: () => download(`piezo-sweep-${stamp}.json`, JSON.stringify({
        volume: vol, toneMs: RES.toneMs, gapMs: RES.gapMs, rate: last.r.rate, freqs: last.r.freqs, marks: last.r.marks,
        microphone: rec.info, peaks: last.r.peaks, loudest: last.r.loudest, curve: last.r.curve, heard: last.r.heard,
        steps: last.r.steps }, null, 1)) }, "Download the values (JSON)"));
  }

  return fieldset({ title: "Piezo resonance", help: "The device plays tones from 200 Hz to 5 kHz one after the " +
      "other; the browser records them with the microphone of this computer or phone and measures each tone and its " +
      "harmonics. The result shows where piezo and housing resonate and which tone frequencies sound loud. The " +
      "recording stays in the browser." },
  el("p", { class: "hint" }, "Quiet room, the device on a soft cloth, the microphone 5 to 10 cm in front of the " +
    "piezo's sound opening; keep still during the sweep. A phone or laptop microphone is good enough: resonances " +
    "stand 10 dB and more above their surroundings, the microphone's own response shifts the levels by a few dB. " +
    "Uses the drive chosen above."),
  customRow("Volume", volBox), customRow("Resolution", resSel),
  el("div", { class: "buttons" }, button), status, result, downloads);
}
