/*
 * csi_camera.c
 *
 * MicroPython user C module: csi_camera
 * Provides CSICamera class for ESP32-P4 MIPI CSI camera with HW JPEG encoding.
 *
 * Python API:
 *   from csi_camera import CSICamera
 *
 *   cam = CSICamera(h_res=800, v_res=640, jpeg_quality=80)
 *   cam.init()
 *   img = cam.capture()       # returns bytes (JPEG)
 *   cam.set_color(brightness=20, contrast=128, saturation=128, hue=0)
 *   cam.deinit()
 *
 * Singleton: re-calling init() without deinit() reuses existing hardware.
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "csi_camera_hal.h"

typedef struct _csi_camera_obj_t {
    mp_obj_base_t base;
    csi_camera_t cam;
} csi_camera_obj_t;

extern const mp_obj_type_t csi_camera_type;

// CSICamera(h_res=800, v_res=640, jpeg_quality=80, ...)
static mp_obj_t csi_camera_make_new(const mp_obj_type_t *type, size_t n_args,
                                     size_t n_kw, const mp_obj_t *all_args) {
    enum {
        ARG_h_res, ARG_v_res, ARG_jpeg_quality,
        ARG_data_lanes, ARG_lane_bitrate,
        ARG_sccb_sda, ARG_sccb_scl,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_h_res,         MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 800} },
        { MP_QSTR_v_res,         MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 640} },
        { MP_QSTR_jpeg_quality,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 80} },
        { MP_QSTR_data_lanes,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2} },
        { MP_QSTR_lane_bitrate,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 200} },
        { MP_QSTR_sccb_sda,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_sccb_scl,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
                               MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    csi_camera_obj_t *self = mp_obj_malloc(csi_camera_obj_t, &csi_camera_type);
    memset(&self->cam, 0, sizeof(csi_camera_t));

    self->cam.config.h_res = args[ARG_h_res].u_int;
    self->cam.config.v_res = args[ARG_v_res].u_int;
    self->cam.config.jpeg_quality = args[ARG_jpeg_quality].u_int;
    self->cam.config.data_lanes = args[ARG_data_lanes].u_int;
    self->cam.config.lane_bitrate_mbps = args[ARG_lane_bitrate].u_int;
    self->cam.config.sccb_sda_io = args[ARG_sccb_sda].u_int;
    self->cam.config.sccb_scl_io = args[ARG_sccb_scl].u_int;

    if (self->cam.config.jpeg_quality < 1 || self->cam.config.jpeg_quality > 100) {
        mp_raise_ValueError(MP_ERROR_TEXT("jpeg_quality must be 1-100"));
    }
    if (self->cam.config.data_lanes < 1 || self->cam.config.data_lanes > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("data_lanes must be 1 or 2"));
    }

    return MP_OBJ_FROM_PTR(self);
}

// cam.init()
static mp_obj_t csi_camera_obj_init(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_err_t err = csi_camera_init(&self->cam);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("init failed at step %d: err %d"),
                          self->cam.init_step, err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_obj_init_obj, csi_camera_obj_init);

// cam.capture() -> bytes (JPEG data)
static mp_obj_t csi_camera_capture_func(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!csi_camera_get_instance()->initialized) {
        mp_raise_OSError(MP_ENOENT);
    }

    esp_err_t err = csi_camera_capture(&self->cam);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("capture failed: %d"), err);
    }

    return mp_obj_new_bytes(self->cam.jpeg_buffer, self->cam.jpeg_data_size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_capture_obj, csi_camera_capture_func);

// cam.free_buffer()
static mp_obj_t csi_camera_free_buffer_func(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    csi_camera_free_buffer(&self->cam);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_free_buffer_obj, csi_camera_free_buffer_func);

// cam.deinit()
static mp_obj_t csi_camera_obj_deinit(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    csi_camera_deinit(&self->cam);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_obj_deinit_obj, csi_camera_obj_deinit);

// cam.set_color(brightness=0, contrast=128, saturation=128, hue=0)
static mp_obj_t csi_camera_set_color_func(size_t n_args, const mp_obj_t *pos_args,
                                           mp_map_t *kw_args) {
    enum { ARG_brightness, ARG_contrast, ARG_saturation, ARG_hue };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_brightness, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_contrast,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 128} },
        { MP_QSTR_saturation, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 128} },
        { MP_QSTR_hue,        MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    csi_camera_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    esp_err_t err = csi_camera_set_color(&self->cam,
                                          args[ARG_brightness].u_int,
                                          args[ARG_contrast].u_int,
                                          args[ARG_saturation].u_int,
                                          args[ARG_hue].u_int);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("set_color failed: %d"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(csi_camera_set_color_obj, 1, csi_camera_set_color_func);

// cam.set_mirror_flip(hmirror=False, vflip=False)
static mp_obj_t csi_camera_set_mirror_flip_func(size_t n_args, const mp_obj_t *pos_args,
                                                  mp_map_t *kw_args) {
    enum { ARG_hmirror, ARG_vflip };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_hmirror, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_vflip,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    csi_camera_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    esp_err_t err = csi_camera_set_mirror_flip(&self->cam,
                                                args[ARG_hmirror].u_bool,
                                                args[ARG_vflip].u_bool);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("set_mirror_flip failed: %d"), err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(csi_camera_set_mirror_flip_obj, 1, csi_camera_set_mirror_flip_func);

// Property: cam.initialized
static mp_obj_t csi_camera_get_initialized(mp_obj_t self_in) {
    return mp_obj_new_bool(csi_camera_get_instance()->initialized);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_get_initialized_obj, csi_camera_get_initialized);

// Property: cam.resolution -> (h_res, v_res) — actual output resolution
static mp_obj_t csi_camera_get_resolution(mp_obj_t self_in) {
    csi_camera_t *inst = csi_camera_get_instance();
    mp_obj_t items[2] = {
        mp_obj_new_int(inst->config.h_res),
        mp_obj_new_int(inst->config.v_res),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_get_resolution_obj, csi_camera_get_resolution);

// Property: cam.sensor_resolution -> (h_res, v_res) — sensor native resolution
static mp_obj_t csi_camera_get_sensor_resolution(mp_obj_t self_in) {
    csi_camera_t *inst = csi_camera_get_instance();
    mp_obj_t items[2] = {
        mp_obj_new_int(inst->config.sensor_h_res),
        mp_obj_new_int(inst->config.sensor_v_res),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_get_sensor_resolution_obj, csi_camera_get_sensor_resolution);

// Method table
static const mp_rom_map_elem_t csi_camera_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),              MP_ROM_PTR(&csi_camera_obj_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_capture),           MP_ROM_PTR(&csi_camera_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_free_buffer),       MP_ROM_PTR(&csi_camera_free_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),            MP_ROM_PTR(&csi_camera_obj_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_color),         MP_ROM_PTR(&csi_camera_set_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mirror_flip),  MP_ROM_PTR(&csi_camera_set_mirror_flip_obj) },
    { MP_ROM_QSTR(MP_QSTR_initialized),       MP_ROM_PTR(&csi_camera_get_initialized_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolution),        MP_ROM_PTR(&csi_camera_get_resolution_obj) },
    { MP_ROM_QSTR(MP_QSTR_sensor_resolution), MP_ROM_PTR(&csi_camera_get_sensor_resolution_obj) },
};
static MP_DEFINE_CONST_DICT(csi_camera_locals_dict, csi_camera_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    csi_camera_type,
    MP_QSTR_CSICamera,
    MP_TYPE_FLAG_NONE,
    make_new, csi_camera_make_new,
    locals_dict, &csi_camera_locals_dict
);

// Module globals
static const mp_rom_map_elem_t csi_camera_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_csi_camera) },
    { MP_ROM_QSTR(MP_QSTR_CSICamera),  MP_ROM_PTR(&csi_camera_type) },
};
static MP_DEFINE_CONST_DICT(csi_camera_module_globals, csi_camera_module_globals_table);

const mp_obj_module_t csi_camera_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&csi_camera_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_csi_camera, csi_camera_module);
