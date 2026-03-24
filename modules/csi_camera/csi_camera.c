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
 *   cam.free_buffer()
 *   cam.deinit()
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

// Forward declaration — MP_DEFINE_CONST_OBJ_TYPE produces a non-static const definition
extern const mp_obj_type_t csi_camera_type;

// CSICamera(h_res=800, v_res=640, jpeg_quality=80, data_lanes=2,
//           lane_bitrate=200, sccb_sda=-1, sccb_scl=-1)
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

    // Validate parameters
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
        // Format: "init failed at step N: err E"
        // Steps: 1=LDO, 2=sensor, 3=framebuf, 4=CSI, 5=ISP, 6=JPEG, 7=stream, 8=start
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

    if (!self->cam.initialized) {
        mp_raise_OSError(MP_ENOENT);
    }

    esp_err_t err = csi_camera_capture(&self->cam);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError,
                          MP_ERROR_TEXT("capture failed: %d"), err);
    }

    // Return JPEG data as bytes object
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

// Property: cam.initialized
static mp_obj_t csi_camera_get_initialized(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->cam.initialized);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_get_initialized_obj, csi_camera_get_initialized);

// Property: cam.resolution -> (h_res, v_res)
static mp_obj_t csi_camera_get_resolution(mp_obj_t self_in) {
    csi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[2] = {
        mp_obj_new_int(self->cam.config.h_res),
        mp_obj_new_int(self->cam.config.v_res),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(csi_camera_get_resolution_obj, csi_camera_get_resolution);

// Method table
static const mp_rom_map_elem_t csi_camera_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),          MP_ROM_PTR(&csi_camera_obj_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_capture),       MP_ROM_PTR(&csi_camera_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_free_buffer),   MP_ROM_PTR(&csi_camera_free_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),        MP_ROM_PTR(&csi_camera_obj_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_initialized),   MP_ROM_PTR(&csi_camera_get_initialized_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolution),    MP_ROM_PTR(&csi_camera_get_resolution_obj) },
};
static MP_DEFINE_CONST_DICT(csi_camera_locals_dict, csi_camera_locals_dict_table);

// CSICamera type
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

// Module definition
const mp_obj_module_t csi_camera_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&csi_camera_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_csi_camera, csi_camera_module);
