/*
 * MultiNet MicroPython Module
 *
 * Provides custom wake word / speech command recognition via pinyin.
 * Python controls audio I/O and feeds PCM data to MultiNet for detection.
 *
 * Usage:
 *   import multinetmod
 *
 *   mn = multinetmod.MultiNet()
 *   mn.add_command(1, "xiao yu tong xue")
 *   mn.add_command(2, "da kai deng")
 *   mn.update()
 *
 *   # In audio loop:
 *   audio_in.readinto(buf)
 *   result = mn.detect(buf)
 *   if result:
 *       print(result)  # {"command_id": 1, "phrase": "xiao yu tong xue", "prob": 0.95}
 *
 * SPDX-License-Identifier: MIT
 */

#include "py/obj.h"
#include "py/runtime.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)

#include <string.h>
#include "multinet_ops.h"

/* ========================================================================== */
/* MultiNet class                                                             */
/* ========================================================================== */

typedef struct _multinetmod_obj_t {
    mp_obj_base_t base;
    multinet_ops_t *ops;
} multinetmod_obj_t;

/* Singleton: model survives GC and soft resets */
static multinet_ops_t *s_ops = NULL;

/* -- Constructor ----------------------------------------------------------- */

static mp_obj_t multinetmod_make_new(const mp_obj_type_t *type,
                                      size_t n_args, size_t n_kw,
                                      const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    multinetmod_obj_t *self = mp_obj_malloc(multinetmod_obj_t, type);

    if (s_ops == NULL) {
        s_ops = multinet_ops_create();
        if (!s_ops) {
            mp_raise_msg(&mp_type_RuntimeError,
                         MP_ERROR_TEXT("Failed to initialize MultiNet"));
        }
    } else {
        multinet_ops_clear(s_ops);
    }

    self->ops = s_ops;
    return MP_OBJ_FROM_PTR(self);
}

/* -- Helper: check initialized --------------------------------------------- */

static inline void check_ops(multinetmod_obj_t *self) {
    if (!self->ops) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("MultiNet not initialized"));
    }
}

/* -- add_command(command_id, pinyin) ---------------------------------------- */

static mp_obj_t multinetmod_add_command(mp_obj_t self_in,
                                         mp_obj_t id_in,
                                         mp_obj_t pinyin_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);

    int command_id = mp_obj_get_int(id_in);
    const char *pinyin = mp_obj_str_get_str(pinyin_in);

    if (multinet_ops_add_command(self->ops, command_id, pinyin) != 0) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Invalid pinyin command: '%s'"), pinyin);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(multinetmod_add_command_obj,
                                  multinetmod_add_command);

/* -- remove_command(pinyin) ------------------------------------------------ */

static mp_obj_t multinetmod_remove_command(mp_obj_t self_in,
                                            mp_obj_t pinyin_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);

    const char *pinyin = mp_obj_str_get_str(pinyin_in);
    if (multinet_ops_remove_command(self->ops, pinyin) != 0) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("Command not found: '%s'"), pinyin);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(multinetmod_remove_command_obj,
                                  multinetmod_remove_command);

/* -- clear_commands() ------------------------------------------------------ */

static mp_obj_t multinetmod_clear_commands(mp_obj_t self_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);
    multinet_ops_clear_commands(self->ops);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(multinetmod_clear_commands_obj,
                                  multinetmod_clear_commands);

/* -- update() -------------------------------------------------------------- */

