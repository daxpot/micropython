/*
 * vision_ml.c
 *
 * MicroPython binding layer for the vision_ml module on ESP32-P4.
 *
 * Python API:
 *   import vision_ml
 *   vision_ml.init()                                  # load backbone (PSRAM)
 *   clf = vision_ml.ImageClassifier('/models/fruits/')
 *   print(clf.classes)                                 # ['apple','banana',...]
 *   res = clf.classify(rgb_bytes, top_k=3)             # [(label, score), ...]
 *   clf.close()                                        # optional
 *   vision_ml.deinit()                                 # optional
 *   info = vision_ml.info()                            # {'initialized':bool, 'arena_used':int, ...}
 *
 * The 'rgb_bytes' input must be 224*224*3 = 150528 bytes of RGB888 data,
 * obtained externally (e.g., via the mp_jpeg module decoding a JPEG from
 * csi_camera).
 */

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/builtin.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#include "vision_ml_engine.h"
#include "vision_ml_head.h"
#include "vision_ml_manifest.h"

/* ========================================================================== */
/* Module-level helpers                                                       */
/* ========================================================================== */

/* Read a whole file via MicroPython's builtin open(), so VFS paths like
 * /models/... are resolved correctly. Returned buffer is malloc'd in PSRAM
 * and NUL-terminated. Caller frees with heap_caps_free.
 * Returns NULL on error (raises an exception via mp_call_function_*). */
static char *read_whole_file(const char *path, size_t *size_out) {
    mp_obj_t open_args[2] = {
        mp_obj_new_str(path, strlen(path)),
        MP_OBJ_NEW_QSTR(MP_QSTR_rb),
    };
    mp_obj_t f = mp_call_function_n_kw(mp_load_global(MP_QSTR_open),
                                         2, 0, open_args);

    mp_obj_t dest[3];
    mp_load_method(f, MP_QSTR_read, dest);
    mp_obj_t data_obj = mp_call_method_n_kw(0, 0, dest);

    mp_load_method(f, MP_QSTR_close, dest);
    mp_call_method_n_kw(0, 0, dest);

    mp_buffer_info_t bi;
    mp_get_buffer_raise(data_obj, &bi, MP_BUFFER_READ);

    char *buf = (char *)heap_caps_malloc(bi.len + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    memcpy(buf, bi.buf, bi.len);
    buf[bi.len] = '\0';
    *size_out = bi.len;
    return buf;
}

static void join_path(char *dst, size_t cap, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    bool need_sep = (dlen > 0 && dir[dlen - 1] != '/');
    snprintf(dst, cap, "%s%s%s", dir, need_sep ? "/" : "", name);
}

/* ========================================================================== */
/* Module functions: init / deinit / info                                     */
/* ========================================================================== */

static mp_obj_t mod_vision_ml_init(void) {
    int rc = vision_ml_engine_init();
    if (rc != 0) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("vision_ml engine init failed: %d"), rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_vision_ml_init_obj, mod_vision_ml_init);

static mp_obj_t mod_vision_ml_deinit(void) {
    vision_ml_engine_deinit();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_vision_ml_deinit_obj, mod_vision_ml_deinit);

static mp_obj_t mod_vision_ml_info(void) {
    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_initialized),
                      mp_obj_new_bool(vision_ml_engine_initialized()));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_arena_used),
                      mp_obj_new_int_from_uint(vision_ml_engine_arena_used()));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_arena_size),
                      mp_obj_new_int_from_uint(vision_ml_engine_arena_size()));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_feature_dim),
                      MP_OBJ_NEW_SMALL_INT(VISION_ML_FEATURE_DIM));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_input_size),
                      MP_OBJ_NEW_SMALL_INT(VISION_ML_INPUT_SIZE));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_vision_ml_info_obj, mod_vision_ml_info);

/* ========================================================================== */
/* ImageClassifier class                                                      */
/* ========================================================================== */

