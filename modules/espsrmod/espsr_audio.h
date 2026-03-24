/*
 * ESP-SR Audio Layer Header
 * 
 * This file provides the I2S + AFE + WakeNet management interface
 * for the MicroPython esp-sr module.
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#ifndef ESPSR_AUDIO_H
#define ESPSR_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// I2S mode configuration
typedef enum {
    ESPSR_I2S_MODE_STD = 0,    // Standard I2S mode
    ESPSR_I2S_MODE_PDM = 1,    // PDM mode
} espsr_i2s_mode_t;

// I2S configuration structure
typedef struct {
    int i2s_id;                 // I2S port number (0 or 1)
    int sck_pin;                // Serial clock pin (BCLK for STD, CLK for PDM)
    int ws_pin;                 // Word select pin (LRCK for STD, not used for PDM mono)
    int sd_pin;                 // Serial data pin (DIN)
    espsr_i2s_mode_t mode;      // I2S mode (STD or PDM)
    int sample_rate;            // Sample rate in Hz (default: 16000)
    int debug;
} espsr_i2s_config_t;

// Wake word detection callback type
// Parameters:
//   wake_word: The detected wake word name (e.g., "xiaoyutongxue")
//   user_data: User-provided context pointer
typedef void (*espsr_wake_callback_t)(const char *wake_word, void *user_data);

// Audio state
typedef enum {
    ESPSR_STATE_IDLE = 0,       // Not initialized
    ESPSR_STATE_RUNNING,        // Actively detecting
    ESPSR_STATE_PAUSED,         // Paused, I2S released
} espsr_state_t;

/**
 * @brief Initialize the esp-sr audio subsystem
 * 
 * This must be called once before using other functions.
 * It initializes the model list from the "model" partition.
 * 
 * @return 0 on success, negative error code on failure
 */
int espsr_audio_init(void);

/**
 * @brief Deinitialize the esp-sr audio subsystem
 * 
 * Releases all resources including models.
 */
void espsr_audio_deinit(void);

/**
 * @brief Set the wake word detection callback
 * 
 * @param callback Function to call when wake word is detected
 * @param user_data User context passed to callback
 */
void espsr_audio_set_callback(espsr_wake_callback_t callback, void *user_data);

/**
 * @brief Resume wake word detection
 * 
 * Initializes I2S with the given configuration and starts
 * the AFE + WakeNet detection task.
 * 
 * @param config I2S configuration
 * @return 0 on success, negative error code on failure
 */
int espsr_audio_resume(const espsr_i2s_config_t *config);

/**
 * @brief Pause wake word detection
 * 
 * Stops the detection task and releases I2S resources.
 * After calling this, you can use the I2S port for other purposes.
 * 
 * @return 0 on success, negative error code on failure
 */
int espsr_audio_pause(void);

/**
 * @brief Get current detection state
 * 
 * @return Current state (IDLE, RUNNING, or PAUSED)
 */
espsr_state_t espsr_audio_get_state(void);

/**
 * @brief Set wake word detection threshold
 * 
 * @param threshold Detection threshold (0.5 ~ 0.99, higher = stricter)
 * @return 0 on success, negative error code on failure
 */
int espsr_audio_set_threshold(float threshold);

/**
 * @brief Get the current wake word name
 * 
 * @return Wake word name string (e.g., "xiaoyutongxue")
 */
const char *espsr_audio_get_wake_word(void);

#ifdef __cplusplus
}
#endif

#endif // ESPSR_AUDIO_H
