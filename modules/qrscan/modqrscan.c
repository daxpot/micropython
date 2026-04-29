// MicroPython binding for quirc-based QR code recognition.
//
// Python API:
//   import qrscan
//   qrscan.scan(buf, width, height, downsample=True) -> str
//       grayscale 8-bit input (length >= width*height).
//   qrscan.scan_rgb565(buf, width, height, big_endian=False, downsample=True) -> str
//       RGB565 input (length >= 2*width*height); converted to grayscale in C.
//   qrscan.version() -> str

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include "quirc/quirc.h"
#include "qrscan_alloc.h"
#include "qrscan_resize.h"

#define QRSCAN_DEFAULT_MAX_SIZE 480

// Run quirc on a contiguous grayscale buffer. Returns a Python str
// (decoded payload, or empty string on no-decode). Frees `resized` if non-NULL.
static mp_obj_t qrscan_run_quirc(const uint8_t *gray, int w, int h, uint8_t *owned_buf) {
    struct quirc *q = quirc_new();
    if (q == NULL) {
        if (owned_buf) qrscan_free(owned_buf);
        mp_raise_msg(&mp_type_MemoryError,
                     MP_ERROR_TEXT("qrscan: quirc_new failed"));
    }
    if (quirc_resize(q, w, h) < 0) {
        quirc_destroy(q);
        if (owned_buf) qrscan_free(owned_buf);
        mp_raise_msg(&mp_type_MemoryError,
                     MP_ERROR_TEXT("qrscan: quirc_resize failed"));
    }

    int qw = 0, qh = 0;
    uint8_t *qbuf = quirc_begin(q, &qw, &qh);
    if (qbuf == NULL || qw != w || qh != h) {
        quirc_destroy(q);
        if (owned_buf) qrscan_free(owned_buf);
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("qrscan: quirc_begin failed"));
    }
    memcpy(qbuf, gray, (size_t)w * (size_t)h);
    quirc_end(q);

    mp_obj_t result = mp_obj_new_str("", 0);
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if (err == QUIRC_SUCCESS) {
            size_t plen = data.payload_len;
            if (plen > sizeof(data.payload)) {
                plen = sizeof(data.payload);
            }
            result = mp_obj_new_str_copy(&mp_type_str,
                                         (const byte *)data.payload, plen);
            break;
        }
    }

    quirc_destroy(q);
    if (owned_buf) qrscan_free(owned_buf);
    return result;
}

// Optionally downsample so max(w,h) <= max_size.
static void qrscan_maybe_downsample(const uint8_t **p_gray, int *pw, int *ph,
                                    bool do_down, int max_size, uint8_t **p_owned) {
    int w = *pw;
    int h = *ph;
    int max_dim = w > h ? w : h;
    if (!do_down || max_size <= 0 || max_dim <= max_size) {
        return;
    }
    int target_w = w * max_size / max_dim;
    int target_h = h * max_size / max_dim;
    if (target_w < 1) target_w = 1;
    if (target_h < 1) target_h = 1;
    uint8_t *buf = (uint8_t *)qrscan_malloc((size_t)target_w * (size_t)target_h);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError,
                     MP_ERROR_TEXT("qrscan: cannot allocate resize buffer"));
    }
    qrscan_downsample_gray(*p_gray, w, h, buf, target_w, target_h);
    *p_gray = buf;
    *pw = target_w;
    *ph = target_h;
    *p_owned = buf;
}

