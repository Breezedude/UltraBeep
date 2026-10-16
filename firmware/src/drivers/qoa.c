/*
 * Streaming QOA ("Quite OK Audio") decoder for mono files.
 * Format: https://qoaformat.org (reference implementation by Dominic
 * Szablewski, MIT). Reads one frame (<= 5120 samples) at a time from a byte
 * source (audio.c: a read-ahead ring buffer filled from the SD card) and
 * decodes it slice by slice, as many samples as are asked for.
 */
#include <string.h>

#include "qoa.h"

#define QOA_MAGIC 0x716f6166U /* "qoaf" */

/* round(round((s + 1)^2.75) * {0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7}): the reference's table, no run-time powf. */
static const int dequant[16][8] = {
  {1, -1, 3, -3, 5, -5, 7, -7},
  {5, -5, 18, -18, 32, -32, 49, -49},
  {16, -16, 53, -53, 95, -95, 147, -147},
  {34, -34, 113, -113, 203, -203, 315, -315},
  {63, -63, 210, -210, 378, -378, 588, -588},
  {104, -104, 345, -345, 621, -621, 966, -966},
  {158, -158, 528, -528, 950, -950, 1477, -1477},
  {228, -228, 760, -760, 1368, -1368, 2128, -2128},
  {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
  {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
  {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
  {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
  {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
  {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
  {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
  {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

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

bool qoa_open(qoa_stream_t *qs, qoa_source_t source, void *ctx)
{
  uint8_t hdr[8];
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