typedef struct _vision_ml_classifier_obj_t {
    mp_obj_base_t base;
    bool active;
    vision_ml_manifest_t manifest;
    vision_ml_head_t head;
    float *feature_buf;     /* [VISION_ML_FEATURE_DIM] in PSRAM */
    float *probs_buf;       /* [n_classes] in PSRAM */
} vision_ml_classifier_obj_t;

extern const mp_obj_type_t vision_ml_classifier_type;

static void classifier_release(vision_ml_classifier_obj_t *self) {
    if (!self->active) return;
    vision_ml_head_deinit(&self->head);
    vision_ml_manifest_free(&self->manifest);
    if (self->feature_buf) { heap_caps_free(self->feature_buf); self->feature_buf = NULL; }
    if (self->probs_buf) { heap_caps_free(self->probs_buf); self->probs_buf = NULL; }
    self->active = false;
}

static mp_obj_t classifier_make_new(const mp_obj_type_t *type, size_t n_args,
                                      size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    if (!vision_ml_engine_initialized()) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("call vision_ml.init() before constructing ImageClassifier"));
    }

    const char *dir_path = mp_obj_str_get_str(args[0]);

    char manifest_path[256];
    char weights_path[256];
    join_path(manifest_path, sizeof(manifest_path), dir_path, "manifest.json");
    join_path(weights_path, sizeof(weights_path), dir_path, "head_weights.bin");

    size_t mjson_sz = 0;
    char *mjson = read_whole_file(manifest_path, &mjson_sz);
    if (!mjson) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot read %s"), manifest_path);
    }

    vision_ml_classifier_obj_t *self = mp_obj_malloc(vision_ml_classifier_obj_t,
                                                       &vision_ml_classifier_type);
    self->active = false;
    self->feature_buf = NULL;
    self->probs_buf = NULL;
    memset(&self->manifest, 0, sizeof(self->manifest));
    memset(&self->head, 0, sizeof(self->head));

    vision_ml_manifest_err_t merr = vision_ml_manifest_parse(mjson, &self->manifest);
    heap_caps_free(mjson);
    if (merr != VML_MAN_OK) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("manifest invalid: %d"), (int)merr);
    }

    size_t wsize = 0;
    char *wbuf = read_whole_file(weights_path, &wsize);
    if (!wbuf) {
        vision_ml_manifest_free(&self->manifest);
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot read %s"), weights_path);
    }

    int hrc = vision_ml_head_init(&self->head,
                                    self->manifest.feature_dim,
                                    self->manifest.hidden_dim,
                                    self->manifest.n_classes,
                                    (const uint8_t *)wbuf, wsize);
    heap_caps_free(wbuf);
    if (hrc != 0) {
        vision_ml_manifest_free(&self->manifest);
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("head init failed: %d (weights size mismatch?)"), hrc);
    }

    self->feature_buf = (float *)heap_caps_malloc(
        VISION_ML_FEATURE_DIM * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    self->probs_buf = (float *)heap_caps_malloc(
        self->manifest.n_classes * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!self->feature_buf || !self->probs_buf) {
        if (self->feature_buf) heap_caps_free(self->feature_buf);
        if (self->probs_buf) heap_caps_free(self->probs_buf);
        vision_ml_head_deinit(&self->head);
        vision_ml_manifest_free(&self->manifest);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("PSRAM alloc failed"));
    }

    self->active = true;
    return MP_OBJ_FROM_PTR(self);
}

