/*
 * MultiNet Operations - Feed Mode Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "multinet_ops.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#define TAG "multinet_ops"

struct multinet_ops {
    srmodel_list_t *models;
    const esp_mn_iface_t *multinet;
    model_iface_data_t *mn_data;

    int chunk_samples;
    int sample_rate;
    bool commands_dirty;    /* true if add/remove since last update */
    bool commands_ready;    /* true after successful update() */

    /* Internal accumulation buffer */
    int16_t *buffer;
    int buffer_pos;
};

multinet_ops_t *multinet_ops_create(void) {
    multinet_ops_t *ops = calloc(1, sizeof(multinet_ops_t));
    if (!ops) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    /* Load models from flash partition */
    ops->models = esp_srmodel_init("model");
    if (!ops->models) {
        ESP_LOGE(TAG, "Failed to load models from 'model' partition");
        goto fail;
    }

    /* Find MultiNet model (prefer Chinese) */
    char *mn_name = esp_srmodel_filter(ops->models, ESP_MN_PREFIX, "cn");
    if (!mn_name) {
        mn_name = esp_srmodel_filter(ops->models, ESP_MN_PREFIX, NULL);
    }
    if (!mn_name) {
        ESP_LOGE(TAG, "No MultiNet model found in partition");
        goto fail;
    }
    ESP_LOGI(TAG, "Found model: %s", mn_name);

    /* Get MultiNet interface */
    ops->multinet = esp_mn_handle_from_name(mn_name);
    if (!ops->multinet) {
        ESP_LOGE(TAG, "Failed to get MultiNet handle for %s", mn_name);
        goto fail;
    }

    ESP_LOGI(TAG, "Free heap: %lu, Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Create MultiNet instance (default duration 5000ms) */
    ops->mn_data = ops->multinet->create(mn_name, 5000);
    if (!ops->mn_data) {
        ESP_LOGE(TAG, "Failed to create MultiNet instance (not enough memory?)");
        goto fail;
    }

    /* Query model parameters */
    ops->chunk_samples = ops->multinet->get_samp_chunksize(ops->mn_data);
    ops->sample_rate = ops->multinet->get_samp_rate(ops->mn_data);

    ESP_LOGI(TAG, "MultiNet ready: chunk=%d samples, rate=%d Hz",
             ops->chunk_samples, ops->sample_rate);

    /* Allocate accumulation buffer */
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

    /* Set default threshold (reject low-confidence detections) */
    ops->multinet->set_det_threshold(ops->mn_data, 0.3f);

    /* Initialize command system */
    esp_mn_commands_clear();
    ops->commands_dirty = false;
    ops->commands_ready = false;

    return ops;

fail:
    multinet_ops_destroy(ops);
    return NULL;
}

void multinet_ops_destroy(multinet_ops_t *ops) {
    if (!ops) return;

    if (ops->mn_data && ops->multinet) {
        ops->multinet->destroy(ops->mn_data);
    }
    if (ops->models) {
        esp_srmodel_deinit(ops->models);
    }
    free(ops->buffer);
    free(ops);
    ESP_LOGI(TAG, "MultiNet destroyed");
}

int multinet_ops_clear_commands(multinet_ops_t *ops) {
    if (!ops) return -1;
    esp_mn_commands_clear();
    ops->commands_dirty = true;
    ops->commands_ready = false;
    return 0;
}

int multinet_ops_add_command(multinet_ops_t *ops, int command_id,
                             const char *pinyin) {
    if (!ops || !pinyin) return -1;

    esp_err_t err = esp_mn_commands_add(command_id, pinyin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add command %d '%s': %d",
                 command_id, pinyin, err);
        return -1;
    }

    ops->commands_dirty = true;
    ESP_LOGI(TAG, "Added command %d: '%s'", command_id, pinyin);
    return 0;
}

int multinet_ops_remove_command(multinet_ops_t *ops, const char *pinyin) {
    if (!ops || !pinyin) return -1;

    esp_err_t err = esp_mn_commands_remove(pinyin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remove command '%s': %d", pinyin, err);
        return -1;
    }

    ops->commands_dirty = true;
    return 0;
}

int multinet_ops_update(multinet_ops_t *ops) {
    if (!ops) return -1;

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL && err->num > 0) {
        ESP_LOGE(TAG, "Command update failed with %d errors", err->num);
        ops->commands_ready = false;
        return -1;
    }

    /* Print active commands for debugging */
    ops->multinet->print_active_speech_commands(ops->mn_data);

    ops->commands_dirty = false;
    ops->commands_ready = true;
    ESP_LOGI(TAG, "Commands updated and FST compiled");
    return 0;
}

bool multinet_ops_detect(multinet_ops_t *ops, const int16_t *samples,
                         int num_samples, multinet_result_t *result) {
    if (!ops || !ops->mn_data || !samples || num_samples <= 0 || !result) {
        return false;
    }
    if (!ops->commands_ready) {
        return false;
    }

    int remaining = num_samples;
    const int16_t *src = samples;

    while (remaining > 0) {
        int need = ops->chunk_samples - ops->buffer_pos;
        int copy = (remaining < need) ? remaining : need;

        memcpy(ops->buffer + ops->buffer_pos, src, copy * sizeof(int16_t));
        ops->buffer_pos += copy;
        src += copy;
        remaining -= copy;

        if (ops->buffer_pos >= ops->chunk_samples) {
            esp_mn_state_t mn_state =
                ops->multinet->detect(ops->mn_data, ops->buffer);
            ops->buffer_pos = 0;

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result =
                    ops->multinet->get_results(ops->mn_data);
                if (mn_result && mn_result->num > 0) {
                    result->command_id = mn_result->command_id[0];
                    result->phrase = mn_result->string;
                    result->prob = mn_result->prob[0];
                    return true;
                }
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                /* Reset detection state for continuous monitoring */
                ops->multinet->clean(ops->mn_data);
            }
        }
    }

    return false;
}

void multinet_ops_clear(multinet_ops_t *ops) {
    if (ops) {
        ops->buffer_pos = 0;
        if (ops->mn_data && ops->multinet) {
            ops->multinet->clean(ops->mn_data);
        }
    }
}

int multinet_ops_set_threshold(multinet_ops_t *ops, float threshold) {
    if (!ops || !ops->mn_data) return -1;
    ops->multinet->set_det_threshold(ops->mn_data, threshold);
    return 0;
}

int multinet_ops_get_chunk_samples(multinet_ops_t *ops) {
    return ops ? ops->chunk_samples : 0;
}

int multinet_ops_get_sample_rate(multinet_ops_t *ops) {
    return ops ? ops->sample_rate : 0;
}

bool multinet_ops_is_ready(multinet_ops_t *ops) {
    return ops ? ops->commands_ready : false;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 */
