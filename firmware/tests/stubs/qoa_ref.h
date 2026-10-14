#ifndef QOA_REF_H
#define QOA_REF_H

#include <stdbool.h>
#include <stdint.h>

#include "qoa.h"

/* The earlier frame-at-once decoder (tests/qoa_ref.c). */
#define QOA_REF_PCM_MAX 5120

typedef struct {
  qoa_source_t source;
  void *ctx;
  uint32_t total_samples;
  uint32_t samplerate;
  int16_t pcm[QOA_REF_PCM_MAX];
  uint32_t len;
  uint32_t pos;
} qoa_ref_t;

bool qoa_ref_open(qoa_ref_t *qs, qoa_source_t source, void *ctx);
uint32_t qoa_ref_read(qoa_ref_t *qs, int16_t *out, uint32_t count);

#endif
