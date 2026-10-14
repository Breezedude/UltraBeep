#ifndef QOA_H
#define QOA_H

#include <stdbool.h>
#include <stdint.h>

#define QOA_SLICE_LEN 20               /* samples per slice */
#define QOA_FRAME_MAX (8 + 16 + 256 * 8) /* bytes of one mono frame (<= 5120 samples) */

/* Byte source: copies up to `len` bytes into `buf`; returns fewer at the end of the data. */
typedef uint32_t (*qoa_source_t)(void *ctx, uint8_t *buf, uint32_t len);

/*
 * Streaming decoder: the bytes of one frame are read at once, its samples are
 * decoded slice by slice as qoa_read asks for them, so the work per call stays
 * small and even (no 5120-sample burst every 232 ms).
 */
typedef struct {
  qoa_source_t source;
  void *ctx;
  uint32_t total_samples;
  uint32_t samplerate;
  uint8_t frame[QOA_FRAME_MAX];
  uint32_t frame_bytes;   /* bytes of the current frame */
  uint32_t frame_samples; /* samples of the current frame */
  uint32_t frame_done;    /* samples decoded from it */
  uint32_t slice_pos;     /* byte offset of the next slice */
  uint64_t slice;         /* current slice, shifted as its samples are decoded */
  int slice_sf;
  int slice_left;
  int history[4];
  int weights[4];
} qoa_stream_t;

bool qoa_open(qoa_stream_t *qs, qoa_source_t source, void *ctx);
/* Decodes up to `count` samples; returns fewer at the end of the file. */
uint32_t qoa_read(qoa_stream_t *qs, int16_t *out, uint32_t count);

/* Samples left in the current frame: a qoa_read for more reads the next frame (QOA_FRAME_MAX bytes). */
static inline uint32_t qoa_buffered(const qoa_stream_t *qs)
{
  return qs->frame_samples - qs->frame_done;
}

#endif
