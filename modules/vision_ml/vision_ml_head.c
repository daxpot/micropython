/*
 * vision_ml_head.c
 * Float32 Dense + ReLU + Softmax forward pass for the classifier head.
 */

#include "vision_ml_head.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

static void *psram_alloc(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

int vision_ml_head_init(vision_ml_head_t *head,
                         int in_dim, int hidden_dim, int n_classes,
                         const uint8_t *weights_blob, size_t weights_size) {
    memset(head, 0, sizeof(*head));
    head->in_dim = in_dim;
    head->hidden_dim = hidden_dim;
    head->n_classes = n_classes;

    size_t expected = vision_ml_head_expected_bytes(in_dim, hidden_dim, n_classes);
    if (weights_size != expected) {
        return -1;
    }

    size_t fc1_W_n = (size_t)in_dim * hidden_dim;
    size_t fc1_b_n = (size_t)hidden_dim;
    size_t fc2_W_n = (size_t)hidden_dim * n_classes;
    size_t fc2_b_n = (size_t)n_classes;

    head->fc1_W = (float *)psram_alloc(fc1_W_n * sizeof(float));
    head->fc1_b = (float *)psram_alloc(fc1_b_n * sizeof(float));
    head->fc2_W = (float *)psram_alloc(fc2_W_n * sizeof(float));
    head->fc2_b = (float *)psram_alloc(fc2_b_n * sizeof(float));
    head->hidden_buf = (float *)psram_alloc(fc1_b_n * sizeof(float));
    head->logits_buf = (float *)psram_alloc(fc2_b_n * sizeof(float));

    if (!head->fc1_W || !head->fc1_b || !head->fc2_W || !head->fc2_b ||
        !head->hidden_buf || !head->logits_buf) {
        vision_ml_head_deinit(head);
        return -2;
    }

    const uint8_t *p = weights_blob;
    memcpy(head->fc1_W, p, fc1_W_n * sizeof(float)); p += fc1_W_n * sizeof(float);
    memcpy(head->fc1_b, p, fc1_b_n * sizeof(float)); p += fc1_b_n * sizeof(float);
    memcpy(head->fc2_W, p, fc2_W_n * sizeof(float)); p += fc2_W_n * sizeof(float);
    memcpy(head->fc2_b, p, fc2_b_n * sizeof(float));

    return 0;
}

void vision_ml_head_forward(vision_ml_head_t *head,
                             const float *features,
                             float *probs_out) {
    const int I = head->in_dim;
    const int H = head->hidden_dim;
    const int N = head->n_classes;

    /* fc1: hidden[o] = sum_i features[i] * W[i*H + o] + b[o], then ReLU */
    for (int o = 0; o < H; o++) {
        head->hidden_buf[o] = head->fc1_b[o];
    }
    for (int i = 0; i < I; i++) {
        float fi = features[i];
        if (fi == 0.0f) continue;
        const float *Wrow = head->fc1_W + (size_t)i * H;
        for (int o = 0; o < H; o++) {
            head->hidden_buf[o] += fi * Wrow[o];
        }
    }
    for (int o = 0; o < H; o++) {
        if (head->hidden_buf[o] < 0.0f) head->hidden_buf[o] = 0.0f;
    }

    /* fc2: logits[k] = sum_h hidden[h] * W[h*N + k] + b[k] */
    for (int k = 0; k < N; k++) {
        head->logits_buf[k] = head->fc2_b[k];
    }
    for (int h = 0; h < H; h++) {
        float fh = head->hidden_buf[h];
        if (fh == 0.0f) continue;
        const float *Wrow = head->fc2_W + (size_t)h * N;
        for (int k = 0; k < N; k++) {
            head->logits_buf[k] += fh * Wrow[k];
        }
    }

    /* Numerically-stable softmax */
    float max_logit = head->logits_buf[0];
    for (int k = 1; k < N; k++) {
        if (head->logits_buf[k] > max_logit) max_logit = head->logits_buf[k];
    }
    float sum = 0.0f;
    for (int k = 0; k < N; k++) {
        float e = expf(head->logits_buf[k] - max_logit);
        probs_out[k] = e;
        sum += e;
    }
    if (sum > 0.0f) {
        float inv = 1.0f / sum;
        for (int k = 0; k < N; k++) probs_out[k] *= inv;
    }
}

void vision_ml_head_deinit(vision_ml_head_t *head) {
    if (head->fc1_W) { heap_caps_free(head->fc1_W); head->fc1_W = NULL; }
    if (head->fc1_b) { heap_caps_free(head->fc1_b); head->fc1_b = NULL; }
    if (head->fc2_W) { heap_caps_free(head->fc2_W); head->fc2_W = NULL; }
    if (head->fc2_b) { heap_caps_free(head->fc2_b); head->fc2_b = NULL; }
    if (head->hidden_buf) { heap_caps_free(head->hidden_buf); head->hidden_buf = NULL; }
    if (head->logits_buf) { heap_caps_free(head->logits_buf); head->logits_buf = NULL; }
}