static mp_obj_t multinetmod_update(mp_obj_t self_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);

    if (multinet_ops_update(self->ops) != 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to compile commands into FST"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(multinetmod_update_obj, multinetmod_update);

/* -- detect(pcm_data) -> dict | None --------------------------------------- */

static mp_obj_t multinetmod_detect(mp_obj_t self_in, mp_obj_t pcm_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(pcm_in, &bufinfo, MP_BUFFER_READ);

    if (bufinfo.len < 2 || bufinfo.len % 2 != 0) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("PCM data must contain 16-bit aligned samples"));
    }

    int num_samples = bufinfo.len / sizeof(int16_t);
    multinet_result_t result;

    bool detected = multinet_ops_detect(
        self->ops, (const int16_t *)bufinfo.buf, num_samples, &result);

    if (detected) {
        mp_obj_t dict = mp_obj_new_dict(3);
        mp_obj_dict_store(dict,
            MP_OBJ_NEW_QSTR(MP_QSTR_command_id),
            MP_OBJ_NEW_SMALL_INT(result.command_id));
        mp_obj_dict_store(dict,
            MP_OBJ_NEW_QSTR(MP_QSTR_phrase),
            result.phrase ? mp_obj_new_str(result.phrase, strlen(result.phrase))
                          : mp_const_none);
        mp_obj_dict_store(dict,
            MP_OBJ_NEW_QSTR(MP_QSTR_prob),
            mp_obj_new_float(result.prob));
        return dict;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(multinetmod_detect_obj, multinetmod_detect);

/* -- clear() --------------------------------------------------------------- */

static mp_obj_t multinetmod_clear(mp_obj_t self_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ops) {
        multinet_ops_clear(self->ops);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(multinetmod_clear_obj, multinetmod_clear);

/* -- set_threshold(threshold) ---------------------------------------------- */

static mp_obj_t multinetmod_set_threshold(mp_obj_t self_in,
                                           mp_obj_t threshold_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_ops(self);

    float threshold = mp_obj_get_float(threshold_in);
    if (threshold < 0.0f || threshold > 0.99f) {
        mp_raise_ValueError(MP_ERROR_TEXT("threshold must be 0.0 ~ 0.99"));
    }
    multinet_ops_set_threshold(self->ops, threshold);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(multinetmod_set_threshold_obj,
                                  multinetmod_set_threshold);

/* -- deinit() -------------------------------------------------------------- */

static mp_obj_t multinetmod_deinit(mp_obj_t self_in) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ops) {
        multinet_ops_destroy(self->ops);
        if (self->ops == s_ops) {
            s_ops = NULL;
        }
        self->ops = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(multinetmod_deinit_obj, multinetmod_deinit);

/* -- Read-only attributes -------------------------------------------------- */

static void multinetmod_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    multinetmod_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (dest[0] != MP_OBJ_NULL) return;

    if (!self->ops) {
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }

    if (attr == MP_QSTR_chunk_samples) {
        dest[0] = mp_obj_new_int(multinet_ops_get_chunk_samples(self->ops));
    } else if (attr == MP_QSTR_chunk_bytes) {
        dest[0] = mp_obj_new_int(
            multinet_ops_get_chunk_samples(self->ops) * sizeof(int16_t));
    } else if (attr == MP_QSTR_sample_rate) {
        dest[0] = mp_obj_new_int(multinet_ops_get_sample_rate(self->ops));
    } else if (attr == MP_QSTR_ready) {
        dest[0] = mp_obj_new_bool(multinet_ops_is_ready(self->ops));
    } else {
        dest[1] = MP_OBJ_SENTINEL;
    }
}

/* -- Type definition ------------------------------------------------------- */

static const mp_rom_map_elem_t multinetmod_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_add_command),    MP_ROM_PTR(&multinetmod_add_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove_command), MP_ROM_PTR(&multinetmod_remove_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_commands), MP_ROM_PTR(&multinetmod_clear_commands_obj) },
    { MP_ROM_QSTR(MP_QSTR_update),         MP_ROM_PTR(&multinetmod_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_detect),         MP_ROM_PTR(&multinetmod_detect_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),          MP_ROM_PTR(&multinetmod_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_threshold),  MP_ROM_PTR(&multinetmod_set_threshold_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),         MP_ROM_PTR(&multinetmod_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(multinetmod_locals_dict,
                             multinetmod_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    multinetmod_type,
    MP_QSTR_MultiNet,
    MP_TYPE_FLAG_NONE,
    make_new, multinetmod_make_new,
    attr, multinetmod_attr,
    locals_dict, &multinetmod_locals_dict
);

/* ========================================================================== */
/* Module definition                                                          */
/* ========================================================================== */

static const mp_rom_map_elem_t multinetmod_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_multinetmod) },
    { MP_ROM_QSTR(MP_QSTR_MultiNet),  MP_ROM_PTR(&multinetmod_type) },
};
static MP_DEFINE_CONST_DICT(multinetmod_module_globals,
                             multinetmod_module_globals_table);

const mp_obj_module_t multinetmod_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&multinetmod_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_multinetmod, multinetmod_module);

#endif /* CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 */
