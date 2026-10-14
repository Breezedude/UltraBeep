"use strict";

/*
 * QOA ("Quite OK Audio", qoaformat.org) encoder and decoder for mono 16-bit PCM, as the
 * firmware's decoder (firmware/src/drivers/qoa.c) expects it: frames of up to 256 slices
 * of 20 samples, the LMS state in every frame header. Used by the configurator to build
 * voice clips in the browser; the same file runs under Node for the self-test.
 */

const QOA_SLICE_LEN = 20;
const QOA_SLICES_PER_FRAME = 256;
const QOA_FRAME_LEN = QOA_SLICES_PER_FRAME * QOA_SLICE_LEN;

const QOA_SCALEFACTORS = [1, 7, 21, 45, 84, 138, 211, 304, 421, 562, 731, 928, 1157, 1419, 1715, 2048];
const QOA_RECIPROCALS = [65536, 9363, 3121, 1457, 781, 475, 311, 216, 156, 117, 90, 71, 57, 47, 39, 32];
const QOA_QUANT = [7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6];
/* Rounded half away from zero as C's roundf (Math.round would give -52 for -52.5). */
const QOA_DEQUANT = QOA_SCALEFACTORS.map((s) => [0.75, 2.5, 4.5, 7].flatMap((m) => [Math.round(s * m), -Math.round(s * m)]));

const clamp16 = (v) => (v < -32768 ? -32768 : v > 32767 ? 32767 : v);

/* The LMS predictor: 4 history samples and 4 weights, as in the reference. */
class Lms {
  constructor() {
    this.history = new Int32Array(4);
    this.weights = new Int32Array(4);
  }
  predict() {
    const h = this.history, w = this.weights;
    return (w[0] * h[0] + w[1] * h[1] + w[2] * h[2] + w[3] * h[3]) >> 13;
  }
  update(sample, residual) {
    const delta = residual >> 4;
    const h = this.history, w = this.weights;
    for (let i = 0; i < 4; i++) w[i] += h[i] < 0 ? -delta : delta;
    h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = sample;
  }
  copyFrom(o) {
    this.history.set(o.history);
    this.weights.set(o.weights);
  }
}

/* Division by a scale factor with the reference's rounding. */
function qoaDiv(v, sf) {
  const reciprocal = QOA_RECIPROCALS[sf];
  let n = (v * reciprocal + (1 << 15)) >> 16;
  n = n + ((v > 0) - (v < 0)) - ((n > 0) - (n < 0));
  return n;
}

/* Encodes mono Int16 samples; returns the file as a Uint8Array. */
function qoaEncode(samples, sampleRate) {
  const total = samples.length;
  const frames = Math.ceil(total / QOA_FRAME_LEN);
  const out = [];
  const w32 = (v) => out.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
  const w16 = (v) => out.push((v >>> 8) & 255, v & 255);
  out.push(0x71, 0x6f, 0x61, 0x66); /* "qoaf" */
  w32(total);
  const lms = new Lms();
  lms.weights[2] = -(1 << 13);
  lms.weights[3] = 1 << 14;
  const trial = new Lms();
  const best = new Lms();
  for (let f = 0; f < frames; f++) {
    const start = f * QOA_FRAME_LEN;
    const count = Math.min(QOA_FRAME_LEN, total - start);
    const slices = Math.ceil(count / QOA_SLICE_LEN);
    out.push(1);
    out.push((sampleRate >>> 16) & 255, (sampleRate >>> 8) & 255, sampleRate & 255);
    w16(count);
    w16(8 + 16 + slices * 8);
    for (let i = 0; i < 4; i++) w16(lms.history[i] & 0xffff);
    for (let i = 0; i < 4; i++) w16(lms.weights[i] & 0xffff);
    for (let s = 0; s < slices; s++) {
      const sliceStart = start + s * QOA_SLICE_LEN;
      const sliceLen = Math.min(QOA_SLICE_LEN, total - sliceStart);
      let bestError = Infinity, bestSlice = 0n;
      for (let sf = 0; sf < 16; sf++) {
        trial.copyFrom(lms);
        let slice = BigInt(sf);
        let error = 0;
        for (let i = 0; i < sliceLen; i++) {
          const sample = samples[sliceStart + i];
          const predicted = trial.predict();
          const residual = sample - predicted;
          const scaled = Math.max(-8, Math.min(8, qoaDiv(residual, sf)));
          const quantized = QOA_QUANT[scaled + 8];
          const dequantized = QOA_DEQUANT[sf][quantized];
          const reconstructed = clamp16(predicted + dequantized);
          const e = sample - reconstructed;
          error += e * e;
          if (error > bestError) break;
          trial.update(reconstructed, dequantized);
          slice = (slice << 3n) | BigInt(quantized);
        }
        if (error < bestError) {
          bestError = error;
          bestSlice = slice << BigInt(3 * (QOA_SLICE_LEN - sliceLen));
          best.copyFrom(trial);
        }
      }
      lms.copyFrom(best);
      for (let b = 7; b >= 0; b--) out.push(Number((bestSlice >> BigInt(b * 8)) & 255n));
    }
  }
  return Uint8Array.from(out);
}

