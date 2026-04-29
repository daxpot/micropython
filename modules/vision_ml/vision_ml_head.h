/*
 * vision_ml_head.h
 *
 * Float32 forward pass for the classification head:
 *   feat[256] -> Dense(256, 128, relu) -> Dropout(skipped) -> Dense(128, N, softmax) -> probs[N]
 *
 * Weights are loaded from head_weights.bin (raw float32 LE concatenation
 * in order: fc1.W, fc1.b, fc2.W, fc2.b). Layouts are row-major:
 *   fc1.W shape [in=256, out=128]: index [i, o] = W[i*128 + o]
 *   fc2.W shape [in=128, out=N]:   index [i, o] = W[i*N   + o]
 */

#ifndef VISION_ML_HEAD_H
#define VISION_ML_HEAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int in_dim;     /* must be 256 */
    int hidden_dim; /* fc1 output (e.g. 128) */
    int n_classes;  /* fc2 output */

    /* All buffers are float32 in PSRAM, owned by this struct. */
    float *fc1_W;   /* [in_dim * hidden_dim] */
    float *fc1_b;   /* [hidden_dim] */
    float *fc2_W;   /* [hidden_dim * n_classes] */
    float *fc2_b;   /* [n_classes] */

    /* Reusable scratch for forward pass. */
    float *hidden_buf; /* [hidden_dim] */
    float *logits_buf; /* [n_classes] */
} vision_ml_head_t;

/* Allocate buffers and load raw float32 weights from `weights_blob`
 * (size in bytes must equal expected_size below).
 * Returns 0 on success, -1 on bad size, -2 on alloc failure. */
int vision_ml_head_init(vision_ml_head_t *head,
                         int in_dim, int hidden_dim, int n_classes,
                         const uint8_t *weights_blob, size_t weights_size);

/* Forward pass. probs_out[n_classes] receives softmax probabilities. */
void vision_ml_head_forward(vision_ml_head_t *head,
                             const float *features,
                             float *probs_out);

void vision_ml_head_deinit(vision_ml_head_t *head);

/* Expected weights blob size for the given dims. */
static inline size_t vision_ml_head_expected_bytes(int in_dim, int hidden_dim, int n_classes) {
    return (size_t)sizeof(float) *
           ((size_t)in_dim * hidden_dim + hidden_dim +
            (size_t)hidden_dim * n_classes + n_classes);
}

#ifdef __cplusplus
}
#endif

#endif /* VISION_ML_HEAD_H */
