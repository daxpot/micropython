/*
 * ESP-SR MicroPython Module (Feed Mode)
 *
 * Python feeds PCM data to WakeNet for wake word detection.
 * No I2S management in C layer — Python fully controls audio I/O.
 *
 * Usage:
 *   import espsrmod
 *
 *   wn = espsrmod.WakeNet()
 *   print(wn.wake_word)       # e.g. "xiaoyutongxue"
 *   print(wn.chunk_samples)   # e.g. 480
 *
 *   # In your audio loop:
 *   audio_in.readinto(buf)
 *   result = wn.detect(buf)
 *   if result:
 *       print("Detected:", result)
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#include "py/obj.h"
#include "py/runtime.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)

#include <string.h>
#include "wakenet_ops.h"

// ============================================================================
// WakeNet class
// ============================================================================

typedef struct _espsrmod_wakenet_obj_t {
    mp_obj_base_t base;
    wakenet_ops_t *ops;
} espsrmod_wakenet_obj_t;

// Cache the C-heap ops pointer (survives GC and soft resets).
// The MicroPython wrapper object lives on the GC heap and may be collected,
// but the heavy WakeNet model stays loaded until explicit deinit().
static wakenet_ops_t *s_ops = NULL;

// -- Constructor -------------------------------------------------------------

static mp_obj_t espsrmod_wakenet_make_new(const mp_obj_type_t *type,
                                           size_t n_args, size_t n_kw,
                                           const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    // Always create a fresh MicroPython wrapper (old one may have been GC'd)
    espsrmod_wakenet_obj_t *self = mp_obj_malloc(espsrmod_wakenet_obj_t, type);

    if (s_ops == NULL) {
        // First time — load model from flash
        s_ops = wakenet_ops_create();
        if (!s_ops) {
            mp_raise_msg(&mp_type_RuntimeError,
                         MP_ERROR_TEXT("Failed to initialize WakeNet"));
        }
    } else {
        // Reuse existing model, just reset the buffer
        wakenet_ops_clear(s_ops);
    }

    self->ops = s_ops;
    return MP_OBJ_FROM_PTR(self);
}

// -- Methods -----------------------------------------------------------------

// detect(pcm_data) -> str | None
static mp_obj_t espsrmod_wakenet_detect(mp_obj_t self_in, mp_obj_t pcm_in) {
    espsrmod_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ops) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("WakeNet not initialized"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(pcm_in, &bufinfo, MP_BUFFER_READ);

    if (bufinfo.len < 2 || bufinfo.len % 2 != 0) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("PCM data must contain 16-bit aligned samples"));
    }

    int num_samples = bufinfo.len / sizeof(int16_t);
    const char *result = wakenet_ops_detect(
        self->ops, (const int16_t *)bufinfo.buf, num_samples);

    if (result) {
        return mp_obj_new_str(result, strlen(result));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espsrmod_wakenet_detect_obj,
                                  espsrmod_wakenet_detect);

// clear() — reset internal accumulation buffer
static mp_obj_t espsrmod_wakenet_clear(mp_obj_t self_in) {
    espsrmod_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ops) {
        wakenet_ops_clear(self->ops);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsrmod_wakenet_clear_obj,
                                  espsrmod_wakenet_clear);

// set_threshold(threshold)
static mp_obj_t espsrmod_wakenet_set_threshold(mp_obj_t self_in,
                                                mp_obj_t threshold_in) {
    espsrmod_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ops) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("WakeNet not initialized"));
    }

    float threshold = mp_obj_get_float(threshold_in);
    if (threshold < 0.5f || threshold > 0.99f) {
        mp_raise_ValueError(MP_ERROR_TEXT("threshold must be 0.5 ~ 0.99"));
    }

    wakenet_ops_set_threshold(self->ops, threshold);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espsrmod_wakenet_set_threshold_obj,
                                  espsrmod_wakenet_set_threshold);

// deinit() — explicitly release WakeNet model resources
static mp_obj_t espsrmod_wakenet_deinit(mp_obj_t self_in) {
    espsrmod_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ops) {
        wakenet_ops_destroy(self->ops);
        if (self->ops == s_ops) {
            s_ops = NULL;
        }
        self->ops = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsrmod_wakenet_deinit_obj,
                                  espsrmod_wakenet_deinit);

// -- Read-only attributes ----------------------------------------------------

static void espsrmod_wakenet_attr(mp_obj_t self_in, qstr attr,
                                   mp_obj_t *dest) {
    espsrmod_wakenet_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Only handle load (read) operations
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }

    if (!self->ops) {
        // Fall through to locals_dict for method lookups
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }

    if (attr == MP_QSTR_chunk_samples) {
        dest[0] = mp_obj_new_int(wakenet_ops_get_chunk_samples(self->ops));
    } else if (attr == MP_QSTR_chunk_bytes) {
        dest[0] = mp_obj_new_int(
            wakenet_ops_get_chunk_samples(self->ops) * sizeof(int16_t));
    } else if (attr == MP_QSTR_sample_rate) {
        dest[0] = mp_obj_new_int(wakenet_ops_get_sample_rate(self->ops));
    } else if (attr == MP_QSTR_wake_word) {
        const char *word = wakenet_ops_get_wake_word(self->ops);
        dest[0] = mp_obj_new_str(word, strlen(word));
    } else {
        // Not a property — signal MicroPython to look in locals_dict
        dest[1] = MP_OBJ_SENTINEL;
    }
}

// -- Type definition ---------------------------------------------------------

static const mp_rom_map_elem_t espsrmod_wakenet_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_detect),        MP_ROM_PTR(&espsrmod_wakenet_detect_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),         MP_ROM_PTR(&espsrmod_wakenet_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_threshold), MP_ROM_PTR(&espsrmod_wakenet_set_threshold_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),        MP_ROM_PTR(&espsrmod_wakenet_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(espsrmod_wakenet_locals_dict,
                             espsrmod_wakenet_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espsrmod_wakenet_type,
    MP_QSTR_WakeNet,
    MP_TYPE_FLAG_NONE,
    make_new, espsrmod_wakenet_make_new,
    attr, espsrmod_wakenet_attr,
    locals_dict, &espsrmod_wakenet_locals_dict
);

// ============================================================================
// Module definition
// ============================================================================

static const mp_rom_map_elem_t espsrmod_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsrmod) },
    { MP_ROM_QSTR(MP_QSTR_WakeNet),  MP_ROM_PTR(&espsrmod_wakenet_type) },
};
static MP_DEFINE_CONST_DICT(espsrmod_module_globals,
                             espsrmod_module_globals_table);

const mp_obj_module_t espsrmod_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsrmod_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espsrmod, espsrmod_module);

#endif // CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
