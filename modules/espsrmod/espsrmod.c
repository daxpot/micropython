/*
 * ESP-SR MicroPython Module
 * 
 * This file provides the Python bindings for the esp-sr wake word
 * detection functionality.
 *
 * Usage:
 *   import espsr
 *   
 *   def on_wakeup(word):
 *       print("Wake word detected:", word)
 *       wn.pause()  # Release I2S
 *   
 *   wn = espsr.WakeNet(callback=on_wakeup)
 *   wn.resume(i2s_id=0, sck=12, ws=13, sd=14, pdm=True)
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "py/mperrno.h"

#include "espsr_audio.h"

// Only compile for ESP32-S3 and ESP32-P4
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
// WakeNet class
// ============================================================================

typedef struct _espsr_wakenet_obj_t {
    mp_obj_base_t base;
    mp_obj_t callback;
    bool initialized;
} espsr_wakenet_obj_t;

// Global instance (singleton pattern)
static espsr_wakenet_obj_t *s_wakenet_instance = NULL;

/**
 * Callback from C layer - schedules Python callback
 */
static void espsr_mp_callback(const char *wake_word, void *user_data) {
    espsr_wakenet_obj_t *self = (espsr_wakenet_obj_t *)user_data;
    
    if (self == NULL || self->callback == mp_const_none) {
        return;
    }
    
    // Schedule callback to run in MicroPython context
    mp_obj_t word_str = mp_obj_new_str(wake_word, strlen(wake_word));
    mp_sched_schedule(self->callback, word_str);
    
    // Wake up main task to process scheduled callback
    mp_hal_wake_main_task_from_isr();
}

/**
 * WakeNet constructor
 * 
 * Args:
 *   callback: Function to call when wake word is detected
 */
static mp_obj_t espsr_wakenet_make_new(const mp_obj_type_t *type,
                                        size_t n_args, size_t n_kw,
                                        const mp_obj_t *args) {
    // Parse arguments
    enum { ARG_callback };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_callback, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args),
                              allowed_args, vals);
    
    // Check singleton
    if (s_wakenet_instance != NULL) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("WakeNet already created, use existing instance"));
    }
    
    // Create object
    espsr_wakenet_obj_t *self = mp_obj_malloc(espsr_wakenet_obj_t, type);
    self->callback = vals[ARG_callback].u_obj;
    self->initialized = false;
    
    // Validate callback
    if (self->callback != mp_const_none && !mp_obj_is_callable(self->callback)) {
        mp_raise_ValueError(MP_ERROR_TEXT("callback must be callable"));
    }
    
    // Initialize audio subsystem
    int ret = espsr_audio_init();
    if (ret != 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to initialize esp-sr"));
    }
    
    // Set callback
    espsr_audio_set_callback(espsr_mp_callback, self);
    self->initialized = true;
    
    // Store instance
    s_wakenet_instance = self;
    
    return MP_OBJ_FROM_PTR(self);
}

/**
 * WakeNet.resume(i2s_id, sck, ws, sd, pdm=True, sample_rate=16000)
 * 
 * Initialize I2S and start wake word detection.
 * Must call this after pause() or on first use.
 */
static mp_obj_t espsr_wakenet_resume(size_t n_args, const mp_obj_t *pos_args,
                                      mp_map_t *kw_args) {
    enum { ARG_self, ARG_i2s_id, ARG_sck, ARG_ws, ARG_sd, ARG_pdm, ARG_sample_rate, ARG_debug };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_,           MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_i2s_id,     MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_sck,        MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_ws,         MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_sd,         MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_pdm,        MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16000} },
        { MP_QSTR_debug,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
                     allowed_args, vals);
    
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(vals[ARG_self].u_obj);
    
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("WakeNet not initialized"));
    }
    
    // Build config
    espsr_i2s_config_t config = {
        .i2s_id = vals[ARG_i2s_id].u_int,
        .sck_pin = vals[ARG_sck].u_int,
        .ws_pin = vals[ARG_ws].u_int,
        .sd_pin = vals[ARG_sd].u_int,
        .mode = vals[ARG_pdm].u_bool ? ESPSR_I2S_MODE_PDM : ESPSR_I2S_MODE_STD,
        .sample_rate = vals[ARG_sample_rate].u_int,
        .debug = vals[ARG_debug].u_int,
    };
    mp_printf(&mp_plat_print, "i2s_id=%d, sck=%d, ws=%d, sd=%d, mode=%s, sample_rate=%d, debug=%d\n", config.i2s_id, config.sck_pin, config.ws_pin, config.sd_pin, config.mode == ESPSR_I2S_MODE_PDM ? "PDM" : "STD", config.sample_rate, config.debug);
    
    // Validate I2S port
    if (config.i2s_id < 0 || config.i2s_id > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("i2s_id must be 0 or 1"));
    }
    
    // Resume detection
    int ret = espsr_audio_resume(&config);
    if (ret != 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to start wake word detection"));
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espsr_wakenet_resume_obj, 5, espsr_wakenet_resume);

