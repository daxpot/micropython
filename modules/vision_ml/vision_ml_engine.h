/*
 * vision_ml_engine.h
 *
 * C interface to the TFLite Micro backbone engine.
 * Implementation lives in vision_ml_engine.cpp.
 */

#ifndef VISION_ML_ENGINE_H
#define VISION_ML_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VISION_ML_FEATURE_DIM   256
#define VISION_ML_INPUT_SIZE    224
#define VISION_ML_INPUT_BYTES   (VISION_ML_INPUT_SIZE * VISION_ML_INPUT_SIZE * 3)

/* Initialize backbone: maps ml_model partition, builds TFLM interpreter,
 * allocates tensor arena in PSRAM. Idempotent.
 * Returns 0 on success, negative error code otherwise:
 *   -1 ml_model partition not found
 *   -2 mmap failed
 *   -3 model schema version mismatch
 *   -4 tensor arena PSRAM alloc failed
 *   -5 AllocateTensors failed
 *   -6 unexpected input/output shape
 */
int vision_ml_engine_init(void);

/* Run backbone on a 224x224 RGB888 buffer (length = 150528 bytes).
 * Performs (pixel/127.5)-1 normalization + int8 quantization, invokes the
 * model, then global-average-pools the [1,7,7,256] feature map down to a
 * 256-dim float32 vector written to features_out_256.
 * Returns 0 on success.
 */
int vision_ml_engine_extract_features(const uint8_t *rgb888,
                                       int rgb_len,
                                       float *features_out_256);

void vision_ml_engine_deinit(void);
bool vision_ml_engine_initialized(void);

/* Diagnostics. Returns 0 if not initialized. */
size_t vision_ml_engine_arena_used(void);
size_t vision_ml_engine_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif /* VISION_ML_ENGINE_H */
