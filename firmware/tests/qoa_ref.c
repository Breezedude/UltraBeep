/*
 * Reference for the tests: the earlier frame-at-once QOA decoder, which decoded a
 * whole frame at once. The streaming decoder (src/drivers/qoa.c) must give the
 * same samples.
 */
#include <math.h>
#include <string.h>

#include "qoa_ref.h"

#define QOA_MAGIC      0x716f6166U /* "qoaf" */
#define QOA_SLICE_LEN  20

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

bool qoa_ref_open(qoa_ref_t *qs, qoa_source_t source, void *ctx)
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

/* Decodes the next frame into qs->pcm; returns the number of samples, 0 at end/error. */
static uint32_t decode_frame(qoa_ref_t *qs)
{
  static uint8_t buf[QOA_FRAME_MAX];
  if (qs->source(qs->ctx, buf, 8) != 8) {
    return 0;
  }
  uint64_t fh = read_u64(buf);
  uint32_t channels = (uint32_t)(fh >> 56) & 0xFF;
  uint32_t samplerate = (uint32_t)(fh >> 32) & 0xFFFFFF;
  uint32_t samples = (uint32_t)(fh >> 16) & 0xFFFF;
  uint32_t frame_size = (uint32_t)fh & 0xFFFF;
  if (channels != 1 || frame_size < 8 + 16 || frame_size > QOA_FRAME_MAX || samples > QOA_REF_PCM_MAX) {
    return 0;
  }
  if (qs->source(qs->ctx, buf + 8, frame_size - 8) != frame_size - 8) {
    return 0;
  }
  qs->samplerate = samplerate;

  int history[4], weights[4];
  uint64_t hist = read_u64(buf + 8);
  uint64_t wts = read_u64(buf + 16);
  for (int i = 0; i < 4; i++) {
    history[i] = (int16_t)(hist >> 48);
    hist <<= 16;
    weights[i] = (int16_t)(wts >> 48);
    wts <<= 16;
  }

  const uint8_t *p = buf + 24;
  uint32_t n = 0;
  while (n < samples && p + 8 <= buf + frame_size) {
    uint64_t slice = read_u64(p);
    p += 8;
    int sf = (int)(slice >> 60) & 0xF;
    slice <<= 4;
    for (int i = 0; i < QOA_SLICE_LEN && n < samples; i++) {
      int predicted = (weights[0] * history[0] + weights[1] * history[1] + weights[2] * history[2] +
                       weights[3] * history[3]) >> 13;
      int residual = dequant[sf][(slice >> 61) & 0x7];
      int sample = clamp_s16(predicted + residual);
      slice <<= 3;
      int delta = residual >> 4;
      for (int k = 0; k < 4; k++) {
        weights[k] += history[k] < 0 ? -delta : delta;
      }
      history[0] = history[1];
      history[1] = history[2];
      history[2] = history[3];
      history[3] = sample;
      qs->pcm[n++] = (int16_t)sample;
    }
  }
  return n;
}

uint32_t qoa_ref_read(qoa_ref_t *qs, int16_t *out, uint32_t count)
{
  uint32_t done = 0;
  while (done < count) {
    if (qs->pos >= qs->len) {
      qs->len = decode_frame(qs);
      qs->pos = 0;
      if (!qs->len) {
        break;
      }
    }
    uint32_t n = qs->len - qs->pos;
    if (n > count - done) {
      n = count - done;
    }
    memcpy(&out[done], &qs->pcm[qs->pos], n * sizeof(int16_t));
    qs->pos += n;
    done += n;
  }
  return done;
}
