/*
 * ESP-SR Audio Layer Implementation
 * 
 * This file implements I2S + AFE + WakeNet management for
 * the MicroPython esp-sr module.
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#include "espsr_audio.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

// MicroPython printf for debug output (visible in REPL)
#include "py/runtime.h"
#include "py/mpprint.h"

// Debug logging macros using mp_printf (visible in Thonny/REPL)
#define ESPSR_LOGI(fmt, ...) mp_printf(&mp_plat_print, "[ESPSR] " fmt "\n", ##__VA_ARGS__)
#define ESPSR_LOGE(fmt, ...) mp_printf(&mp_plat_print, "[ESPSR ERROR] " fmt "\n", ##__VA_ARGS__)
#define ESPSR_LOGW(fmt, ...) mp_printf(&mp_plat_print, "[ESPSR WARN] " fmt "\n", ##__VA_ARGS__)

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"

// WakeNet direct API (no AFE)
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"


// Task configuration
#define ESPSR_TASK_STACK_SIZE   (4096)
#define ESPSR_TASK_PRIORITY     (5)
#define ESPSR_TASK_CORE         (1)  // Run on core 1 to avoid blocking MicroPython

// Audio configuration
#define ESPSR_SAMPLE_RATE       (16000)
#define ESPSR_SAMPLE_BITS       (16)
#define ESPSR_DMA_BUF_COUNT     (4)
#define ESPSR_DMA_BUF_LEN       (256)

// Module state
typedef struct {
    // State
    espsr_state_t state;
    
    // I2S
    i2s_chan_handle_t i2s_handle;
    espsr_i2s_config_t i2s_config;
    
    // WakeNet (direct mode, no AFE)
    srmodel_list_t *models;
    const esp_wn_iface_t *wakenet;
    model_iface_data_t *wn_data;  // WakeNet instance data
    char *wn_model_name;          // Model name for create()
    char *wake_word_name;
    float threshold;
    
    // Detection task
    TaskHandle_t detect_task_handle;
    volatile bool task_running;
    
    // WakeNet parameters
    int audio_chunksize;
    
    // Callback
    espsr_wake_callback_t callback;
    void *callback_user_data;
} espsr_audio_ctx_t;

static espsr_audio_ctx_t *s_ctx = NULL;

// Task stack sizes and priorities
#define ESPSR_DETECT_TASK_STACK_SIZE  (8 * 1024)

// Forward declarations
static void espsr_detect_task(void *arg);
static int espsr_i2s_init(const espsr_i2s_config_t *config);
static void espsr_i2s_deinit(void);
static int espsr_wakenet_init(int debug);
static void espsr_wakenet_deinit(void);

/**
 * Initialize the esp-sr audio subsystem
 */
int espsr_audio_init(void) {
    ESPSR_LOGI("audio_init: starting");
    
    if (s_ctx != NULL) {
        ESPSR_LOGW("Already initialized");
        return 0;
    }
    
    // Allocate context
    s_ctx = calloc(1, sizeof(espsr_audio_ctx_t));
    if (s_ctx == NULL) {
        ESPSR_LOGE("Failed to allocate context");
        return -1;
    }
    
    // Initialize models from "model" partition
    // This scans the flash partition for available models
    ESPSR_LOGI("audio_init: loading models from 'model' partition...");
    s_ctx->models = esp_srmodel_init("model");
    if (s_ctx->models == NULL) {
        ESPSR_LOGE("Failed to init models from partition - check partition table and flash content");
        free(s_ctx);
        s_ctx = NULL;
        return -1;
    }
    ESPSR_LOGI("audio_init: models loaded");
    
    // Find WakeNet model
    char *wn_name = esp_srmodel_filter(s_ctx->models, ESP_WN_PREFIX, NULL);
    if (wn_name == NULL) {
        ESPSR_LOGE("No WakeNet model found");
        esp_srmodel_deinit(s_ctx->models);
        free(s_ctx);
        s_ctx = NULL;
        return -1;
    }
    
    ESPSR_LOGI("Found WakeNet model: %s", wn_name);
    
    // Save model name for later use
    s_ctx->wn_model_name = wn_name;
    
    // Get WakeNet handle
    s_ctx->wakenet = esp_wn_handle_from_name(wn_name);
    if (s_ctx->wakenet == NULL) {
        ESPSR_LOGE("Failed to get WakeNet handle");
        esp_srmodel_deinit(s_ctx->models);
        free(s_ctx);
        s_ctx = NULL;
        return -1;
    }
    
    // Get wake word name
    s_ctx->wake_word_name = esp_wn_wakeword_from_name(wn_name);
    ESPSR_LOGI("Wake word: %s", s_ctx->wake_word_name ? s_ctx->wake_word_name : "unknown");
    
    // Set default threshold
    s_ctx->threshold = 0.9f;
    s_ctx->state = ESPSR_STATE_IDLE;
    
    ESPSR_LOGI("ESP-SR audio initialized");
    return 0;
}