/* Decodes a QOA file; returns {sampleRate, samples: Int16Array} or throws. */
function qoaDecode(bytes) {
  const r32 = (p) => ((bytes[p] << 24) | (bytes[p + 1] << 16) | (bytes[p + 2] << 8) | bytes[p + 3]) >>> 0;
  const r16 = (p) => (bytes[p] << 8) | bytes[p + 1];
  const s16 = (p) => (r16(p) << 16) >> 16;
  if (bytes.length < 8 || bytes[0] !== 0x71 || bytes[1] !== 0x6f || bytes[2] !== 0x61 || bytes[3] !== 0x66) {
    throw new Error("not a QOA file");
  }
  const total = r32(4);
  const samples = new Int16Array(total);
  let pos = 8, n = 0, sampleRate = 0;
  const lms = new Lms();
  while (n < total) {
    if (pos + 24 > bytes.length) throw new Error("QOA file truncated");
    const channels = bytes[pos];
    sampleRate = (bytes[pos + 1] << 16) | (bytes[pos + 2] << 8) | bytes[pos + 3];
    const count = r16(pos + 4);
    const size = r16(pos + 6);
    if (channels !== 1) throw new Error(`QOA: ${channels} channels, mono expected`);
    for (let i = 0; i < 4; i++) lms.history[i] = s16(pos + 8 + i * 2);
    for (let i = 0; i < 4; i++) lms.weights[i] = s16(pos + 16 + i * 2);
    let p = pos + 24;
    for (let done = 0; done < count; done += QOA_SLICE_LEN) {
      const sf = bytes[p] >> 4;
      let hi = r32(p), lo = r32(p + 4); /* 64-bit slice as two halves */
      p += 8;
      for (let i = 0; i < QOA_SLICE_LEN && done + i < count; i++) {
        /* the next 3 bits from the top: shift the 64-bit value left by 3 */
        const q = (hi >>> 25) & 7; /* bits 57..59 after the scale factor */
        hi = ((hi << 3) | (lo >>> 29)) >>> 0;
        lo = (lo << 3) >>> 0;
        const predicted = lms.predict();
        const dequantized = QOA_DEQUANT[sf][q];
        const reconstructed = clamp16(predicted + dequantized);
        samples[n++] = reconstructed;
        lms.update(reconstructed, dequantized);
      }
    }
    if (p - pos !== size) throw new Error("QOA frame size mismatch");
    pos = p;
  }
  return { sampleRate, samples };
}

if (typeof module !== "undefined") {
  module.exports = { qoaEncode, qoaDecode, QOA_SLICE_LEN, QOA_FRAME_LEN };
}