static mp_obj_t mod_qrscan_scan(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_buf, ARG_width, ARG_height, ARG_downsample, ARG_max_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buf,        MP_ARG_REQUIRED | MP_ARG_OBJ,  {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_width,      MP_ARG_REQUIRED | MP_ARG_INT,  {.u_int = 0} },
        { MP_QSTR_height,     MP_ARG_REQUIRED | MP_ARG_INT,  {.u_int = 0} },
        { MP_QSTR_downsample, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_max_size,   MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int = QRSCAN_DEFAULT_MAX_SIZE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int w = args[ARG_width].u_int;
    int h = args[ARG_height].u_int;
    if (w <= 0 || h <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid width/height"));
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len < (size_t)w * (size_t)h) {
        mp_raise_ValueError(MP_ERROR_TEXT("buf too small for width*height"));
    }

    const uint8_t *gray = (const uint8_t *)bufinfo.buf;
    uint8_t *owned = NULL;
    qrscan_maybe_downsample(&gray, &w, &h,
                            args[ARG_downsample].u_bool,
                            args[ARG_max_size].u_int, &owned);
    return qrscan_run_quirc(gray, w, h, owned);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_qrscan_scan_obj, 3, mod_qrscan_scan);

static mp_obj_t mod_qrscan_scan_rgb565(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_buf, ARG_width, ARG_height, ARG_big_endian, ARG_downsample, ARG_max_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buf,        MP_ARG_REQUIRED | MP_ARG_OBJ,  {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_width,      MP_ARG_REQUIRED | MP_ARG_INT,  {.u_int = 0} },
        { MP_QSTR_height,     MP_ARG_REQUIRED | MP_ARG_INT,  {.u_int = 0} },
        { MP_QSTR_big_endian, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_downsample, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_max_size,   MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int = QRSCAN_DEFAULT_MAX_SIZE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int w = args[ARG_width].u_int;
    int h = args[ARG_height].u_int;
    bool big_endian = args[ARG_big_endian].u_bool;
    bool do_down = args[ARG_downsample].u_bool;
    int max_size = args[ARG_max_size].u_int;
    if (w <= 0 || h <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid width/height"));
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);
    size_t need = (size_t)w * (size_t)h * 2u;
    if (bufinfo.len < need) {
        mp_raise_ValueError(MP_ERROR_TEXT("buf too small for 2*width*height"));
    }

    size_t pixels = (size_t)w * (size_t)h;
    uint8_t *gray_buf = (uint8_t *)qrscan_malloc(pixels);
    if (gray_buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError,
                     MP_ERROR_TEXT("qrscan: cannot allocate gray buffer"));
    }
    const uint8_t *p = (const uint8_t *)bufinfo.buf;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t px;
        if (big_endian) {
            px = ((uint16_t)p[0] << 8) | p[1];
        } else {
            px = ((uint16_t)p[1] << 8) | p[0];
        }
        p += 2;
        uint8_t r5 = (px >> 11) & 0x1F;
        uint8_t g6 = (px >> 5)  & 0x3F;
        uint8_t b5 =  px        & 0x1F;
        uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
        uint32_t y = (uint32_t)r8 * 76u + (uint32_t)g8 * 150u + (uint32_t)b8 * 30u;
        gray_buf[i] = (uint8_t)(y >> 8);
    }

    const uint8_t *gray = gray_buf;
    uint8_t *owned = gray_buf;
    int gw = w, gh = h;
    if (do_down && max_size > 0) {
        int max_dim = w > h ? w : h;
        if (max_dim > max_size) {
            int target_w = w * max_size / max_dim;
            int target_h = h * max_size / max_dim;
            if (target_w < 1) target_w = 1;
            if (target_h < 1) target_h = 1;
            uint8_t *small = (uint8_t *)qrscan_malloc((size_t)target_w * (size_t)target_h);
            if (small == NULL) {
                qrscan_free(gray_buf);
                mp_raise_msg(&mp_type_MemoryError,
                             MP_ERROR_TEXT("qrscan: cannot allocate resize buffer"));
            }
            qrscan_downsample_gray(gray_buf, w, h, small, target_w, target_h);
            qrscan_free(gray_buf);
            gray = small;
            owned = small;
            gw = target_w;
            gh = target_h;
        }
    }
    return qrscan_run_quirc(gray, gw, gh, owned);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_qrscan_scan_rgb565_obj, 3, mod_qrscan_scan_rgb565);

static mp_obj_t mod_qrscan_version(void) {
    const char *v = quirc_version();
    return mp_obj_new_str(v, strlen(v));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_qrscan_version_obj, mod_qrscan_version);

static const mp_rom_map_elem_t qrscan_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_qrscan) },
    { MP_ROM_QSTR(MP_QSTR_scan),        MP_ROM_PTR(&mod_qrscan_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan_rgb565), MP_ROM_PTR(&mod_qrscan_scan_rgb565_obj) },
    { MP_ROM_QSTR(MP_QSTR_version),     MP_ROM_PTR(&mod_qrscan_version_obj) },
};
static MP_DEFINE_CONST_DICT(qrscan_module_globals, qrscan_module_globals_table);

const mp_obj_module_t qrscan_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&qrscan_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_qrscan, qrscan_user_cmodule);