/**
 * Deinitialize the esp-sr audio subsystem
 */
void espsr_audio_deinit(void) {
    if (s_ctx == NULL) {
        return;
    }
    
    // Stop detection if running
    espsr_audio_pause();
    
    // Cleanup
    if (s_ctx->models) {
        esp_srmodel_deinit(s_ctx->models);
    }
    
    free(s_ctx);
    s_ctx = NULL;
    
    ESPSR_LOGI("ESP-SR audio deinitialized");
}

/**
 * Set the wake word detection callback
 * Note: No mutex needed here - Python calls are single-threaded
 */
void espsr_audio_set_callback(espsr_wake_callback_t callback, void *user_data) {
    if (s_ctx == NULL) {
        return;
    }
    
    s_ctx->callback = callback;
    s_ctx->callback_user_data = user_data;
}

/**
 * Resume wake word detection
 * Note: No mutex needed - Python calls are single-threaded
 */
int espsr_audio_resume(const espsr_i2s_config_t *config) {
    ESPSR_LOGI("resume: entering");
    
    if (s_ctx == NULL) {
        ESPSR_LOGE("Not initialized");
        return -1;
    }
    
    if (s_ctx->state == ESPSR_STATE_RUNNING) {
        ESPSR_LOGW("Already running");
        return 0;
    }
    if(config->debug == 1) {
        return -1;
    }
    // Save config
    memcpy(&s_ctx->i2s_config, config, sizeof(espsr_i2s_config_t));
    if (s_ctx->i2s_config.sample_rate == 0) {
        s_ctx->i2s_config.sample_rate = ESPSR_SAMPLE_RATE;
    }
    
    ESPSR_LOGI("resume: initializing I2S (port=%d, mode=%s, sck=%d, ws=%d, sd=%d)",
             config->i2s_id,
             config->mode == ESPSR_I2S_MODE_PDM ? "PDM" : "STD",
             config->sck_pin, config->ws_pin, config->sd_pin);
    
    if(config->debug == 2) {
        return -1;
    }
    // Initialize I2S
    int ret = espsr_i2s_init(&s_ctx->i2s_config);
    if (ret != 0) {
        ESPSR_LOGE("Failed to init I2S");
        return ret;
    }
    ESPSR_LOGI("resume: I2S initialized OK");
    if(config->debug == 3) {
        return -1;
    }
    // Initialize WakeNet directly (no AFE)
    ESPSR_LOGI("resume: initializing WakeNet (this may take a few seconds...)");
    ret = espsr_wakenet_init(config->debug);
    if (ret != 0) {
        ESPSR_LOGE("Failed to init WakeNet");
        espsr_i2s_deinit();
        return ret;
    }
    ESPSR_LOGI("resume: WakeNet initialized OK");
    if(config->debug == 4) {
        return -1;
    }
    
    // Start detection task
    ESPSR_LOGI("resume: creating detection task");
    s_ctx->task_running = true;
    
    BaseType_t xret = xTaskCreatePinnedToCore(
        espsr_detect_task,
        "espsr_detect",
        ESPSR_DETECT_TASK_STACK_SIZE,
        s_ctx,
        ESPSR_TASK_PRIORITY,
        &s_ctx->detect_task_handle,
        ESPSR_TASK_CORE  // Core 1
    );
    
    if (xret != pdPASS) {
        ESPSR_LOGE("Failed to create detect task");
        espsr_wakenet_deinit();
        espsr_i2s_deinit();
        s_ctx->task_running = false;
        return -1;
    }
    
    s_ctx->state = ESPSR_STATE_RUNNING;
    
    ESPSR_LOGI("resume: Wake word detection started successfully");
    return 0;
}

/**
 * Pause wake word detection
 * Note: No mutex needed - Python calls are single-threaded,
 * task_running is volatile for safe cross-thread access
 */
int espsr_audio_pause(void) {
    if (s_ctx == NULL) {
        return -1;
    }
    
    if (s_ctx->state != ESPSR_STATE_RUNNING) {
        return 0;
    }
    
    // Signal task to stop (volatile flag)
    s_ctx->task_running = false;
    
    // Wait for task to finish
    vTaskDelay(pdMS_TO_TICKS(200));
    
    s_ctx->detect_task_handle = NULL;
    
    // Cleanup WakeNet
    espsr_wakenet_deinit();
    
    // Release I2S
    espsr_i2s_deinit();
    
    s_ctx->state = ESPSR_STATE_PAUSED;
    
    ESPSR_LOGI("Wake word detection paused, I2S released");
    return 0;
}

