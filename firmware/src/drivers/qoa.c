/*
 * Streaming QOA ("Quite OK Audio") decoder for mono files.
 * Format: https://qoaformat.org (reference implementation by Dominic
 * Szablewski, MIT). Reads one frame (<= 5120 samples) at a time from a byte
 * source (audio.c: a read-ahead ring buffer filled from the SD card) and
 * decodes it slice by slice, as many samples as are asked for.
 */
#include <math.h>
#include <string.h>

#include "qoa.h"

#define QOA_MAGIC 0x716f6166U /* "qoaf" */

static int dequant[16][8];

static uint64_t read_u64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

static int clamp_s16(int v)
{
  return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static void init_tables(void)
{
  static const float lut[8] = {0.75f, -0.75f, 2.5f, -2.5f, 4.5f, -4.5f, 7.0f, -7.0f};
  if (dequant[15][0]) {
    return;
  }
  for (int s = 0; s < 16; s++) {
    float sf = roundf(powf((float)(s + 1), 2.75f));
    for (int q = 0; q < 8; q++) {
      dequant[s][q] = (int)roundf(sf * lut[q]);
    }
  }
}

bool qoa_open(qoa_stream_t *qs, qoa_source_t source, void *ctx)
{
  uint8_t hdr[8];
  init_tables();
  memset(qs, 0, sizeof(*qs));
  qs->source = source;
  qs->ctx = ctx;
  if (source(ctx, hdr, sizeof(hdr)) != sizeof(hdr)) {
    return false;
  }
  uint64_t h = read_u64(hdr);
  if ((uint32_t)(h >> 32) != QOA_MAGIC) {
    return false;
  }
  qs->total_samples = (uint32_t)h;
  return true;
}

/* Reads the next frame and its LMS state; false at the end or on an error. */
static bool next_frame(qoa_stream_t *qs)
{
  uint8_t *buf = qs->frame;
  if (qs->source(qs->ctx, buf, 8) != 8) {
    return false;
  }
  uint64_t fh = read_u64(buf);
  uint32_t channels = (uint32_t)(fh >> 56) & 0xFF;
  uint32_t samplerate = (uint32_t)(fh >> 32) & 0xFFFFFF;
  uint32_t samples = (uint32_t)(fh >> 16) & 0xFFFF;
  uint32_t frame_size = (uint32_t)fh & 0xFFFF;
  if (channels != 1 || frame_size < 8 + 16 || frame_size > QOA_FRAME_MAX || samples > 256U * QOA_SLICE_LEN) {
    return false;
  }
  if (qs->source(qs->ctx, buf + 8, frame_size - 8) != frame_size - 8) {
    return false;
  }
  qs->samplerate = samplerate;
  uint64_t hist = read_u64(buf + 8);
  uint64_t wts = read_u64(buf + 16);
  for (int i = 0; i < 4; i++) {
    qs->history[i] = (int16_t)(hist >> 48);
    hist <<= 16;
    qs->weights[i] = (int16_t)(wts >> 48);
    wts <<= 16;
  }
  qs->frame_bytes = frame_size;
  qs->frame_samples = samples;
  qs->frame_done = 0;
  qs->slice_pos = 24;
  qs->slice_left = 0;
  return true;
}

uint32_t qoa_read(qoa_stream_t *qs, int16_t *out, uint32_t count)
{
  uint32_t done = 0;
  while (done < count) {
    if (qs->frame_done >= qs->frame_samples) {
      if (!next_frame(qs) || qs->frame_samples == 0) {
        break;
      }
    }
    if (qs->slice_left == 0) {
      if (qs->slice_pos + 8 > qs->frame_bytes) {
        qs->frame_done = qs->frame_samples; /* short frame: go on with the next one */
        continue;
      }
      uint64_t slice = read_u64(qs->frame + qs->slice_pos);
      qs->slice_pos += 8;
      qs->slice_sf = (int)(slice >> 60) & 0xF;
      qs->slice = slice << 4;
      qs->slice_left = QOA_SLICE_LEN;
    }
    int *history = qs->history;
    int *weights = qs->weights;
    while (qs->slice_left > 0 && done < count && qs->frame_done < qs->frame_samples) {
      int predicted = (weights[0] * history[0] + weights[1] * history[1] + weights[2] * history[2] +
                       weights[3] * history[3]) >> 13;
      int residual = dequant[qs->slice_sf][(qs->slice >> 61) & 0x7];
      int sample = clamp_s16(predicted + residual);
      qs->slice <<= 3;
      int delta = residual >> 4;
      for (int k = 0; k < 4; k++) {
        weights[k] += history[k] < 0 ? -delta : delta;
      }
      history[0] = history[1];
      history[1] = history[2];
      history[2] = history[3];
      history[3] = sample;
      out[done++] = (int16_t)sample;
      qs->slice_left--;
      qs->frame_done++;
    }
    if (qs->frame_done >= qs->frame_samples) {
      qs->slice_left = 0;
    }
  }
  return done;
}
