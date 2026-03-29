/*
 * WakeNet Operations - Feed Mode
 *
 * Provides a simple buffer-and-detect interface to ESP-SR WakeNet.
 * Caller feeds PCM samples; internally buffers until a full chunk
 * is available and runs detection.
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#ifndef WAKENET_OPS_H
#define WAKENET_OPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wakenet_ops wakenet_ops_t;

// Create WakeNet instance (loads model from "model" partition).
// Returns NULL on failure.
wakenet_ops_t *wakenet_ops_create(void);

// Destroy and free all resources.
void wakenet_ops_destroy(wakenet_ops_t *ops);

// Feed PCM samples and run detection.
//   samples:     pointer to int16_t PCM data
//   num_samples: number of int16_t samples (not bytes)
// Returns wake word name if detected, NULL otherwise.
const char *wakenet_ops_detect(wakenet_ops_t *ops,
                               const int16_t *samples, int num_samples);

// Reset internal accumulation buffer (discard partial data).
void wakenet_ops_clear(wakenet_ops_t *ops);

// Set detection threshold (0.5 ~ 0.99).
int wakenet_ops_set_threshold(wakenet_ops_t *ops, float threshold);

// Get the number of samples per WakeNet chunk.
int wakenet_ops_get_chunk_samples(wakenet_ops_t *ops);

// Get the expected sample rate in Hz.
int wakenet_ops_get_sample_rate(wakenet_ops_t *ops);

// Get the configured wake word name.
const char *wakenet_ops_get_wake_word(wakenet_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif // WAKENET_OPS_H