/**
 * WakeNet.pause()
 * 
 * Stop wake word detection and release I2S resources.
 * After calling this, you can use the I2S port for other purposes.
 */
static mp_obj_t espsr_wakenet_pause(mp_obj_t self_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized) {
        return mp_const_none;
    }
    
    int ret = espsr_audio_pause();
    if (ret != 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to pause wake word detection"));
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_wakenet_pause_obj, espsr_wakenet_pause);

/**
 * WakeNet.set_threshold(threshold)
 * 
 * Set detection threshold (0.5 ~ 0.99).
 * Higher values = stricter detection (fewer false positives).
 */
static mp_obj_t espsr_wakenet_set_threshold(mp_obj_t self_in, mp_obj_t threshold_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("WakeNet not initialized"));
    }
    
    float threshold = mp_obj_get_float(threshold_in);
    
    if (threshold < 0.5f || threshold > 0.99f) {
        mp_raise_ValueError(MP_ERROR_TEXT("threshold must be between 0.5 and 0.99"));
    }
    
    int ret = espsr_audio_set_threshold(threshold);
    if (ret != 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to set threshold"));
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espsr_wakenet_set_threshold_obj, espsr_wakenet_set_threshold);

/**
 * WakeNet.get_wake_word()
 * 
 * Get the name of the configured wake word.
 */
static mp_obj_t espsr_wakenet_get_wake_word(mp_obj_t self_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("WakeNet not initialized"));
    }
    
    const char *word = espsr_audio_get_wake_word();
    return mp_obj_new_str(word, strlen(word));
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_wakenet_get_wake_word_obj, espsr_wakenet_get_wake_word);

/**
 * WakeNet.is_running()
 * 
 * Check if wake word detection is currently running.
 */
static mp_obj_t espsr_wakenet_is_running(mp_obj_t self_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (!self->initialized) {
        return mp_const_false;
    }
    
    espsr_state_t state = espsr_audio_get_state();
    return mp_obj_new_bool(state == ESPSR_STATE_RUNNING);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_wakenet_is_running_obj, espsr_wakenet_is_running);

/**
 * WakeNet.deinit()
 * 
 * Completely deinitialize the wake word detection system.
 * After this, you need to create a new WakeNet instance.
 */
static mp_obj_t espsr_wakenet_deinit(mp_obj_t self_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (self->initialized) {
        espsr_audio_deinit();
        self->initialized = false;
    }
    
    // Clear singleton
    if (s_wakenet_instance == self) {
        s_wakenet_instance = NULL;
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_wakenet_deinit_obj, espsr_wakenet_deinit);

/**
 * WakeNet.set_callback(callback)
 * 
 * Set or update the wake word detection callback.
 */
static mp_obj_t espsr_wakenet_set_callback(mp_obj_t self_in, mp_obj_t callback_in) {
    espsr_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (callback_in != mp_const_none && !mp_obj_is_callable(callback_in)) {
        mp_raise_ValueError(MP_ERROR_TEXT("callback must be callable"));
    }
    
    self->callback = callback_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espsr_wakenet_set_callback_obj, espsr_wakenet_set_callback);

// WakeNet locals dict
static const mp_rom_map_elem_t espsr_wakenet_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_resume),        MP_ROM_PTR(&espsr_wakenet_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause),         MP_ROM_PTR(&espsr_wakenet_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_threshold), MP_ROM_PTR(&espsr_wakenet_set_threshold_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_wake_word), MP_ROM_PTR(&espsr_wakenet_get_wake_word_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running),    MP_ROM_PTR(&espsr_wakenet_is_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),        MP_ROM_PTR(&espsr_wakenet_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_callback),  MP_ROM_PTR(&espsr_wakenet_set_callback_obj) },
};
static MP_DEFINE_CONST_DICT(espsr_wakenet_locals_dict, espsr_wakenet_locals_dict_table);

// WakeNet type definition
MP_DEFINE_CONST_OBJ_TYPE(
    espsr_wakenet_type,
    MP_QSTR_WakeNet,
    MP_TYPE_FLAG_NONE,
    make_new, espsr_wakenet_make_new,
    locals_dict, &espsr_wakenet_locals_dict
);

// ============================================================================
// Module definition
// ============================================================================

// Module globals
static const mp_rom_map_elem_t espsr_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsr) },
    { MP_ROM_QSTR(MP_QSTR_WakeNet),  MP_ROM_PTR(&espsr_wakenet_type) },
};
static MP_DEFINE_CONST_DICT(espsr_module_globals, espsr_module_globals_table);

// Module definition
const mp_obj_module_t espsr_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsr_module_globals,
};

// Register module
MP_REGISTER_MODULE(MP_QSTR_espsr, espsr_module);

#endif // CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
