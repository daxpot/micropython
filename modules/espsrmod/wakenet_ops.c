/*
 * WakeNet Operations - Feed Mode Implementation
 *
 * Manages WakeNet model lifecycle and provides a streaming
 * detect interface with internal PCM buffering.
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#include "wakenet_ops.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#define TAG "wakenet_ops"

struct wakenet_ops {
    // Model resources
    srmodel_list_t *models;
    const esp_wn_iface_t *wakenet;
    model_iface_data_t *wn_data;

    // Model info
    char *wake_word_name;
    int chunk_samples;
    int sample_rate;

    // Internal accumulation buffer (size = chunk_samples int16_t's)
    int16_t *buffer;
    int buffer_pos;
};

wakenet_ops_t *wakenet_ops_create(void) {
    wakenet_ops_t *ops = calloc(1, sizeof(wakenet_ops_t));
    if (!ops) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    // Load models from flash partition
    ops->models = esp_srmodel_init("model");
    if (!ops->models) {
        ESP_LOGE(TAG, "Failed to load models from 'model' partition");
        goto fail;
    }

    // Find first available WakeNet model
    char *wn_name = esp_srmodel_filter(ops->models, ESP_WN_PREFIX, NULL);
    if (!wn_name) {
        ESP_LOGE(TAG, "No WakeNet model found in partition");
        goto fail;
    }
    ESP_LOGI(TAG, "Found model: %s", wn_name);

    // Get WakeNet interface handle
    ops->wakenet = esp_wn_handle_from_name(wn_name);
    if (!ops->wakenet) {
        ESP_LOGE(TAG, "Failed to get WakeNet handle for %s", wn_name);
        goto fail;
    }

    // Log available memory before loading neural network
    ESP_LOGI(TAG, "Free heap: %lu, Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Create WakeNet instance (DET_MODE_90 = balanced sensitivity)
    ops->wn_data = ops->wakenet->create(wn_name, DET_MODE_90);
    if (!ops->wn_data) {
        ESP_LOGE(TAG, "Failed to create WakeNet instance (not enough memory?)");
        goto fail;
    }

    // Read model parameters
    ops->chunk_samples = ops->wakenet->get_samp_chunksize(ops->wn_data);
    ops->sample_rate = ops->wakenet->get_samp_rate(ops->wn_data);
    ops->wake_word_name = esp_wn_wakeword_from_name(wn_name);

    ESP_LOGI(TAG, "WakeNet ready: wake_word=%s, chunk=%d samples, rate=%d Hz",
             ops->wake_word_name ? ops->wake_word_name : "unknown",
             ops->chunk_samples, ops->sample_rate);

    // Allocate accumulation buffer (prefer PSRAM)
    size_t buf_bytes = ops->chunk_samples * sizeof(int16_t);
    ops->buffer = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ops->buffer) {
        ops->buffer = malloc(buf_bytes);
    }
    if (!ops->buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer (%d bytes)", (int)buf_bytes);
        goto fail;
    }
    ops->buffer_pos = 0;

    // Set default threshold (word_index 0 = first wake word)
    ops->wakenet->set_det_threshold(ops->wn_data, 0.9f, 0);

    return ops;

fail:
    wakenet_ops_destroy(ops);
    return NULL;
}

void wakenet_ops_destroy(wakenet_ops_t *ops) {
    if (!ops) {
        return;
    }
    if (ops->wn_data && ops->wakenet) {
        ops->wakenet->destroy(ops->wn_data);
    }
    if (ops->models) {
        esp_srmodel_deinit(ops->models);
    }
    free(ops->buffer);
    free(ops);
    ESP_LOGI(TAG, "WakeNet destroyed");
}

const char *wakenet_ops_detect(wakenet_ops_t *ops,
                               const int16_t *samples, int num_samples) {
    if (!ops || !ops->wn_data || !samples || num_samples <= 0) {
        return NULL;
    }

    int remaining = num_samples;
    const int16_t *src = samples;

    while (remaining > 0) {
        // Fill the accumulation buffer toward chunk_samples
        int need = ops->chunk_samples - ops->buffer_pos;
        int copy = (remaining < need) ? remaining : need;

        memcpy(ops->buffer + ops->buffer_pos, src, copy * sizeof(int16_t));
        ops->buffer_pos += copy;
        src += copy;
        remaining -= copy;

        // When we have a full chunk, run detection
        if (ops->buffer_pos >= ops->chunk_samples) {
            int result = ops->wakenet->detect(ops->wn_data, ops->buffer);
            ops->buffer_pos = 0;

            if (result > 0) {
                return ops->wake_word_name ? ops->wake_word_name : "detected";
            }
        }
    }

    return NULL;
}

void wakenet_ops_clear(wakenet_ops_t *ops) {
    if (ops) {
        ops->buffer_pos = 0;
    }
}

int wakenet_ops_set_threshold(wakenet_ops_t *ops, float threshold) {
    if (!ops || !ops->wn_data) {
        return -1;
    }
    ops->wakenet->set_det_threshold(ops->wn_data, threshold, 0);
    return 0;
}

int wakenet_ops_get_chunk_samples(wakenet_ops_t *ops) {
    return ops ? ops->chunk_samples : 0;
}

int wakenet_ops_get_sample_rate(wakenet_ops_t *ops) {
    return ops ? ops->sample_rate : 0;
}

const char *wakenet_ops_get_wake_word(wakenet_ops_t *ops) {
    if (!ops || !ops->wake_word_name) {
        return "unknown";
    }
    return ops->wake_word_name;
}

#endif // CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