/**
 * Get current detection state
 */
espsr_state_t espsr_audio_get_state(void) {
    if (s_ctx == NULL) {
        return ESPSR_STATE_IDLE;
    }
    return s_ctx->state;
}

/**
 * Set wake word detection threshold
 * Note: No mutex needed - Python calls are single-threaded
 */
int espsr_audio_set_threshold(float threshold) {
    if (s_ctx == NULL) {
        return -1;
    }
    
    if (threshold < 0.5f || threshold > 0.99f) {
        ESPSR_LOGE("Threshold must be between 0.5 and 0.99");
        return -1;
    }
    
    s_ctx->threshold = threshold;
    
    // Update WakeNet threshold if running
    if (s_ctx->wakenet && s_ctx->wn_data) {
        s_ctx->wakenet->set_det_threshold(s_ctx->wn_data, threshold);
    }
    
    return 0;
}

/**
 * Get the current wake word name
 */
const char *espsr_audio_get_wake_word(void) {
    if (s_ctx == NULL || s_ctx->wake_word_name == NULL) {
        return "unknown";
    }
    return s_ctx->wake_word_name;
}

// ============================================================================
// Internal functions
// ============================================================================

/**
 * Initialize I2S for audio capture
 */
static int espsr_i2s_init(const espsr_i2s_config_t *config) {
    esp_err_t ret;
    
    if (config->mode == ESPSR_I2S_MODE_PDM) {
        // PDM RX mode configuration
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
            config->i2s_id == 0 ? I2S_NUM_0 : I2S_NUM_1,
            I2S_ROLE_MASTER
        );
        chan_cfg.dma_desc_num = ESPSR_DMA_BUF_COUNT;
        chan_cfg.dma_frame_num = ESPSR_DMA_BUF_LEN;
        
        ret = i2s_new_channel(&chan_cfg, NULL, &s_ctx->i2s_handle);
        if (ret != ESP_OK) {
            ESPSR_LOGE("Failed to create I2S channel: %s", esp_err_to_name(ret));
            return -1;
        }
        
        i2s_pdm_rx_config_t pdm_cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(config->sample_rate),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = config->sck_pin,
                .din = config->sd_pin,
                .invert_flags = {
                    .clk_inv = false,
                },
            },
        };
        
        ret = i2s_channel_init_pdm_rx_mode(s_ctx->i2s_handle, &pdm_cfg);
        if (ret != ESP_OK) {
            ESPSR_LOGE("Failed to init PDM RX: %s", esp_err_to_name(ret));
            i2s_del_channel(s_ctx->i2s_handle);
            s_ctx->i2s_handle = NULL;
            return -1;
        }
    } else {
        // Standard I2S RX mode configuration
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
            config->i2s_id == 0 ? I2S_NUM_0 : I2S_NUM_1,
            I2S_ROLE_MASTER
        );
        chan_cfg.dma_desc_num = ESPSR_DMA_BUF_COUNT;
        chan_cfg.dma_frame_num = ESPSR_DMA_BUF_LEN;
        
        ret = i2s_new_channel(&chan_cfg, NULL, &s_ctx->i2s_handle);
        if (ret != ESP_OK) {
            ESPSR_LOGE("Failed to create I2S channel: %s", esp_err_to_name(ret));
            return -1;
        }
        
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = config->sck_pin,
                .ws = config->ws_pin,
                .dout = I2S_GPIO_UNUSED,
                .din = config->sd_pin,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        
        ret = i2s_channel_init_std_mode(s_ctx->i2s_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESPSR_LOGE("Failed to init STD RX: %s", esp_err_to_name(ret));
            i2s_del_channel(s_ctx->i2s_handle);
            s_ctx->i2s_handle = NULL;
            return -1;
        }
    }
    
    // Enable I2S channel
    ret = i2s_channel_enable(s_ctx->i2s_handle);
    if (ret != ESP_OK) {
        ESPSR_LOGE("Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_ctx->i2s_handle);
        s_ctx->i2s_handle = NULL;
        return -1;
    }
    
    ESPSR_LOGI("I2S initialized: port=%d, mode=%s, sck=%d, ws=%d, sd=%d",
             config->i2s_id,
             config->mode == ESPSR_I2S_MODE_PDM ? "PDM" : "STD",
             config->sck_pin, config->ws_pin, config->sd_pin);
    
    return 0;
}

/**
 * Deinitialize I2S
 */
