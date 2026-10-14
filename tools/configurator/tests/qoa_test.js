"use strict";

/*
 * Self-test of js/qoa.js under Node: a round trip of a synthetic signal (SNR), short last
 * slices, and - with a voice pack given as the argument - the decoding of a real clip.
 *   node tools/configurator/tests/qoa_test.js [path/to/de.ubpack]
 */
const fs = require("node:fs");
const path = require("node:path");
const { qoaEncode, qoaDecode } = require(path.join(__dirname, "..", "js", "qoa.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

/* a 1.3 s "word": two tones with an envelope and a little noise, 22050 Hz */
const rate = 22050;
const n = Math.round(rate * 1.3);
const pcm = new Int16Array(n);
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff - 0.5; };
for (let i = 0; i < n; i++) {
  const t = i / rate;
  const env = Math.sin(Math.PI * t / 1.3);
  const v = env * (0.5 * Math.sin(2 * Math.PI * 220 * t) + 0.25 * Math.sin(2 * Math.PI * 1760 * t)) + 0.01 * rnd();
  pcm[i] = Math.max(-32768, Math.min(32767, Math.round(v * 20000)));
}

const t0 = Date.now();
const bytes = qoaEncode(pcm, rate);
const encodeMs = Date.now() - t0;
const { sampleRate, samples } = qoaDecode(bytes);
check(sampleRate === rate && samples.length === n, `round trip: ${samples.length} samples at ${sampleRate} Hz`);
let sig = 0, err = 0;
for (let i = 0; i < n; i++) {
  sig += pcm[i] * pcm[i];
  const d = samples[i] - pcm[i];
  err += d * d;
}
const snr = 10 * Math.log10(sig / err);
check(snr > 40, `SNR ${snr.toFixed(1)} dB (${bytes.length} bytes, ${(bytes.length * 8 / 1.3 / 1000).toFixed(0)} kbit/s, encode ${encodeMs} ms)`);
check(bytes.length === 8 + Math.ceil(n / 5120) * 24 + Math.ceil(n / 20) * 8,
  "file size: header, a 24-byte header per 5120-sample frame, one slice per 20 samples");

/* 7 samples: one short slice, padded with zeros in the slice word */
const tiny = Int16Array.from([100, -200, 300, -400, 500, -600, 700]);
const tinyOut = qoaDecode(qoaEncode(tiny, rate));
check(tinyOut.samples.length === 7 && Math.abs(tinyOut.samples[6] - 700) < 80, `short last slice: ${Array.from(tinyOut.samples).join(" ")}`);

/* a real clip from a voice pack: find the first "qoaf" in the FAT image */
const pack = process.argv[2];
if (pack && fs.existsSync(pack)) {
  const img = fs.readFileSync(pack);
  const at = img.indexOf("qoaf");
  const clip = qoaDecode(new Uint8Array(img.buffer, img.byteOffset + at, Math.min(200000, img.length - at)));
  let peak = 0;
  for (const s of clip.samples) peak = Math.max(peak, Math.abs(s));
  check(clip.sampleRate === 22050 && clip.samples.length > 1000 && peak > 1000,
    `stock clip: ${clip.samples.length} samples at ${clip.sampleRate} Hz, peak ${(20 * Math.log10(peak / 32768)).toFixed(1)} dBFS`);
} else {
  console.log("skip stock clip (no voice pack given)");
}

console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