/* clf.classify(rgb_bytes, top_k=1) -> [(label, score), ...] */
static mp_obj_t classifier_classify(size_t n_args, const mp_obj_t *pos_args,
                                      mp_map_t *kw_args) {
    enum { ARG_rgb, ARG_top_k };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_rgb,   MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_top_k, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    vision_ml_classifier_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (!self->active) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("classifier closed"));
    }

    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[ARG_rgb].u_obj, &bi, MP_BUFFER_READ);
    if ((int)bi.len != VISION_ML_INPUT_BYTES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("expected %d RGB888 bytes, got %d"),
            VISION_ML_INPUT_BYTES, (int)bi.len);
    }

    int rc = vision_ml_engine_extract_features((const uint8_t *)bi.buf,
                                                 (int)bi.len,
                                                 self->feature_buf);
    if (rc != 0) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("backbone forward failed: %d"), rc);
    }

    vision_ml_head_forward(&self->head, self->feature_buf, self->probs_buf);

    int N = self->manifest.n_classes;
    int top_k = args[ARG_top_k].u_int;
    if (top_k < 1) top_k = 1;
    if (top_k > N) top_k = N;

    /* Selection sort top_k indices */
    int idx[VISION_ML_MAX_CLASSES];
    bool taken[VISION_ML_MAX_CLASSES] = {0};
    for (int t = 0; t < top_k; t++) {
        int best = -1;
        float best_p = -1.0f;
        for (int k = 0; k < N; k++) {
            if (taken[k]) continue;
            if (self->probs_buf[k] > best_p) { best_p = self->probs_buf[k]; best = k; }
        }
        idx[t] = best;
        if (best >= 0) taken[best] = true;
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int t = 0; t < top_k; t++) {
        int k = idx[t];
        if (k < 0) break;
        mp_obj_t pair[2] = {
            mp_obj_new_str(self->manifest.labels[k], strlen(self->manifest.labels[k])),
            mp_obj_new_float(self->probs_buf[k]),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(2, pair));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(classifier_classify_obj, 1, classifier_classify);

/* clf.close() */
static mp_obj_t classifier_close(mp_obj_t self_in) {
    vision_ml_classifier_obj_t *self = MP_OBJ_TO_PTR(self_in);
    classifier_release(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(classifier_close_obj, classifier_close);

/* property: clf.classes -> list[str] (via attr slot below) */
static mp_obj_t classifier_build_classes_list(vision_ml_classifier_obj_t *self) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < self->manifest.n_labels; i++) {
        mp_obj_list_append(list, mp_obj_new_str(self->manifest.labels[i],
                                                  strlen(self->manifest.labels[i])));
    }
    return list;
}

static void classifier_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    vision_ml_classifier_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        /* Load attribute. */
        if (attr == MP_QSTR_classes) {
            if (!self->active) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("classifier closed"));
            }
            dest[0] = classifier_build_classes_list(self);
            return;
        }
        if (attr == MP_QSTR_n_classes) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(self->manifest.n_classes);
            return;
        }
        /* Fall through to locals_dict for methods like classify/close. */
        dest[1] = MP_OBJ_SENTINEL;
    }
    /* dest[0] != NULL means a store/delete; not supported (leave dest as-is). */
}

static const mp_rom_map_elem_t classifier_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_classify), MP_ROM_PTR(&classifier_classify_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),    MP_ROM_PTR(&classifier_close_obj) },
};
static MP_DEFINE_CONST_DICT(classifier_locals_dict, classifier_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    vision_ml_classifier_type,
    MP_QSTR_ImageClassifier,
    MP_TYPE_FLAG_NONE,
    make_new, classifier_make_new,
    attr, classifier_attr,
    locals_dict, &classifier_locals_dict
);

/* ========================================================================== */
/* Module globals                                                             */
/* ========================================================================== */

static const mp_rom_map_elem_t vision_ml_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),         MP_ROM_QSTR(MP_QSTR_vision_ml) },
    { MP_ROM_QSTR(MP_QSTR_init),             MP_ROM_PTR(&mod_vision_ml_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),           MP_ROM_PTR(&mod_vision_ml_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_info),             MP_ROM_PTR(&mod_vision_ml_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_ImageClassifier),  MP_ROM_PTR(&vision_ml_classifier_type) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_SIZE),       MP_ROM_INT(VISION_ML_INPUT_SIZE) },
    { MP_ROM_QSTR(MP_QSTR_FEATURE_DIM),      MP_ROM_INT(VISION_ML_FEATURE_DIM) },
    { MP_ROM_QSTR(MP_QSTR_INPUT_BYTES),      MP_ROM_INT(VISION_ML_INPUT_BYTES) },
};
static MP_DEFINE_CONST_DICT(vision_ml_module_globals, vision_ml_module_globals_table);

const mp_obj_module_t vision_ml_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vision_ml_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_vision_ml, vision_ml_module);