static void espsr_i2s_deinit(void) {
    if (s_ctx->i2s_handle) {
        i2s_channel_disable(s_ctx->i2s_handle);
        i2s_del_channel(s_ctx->i2s_handle);
        s_ctx->i2s_handle = NULL;
        ESPSR_LOGI("I2S deinitialized");
    }
}

/**
 * Initialize WakeNet directly (no AFE)
 */
static int espsr_wakenet_init(int debug) {
    ESPSR_LOGI("wakenet_init: starting");
    if(debug == 5) {
        return -1;
    }
    
    ESPSR_LOGI("wakenet_init: Using model: %s", s_ctx->wn_model_name);
    if(debug == 6) {
        return -1;
    }
    
    // Check available memory before loading neural network
    ESPSR_LOGI("wakenet_init: Free heap: %lu, Free PSRAM: %lu",
               (unsigned long)esp_get_free_heap_size(),
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    if(debug == 7) {
        return -1;
    }
    
    // Create WakeNet instance directly
    // DET_MODE_90 = 90% detection mode (balanced sensitivity)
    ESPSR_LOGI("wakenet_init: calling wakenet->create()...");
    if(debug == 8) {
        return -1;
    }
    
    s_ctx->wn_data = s_ctx->wakenet->create(s_ctx->wn_model_name, DET_MODE_90);
    
    ESPSR_LOGI("wakenet_init: wakenet->create() returned");
    if(debug == 9) {
        return -1;
    }
    
    if (s_ctx->wn_data == NULL) {
        ESPSR_LOGE("Failed to create WakeNet instance");
        return -1;
    }
    
    // Get audio chunk size
    s_ctx->audio_chunksize = s_ctx->wakenet->get_samp_chunksize(s_ctx->wn_data);
    int sample_rate = s_ctx->wakenet->get_samp_rate(s_ctx->wn_data);
    
    ESPSR_LOGI("wakenet_init: chunksize=%d, sample_rate=%d",
               s_ctx->audio_chunksize, sample_rate);
    
    if(debug == 10) {
        return -1;
    }
    
    // Set detection threshold
    s_ctx->wakenet->set_det_threshold(s_ctx->wn_data, s_ctx->threshold);
    
    ESPSR_LOGI("wakenet_init: WakeNet initialization complete");
    return 0;
}

/**
 * Deinitialize WakeNet
 */
static void espsr_wakenet_deinit(void) {
    if (s_ctx->wn_data && s_ctx->wakenet) {
        s_ctx->wakenet->destroy(s_ctx->wn_data);
        s_ctx->wn_data = NULL;
        ESPSR_LOGI("WakeNet deinitialized");
    }
}

/**
 * Detection task - reads audio from I2S and runs WakeNet detection
 */
static void espsr_detect_task(void *arg) {
    espsr_audio_ctx_t *ctx = (espsr_audio_ctx_t *)arg;
    
    int chunksize = ctx->audio_chunksize;
    int sample_rate = ctx->wakenet->get_samp_rate(ctx->wn_data);
    int chunk_ms = (chunksize * 1000) / sample_rate;
    
    // Allocate audio buffer
    size_t buff_size = chunksize * sizeof(int16_t);
    int16_t *audio_buff = heap_caps_malloc(buff_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio_buff == NULL) {
        audio_buff = malloc(buff_size);
    }
    
    if (audio_buff == NULL) {
        ESPSR_LOGE("Detect task: failed to allocate audio buffer (%d bytes)", buff_size);
        ctx->task_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    ESPSR_LOGI("Detect task started: chunksize=%d, sample_rate=%d, chunk_ms=%d",
               chunksize, sample_rate, chunk_ms);
    
    size_t bytes_read = 0;
    int read_size = chunksize * sizeof(int16_t);
    
    while (ctx->task_running) {
        // Read audio from I2S
        esp_err_t ret = i2s_channel_read(ctx->i2s_handle, audio_buff,
                                         read_size, &bytes_read,
                                         pdMS_TO_TICKS(100));
        
        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Run WakeNet detection directly
        int result = ctx->wakenet->detect(ctx->wn_data, audio_buff);
        
        // Check for wake word detection
        if (result > 0) {
            ESPSR_LOGI(">>> Wake word '%s' detected! (result=%d) <<<", 
                       ctx->wake_word_name, result);
            
            // Call Python callback if set
            if (ctx->callback) {
                ctx->callback(ctx->wake_word_name, ctx->callback_user_data);
            }
        }
        
        // Small delay to avoid CPU spinning
        if (chunk_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(chunk_ms / 2));
        }
    }
    
    free(audio_buff);
    ESPSR_LOGI("Detect task exiting");
    vTaskDelete(NULL);
}
