/**
 * @file ml307mod.c
 * @brief MicroPython module for ML307/EC801E/NT26K 4G modem
 * 
 * This module provides MicroPython bindings for the esp-ml307 library,
 * enabling 4G network connectivity with support for HTTP, WebSocket,
 * TCP, UDP, and MQTT protocols.
 * 
 * Usage:
 *   import ml307
 *   modem = ml307.Modem(tx_pin=13, rx_pin=14, dtr_pin=15, baud_rate=921600)
 *   status = modem.wait_for_network(timeout_ms=30000)
 *   if status == ml307.STATUS_READY:
 *       print("Network ready!")
 *       print("IMEI:", modem.imei())
 *       print("Signal:", modem.csq())
 */

#include "py/mpprint.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/objstr.h"
#include "ml307call.h"

// Forward declarations
static const mp_obj_type_t ml307mod_modem_type;
static const mp_obj_type_t ml307mod_http_type;
static const mp_obj_type_t ml307mod_websocket_type;
static const mp_obj_type_t ml307mod_tcp_type;
static const mp_obj_type_t ml307mod_udp_type;
static const mp_obj_type_t ml307mod_mqtt_type;

// Global cached modem object (Python level singleton)
static mp_obj_t g_cached_modem_obj = MP_OBJ_NULL;

// ============================================================================
// Modem Class
// ============================================================================

typedef struct _ml307mod_modem_obj_t {
    mp_obj_base_t base;
    ml307_modem_handle_t handle;
    int tx_pin;
    int rx_pin;
    int dtr_pin;
    int baud_rate;
} ml307mod_modem_obj_t;

static mp_obj_t ml307mod_modem_del(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_del_obj, ml307mod_modem_del);

static mp_obj_t ml307mod_modem_make_new(const mp_obj_type_t *type,
                                         size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_tx_pin, ARG_rx_pin, ARG_dtr_pin, ARG_baud_rate, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_tx_pin,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_rx_pin,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_dtr_pin,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_baud_rate,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 115200} },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, vals);
    
    int tx_pin = vals[ARG_tx_pin].u_int;
    int rx_pin = vals[ARG_rx_pin].u_int;
    int dtr_pin = vals[ARG_dtr_pin].u_int;
    int baud_rate = vals[ARG_baud_rate].u_int;
    int timeout_ms = vals[ARG_timeout_ms].u_int;
    
    if (tx_pin < 0 || rx_pin < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("tx_pin and rx_pin must be valid GPIO numbers"));
    }
    
    // Check if C layer has cached modem
    if (ml307_is_initialized()) {
        // C layer has cache, check if Python layer cache is still valid
        if (g_cached_modem_obj != MP_OBJ_NULL) {
            // Verify the Python object is still valid (check type)
            if (mp_obj_is_type(g_cached_modem_obj, &ml307mod_modem_type)) {
                ml307mod_modem_obj_t *cached = MP_OBJ_TO_PTR(g_cached_modem_obj);
                // Check if handle matches C layer cache
                if (cached->handle == ml307_get_cached()) {
                    // Both caches valid, return cached object
                    return g_cached_modem_obj;
                }
            }
        }
        
        // Python cache invalid (soft reboot), but C layer cache exists
        // Re-wrap the C layer cached handle with a new Python object
        ml307mod_modem_obj_t *self = mp_obj_malloc(ml307mod_modem_obj_t, type);
        self->handle = ml307_get_cached();
        self->tx_pin = tx_pin;
        self->rx_pin = rx_pin;
        self->dtr_pin = dtr_pin;
        self->baud_rate = baud_rate;
        
        // Update Python layer cache
        g_cached_modem_obj = MP_OBJ_FROM_PTR(self);
        return g_cached_modem_obj;
    }
    
    // No cache exists, create new modem
    ml307_modem_handle_t handle = ml307_detect(tx_pin, rx_pin, dtr_pin, baud_rate, timeout_ms);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem detection failed"));
    }
    
    ml307mod_modem_obj_t *self = mp_obj_malloc(ml307mod_modem_obj_t, type);
    self->handle = handle;
    self->tx_pin = tx_pin;
    self->rx_pin = rx_pin;
    self->dtr_pin = dtr_pin;
    self->baud_rate = baud_rate;
    
    // Cache the Python object for future use
    g_cached_modem_obj = MP_OBJ_FROM_PTR(self);
    
    return g_cached_modem_obj;
}

static mp_obj_t ml307mod_modem_wait_for_network(size_t n_args, const mp_obj_t *args) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int timeout_ms = (n_args > 1) ? mp_obj_get_int(args[1]) : 30000;
    ml307_network_status_t status = ml307_wait_for_network_ready(self->handle, timeout_ms);
    return mp_obj_new_int(status);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_modem_wait_for_network_obj, 1, 2, ml307mod_modem_wait_for_network);

static mp_obj_t ml307mod_modem_is_ready(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_is_network_ready(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_is_ready_obj, ml307mod_modem_is_ready);

static mp_obj_t ml307mod_modem_reboot(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_reboot(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_reboot_obj, ml307mod_modem_reboot);

static mp_obj_t ml307mod_modem_imei(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[32];
    int len = ml307_get_imei(self->handle, buffer, sizeof(buffer));
    if (len < 0) return mp_const_none;
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_imei_obj, ml307mod_modem_imei);

static mp_obj_t ml307mod_modem_iccid(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[32];
    int len = ml307_get_iccid(self->handle, buffer, sizeof(buffer));
    if (len < 0) return mp_const_none;
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_iccid_obj, ml307mod_modem_iccid);

static mp_obj_t ml307mod_modem_revision(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[64];
    int len = ml307_get_module_revision(self->handle, buffer, sizeof(buffer));
    if (len < 0) return mp_const_none;
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_revision_obj, ml307mod_modem_revision);

static mp_obj_t ml307mod_modem_carrier(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[64];
    int len = ml307_get_carrier_name(self->handle, buffer, sizeof(buffer));
    if (len < 0) return mp_const_none;
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_carrier_obj, ml307mod_modem_carrier);

static mp_obj_t ml307mod_modem_csq(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_get_csq(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_csq_obj, ml307mod_modem_csq);

static mp_obj_t ml307mod_modem_close(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_close_obj, ml307mod_modem_close);

// Forward declarations for create methods
static mp_obj_t ml307mod_modem_create_http(mp_obj_t self_in);
static mp_obj_t ml307mod_modem_create_websocket(mp_obj_t self_in);
static mp_obj_t ml307mod_modem_create_tcp(size_t n_args, const mp_obj_t *args);
static mp_obj_t ml307mod_modem_create_udp(mp_obj_t self_in);
static mp_obj_t ml307mod_modem_create_mqtt(mp_obj_t self_in);

static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_create_http_obj, ml307mod_modem_create_http);
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_create_websocket_obj, ml307mod_modem_create_websocket);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_modem_create_tcp_obj, 1, 2, ml307mod_modem_create_tcp);
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_create_udp_obj, ml307mod_modem_create_udp);
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_create_mqtt_obj, ml307mod_modem_create_mqtt);

static const mp_rom_map_elem_t ml307mod_modem_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_modem_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ml307mod_modem_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_for_network), MP_ROM_PTR(&ml307mod_modem_wait_for_network_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_ready), MP_ROM_PTR(&ml307mod_modem_is_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_reboot), MP_ROM_PTR(&ml307mod_modem_reboot_obj) },
    { MP_ROM_QSTR(MP_QSTR_imei), MP_ROM_PTR(&ml307mod_modem_imei_obj) },
    { MP_ROM_QSTR(MP_QSTR_iccid), MP_ROM_PTR(&ml307mod_modem_iccid_obj) },
    { MP_ROM_QSTR(MP_QSTR_revision), MP_ROM_PTR(&ml307mod_modem_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_carrier), MP_ROM_PTR(&ml307mod_modem_carrier_obj) },
    { MP_ROM_QSTR(MP_QSTR_csq), MP_ROM_PTR(&ml307mod_modem_csq_obj) },
    // Factory methods
    { MP_ROM_QSTR(MP_QSTR_create_http), MP_ROM_PTR(&ml307mod_modem_create_http_obj) },
    { MP_ROM_QSTR(MP_QSTR_create_websocket), MP_ROM_PTR(&ml307mod_modem_create_websocket_obj) },
    { MP_ROM_QSTR(MP_QSTR_create_tcp), MP_ROM_PTR(&ml307mod_modem_create_tcp_obj) },
    { MP_ROM_QSTR(MP_QSTR_create_udp), MP_ROM_PTR(&ml307mod_modem_create_udp_obj) },
    { MP_ROM_QSTR(MP_QSTR_create_mqtt), MP_ROM_PTR(&ml307mod_modem_create_mqtt_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_modem_locals_dict, ml307mod_modem_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_modem_type,
    MP_QSTR_Modem,
    MP_TYPE_FLAG_NONE,
    make_new, ml307mod_modem_make_new,
    locals_dict, &ml307mod_modem_locals_dict
);

// ============================================================================
// HTTP Class
// ============================================================================

typedef struct _ml307mod_http_obj_t {
    mp_obj_base_t base;
    ml307_http_handle_t handle;
    ml307mod_modem_obj_t *modem;
} ml307mod_http_obj_t;

static mp_obj_t ml307mod_modem_create_http(mp_obj_t self_in) {
    ml307mod_modem_obj_t *modem = MP_OBJ_TO_PTR(self_in);
    ml307_http_handle_t handle = ml307_http_create(modem->handle);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create HTTP client"));
    }
    ml307mod_http_obj_t *http = mp_obj_malloc(ml307mod_http_obj_t, &ml307mod_http_type);
    http->handle = handle;
    http->modem = modem;
    return MP_OBJ_FROM_PTR(http);
}

static mp_obj_t ml307mod_http_del(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_http_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_del_obj, ml307mod_http_del);

static mp_obj_t ml307mod_http_set_timeout(mp_obj_t self_in, mp_obj_t timeout_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_http_set_timeout(self->handle, mp_obj_get_int(timeout_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_http_set_timeout_obj, ml307mod_http_set_timeout);

static mp_obj_t ml307mod_http_set_header(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *key = mp_obj_str_get_str(key_in);
    const char *value = mp_obj_str_get_str(value_in);
    ml307_http_set_header(self->handle, key, value);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ml307mod_http_set_header_obj, ml307mod_http_set_header);

static mp_obj_t ml307mod_http_set_content(mp_obj_t self_in, mp_obj_t content_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(content_in, &bufinfo, MP_BUFFER_READ);
    ml307_http_set_content(self->handle, bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_http_set_content_obj, ml307mod_http_set_content);

static mp_obj_t ml307mod_http_open(mp_obj_t self_in, mp_obj_t method_in, mp_obj_t url_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *method = mp_obj_str_get_str(method_in);
    const char *url = mp_obj_str_get_str(url_in);
    return mp_obj_new_bool(ml307_http_open(self->handle, method, url));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ml307mod_http_open_obj, ml307mod_http_open);

static mp_obj_t ml307mod_http_close(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_http_close(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_close_obj, ml307mod_http_close);

static mp_obj_t ml307mod_http_status_code(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_http_get_status_code(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_status_code_obj, ml307mod_http_status_code);

static mp_obj_t ml307mod_http_body_length(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_http_get_body_length(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_body_length_obj, ml307mod_http_body_length);

static mp_obj_t ml307mod_http_read(size_t n_args, const mp_obj_t *args) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    size_t size = (n_args > 1) ? mp_obj_get_int(args[1]) : 1024;
    
    char *buffer = m_new(char, size);
    int len = ml307_http_read(self->handle, buffer, size);
    if (len < 0) {
        m_del(char, buffer, size);
        return mp_const_none;
    }
    mp_obj_t result = mp_obj_new_bytes((const byte *)buffer, len);
    m_del(char, buffer, size);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_http_read_obj, 1, 2, ml307mod_http_read);

static mp_obj_t ml307mod_http_read_all(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t body_len = ml307_http_get_body_length(self->handle);
    if (body_len == 0) body_len = 4096;  // Default buffer size
    
    char *buffer = m_new(char, body_len + 1);
    int len = ml307_http_read_all(self->handle, buffer, body_len + 1);
    if (len < 0) {
        m_del(char, buffer, body_len + 1);
        return mp_const_none;
    }
    mp_obj_t result = mp_obj_new_str(buffer, len);
    m_del(char, buffer, body_len + 1);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_read_all_obj, ml307mod_http_read_all);

static mp_obj_t ml307mod_http_last_error(mp_obj_t self_in) {
    ml307mod_http_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_http_get_last_error(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_http_last_error_obj, ml307mod_http_last_error);

static const mp_rom_map_elem_t ml307mod_http_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_http_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_timeout), MP_ROM_PTR(&ml307mod_http_set_timeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_header), MP_ROM_PTR(&ml307mod_http_set_header_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_content), MP_ROM_PTR(&ml307mod_http_set_content_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&ml307mod_http_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ml307mod_http_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_status_code), MP_ROM_PTR(&ml307mod_http_status_code_obj) },
    { MP_ROM_QSTR(MP_QSTR_body_length), MP_ROM_PTR(&ml307mod_http_body_length_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&ml307mod_http_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_all), MP_ROM_PTR(&ml307mod_http_read_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&ml307mod_http_last_error_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_http_locals_dict, ml307mod_http_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_http_type,
    MP_QSTR_Http,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ml307mod_http_locals_dict
);

// ============================================================================
// WebSocket Class
// ============================================================================

typedef struct _ml307mod_websocket_obj_t {
    mp_obj_base_t base;
    ml307_websocket_handle_t handle;
    ml307mod_modem_obj_t *modem;
    mp_obj_t on_data_cb;
    mp_obj_t on_connected_cb;
    mp_obj_t on_disconnected_cb;
    mp_obj_t on_error_cb;
} ml307mod_websocket_obj_t;

// Callback wrappers for WebSocket
static void ws_on_data_wrapper(void* user_data, const char* data, size_t len, bool binary) {
    ml307mod_websocket_obj_t *self = (ml307mod_websocket_obj_t *)user_data;
    if (self->on_data_cb != mp_const_none) {
        mp_obj_t args[2] = {
            mp_obj_new_bytes((const byte *)data, len),
            mp_obj_new_bool(binary)
        };
        mp_call_function_n_kw(self->on_data_cb, 2, 0, args);
    }
}

static void ws_on_connected_wrapper(void* user_data) {
    ml307mod_websocket_obj_t *self = (ml307mod_websocket_obj_t *)user_data;
    if (self->on_connected_cb != mp_const_none) {
        mp_call_function_0(self->on_connected_cb);
    }
}

static void ws_on_disconnected_wrapper(void* user_data) {
    ml307mod_websocket_obj_t *self = (ml307mod_websocket_obj_t *)user_data;
    if (self->on_disconnected_cb != mp_const_none) {
        mp_call_function_0(self->on_disconnected_cb);
    }
}

static void ws_on_error_wrapper(void* user_data, int error) {
    ml307mod_websocket_obj_t *self = (ml307mod_websocket_obj_t *)user_data;
    if (self->on_error_cb != mp_const_none) {
        mp_call_function_1(self->on_error_cb, mp_obj_new_int(error));
    }
}

static mp_obj_t ml307mod_modem_create_websocket(mp_obj_t self_in) {
    ml307mod_modem_obj_t *modem = MP_OBJ_TO_PTR(self_in);
    ml307_websocket_handle_t handle = ml307_websocket_create(modem->handle);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create WebSocket client"));
    }
    ml307mod_websocket_obj_t *ws = mp_obj_malloc(ml307mod_websocket_obj_t, &ml307mod_websocket_type);
    ws->handle = handle;
    ws->modem = modem;
    ws->on_data_cb = mp_const_none;
    ws->on_connected_cb = mp_const_none;
    ws->on_disconnected_cb = mp_const_none;
    ws->on_error_cb = mp_const_none;
    return MP_OBJ_FROM_PTR(ws);
}

static mp_obj_t ml307mod_websocket_del(mp_obj_t self_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_websocket_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_websocket_del_obj, ml307mod_websocket_del);

static mp_obj_t ml307mod_websocket_set_header(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *key = mp_obj_str_get_str(key_in);
    const char *value = mp_obj_str_get_str(value_in);
    ml307_websocket_set_header(self->handle, key, value);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ml307mod_websocket_set_header_obj, ml307mod_websocket_set_header);

static mp_obj_t ml307mod_websocket_connect(mp_obj_t self_in, mp_obj_t url_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *url = mp_obj_str_get_str(url_in);
    return mp_obj_new_bool(ml307_websocket_connect(self->handle, url));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_websocket_connect_obj, ml307mod_websocket_connect);

static mp_obj_t ml307mod_websocket_send(size_t n_args, const mp_obj_t *args) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_READ);
    bool binary = (n_args > 2) ? mp_obj_is_true(args[2]) : false;
    return mp_obj_new_bool(ml307_websocket_send(self->handle, bufinfo.buf, bufinfo.len, binary));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_websocket_send_obj, 2, 3, ml307mod_websocket_send);

static mp_obj_t ml307mod_websocket_ping(mp_obj_t self_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_websocket_ping(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_websocket_ping_obj, ml307mod_websocket_ping);

static mp_obj_t ml307mod_websocket_close(mp_obj_t self_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_websocket_close(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_websocket_close_obj, ml307mod_websocket_close);

static mp_obj_t ml307mod_websocket_is_connected(mp_obj_t self_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_websocket_is_connected(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_websocket_is_connected_obj, ml307mod_websocket_is_connected);

static mp_obj_t ml307mod_websocket_on_data(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_data_cb = callback;
    ml307_websocket_set_callbacks(self->handle, 
        ws_on_data_wrapper, ws_on_connected_wrapper, 
        ws_on_disconnected_wrapper, ws_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_websocket_on_data_obj, ml307mod_websocket_on_data);

static mp_obj_t ml307mod_websocket_on_connected(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_connected_cb = callback;
    ml307_websocket_set_callbacks(self->handle,
        ws_on_data_wrapper, ws_on_connected_wrapper,
        ws_on_disconnected_wrapper, ws_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_websocket_on_connected_obj, ml307mod_websocket_on_connected);

static mp_obj_t ml307mod_websocket_on_disconnected(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_disconnected_cb = callback;
    ml307_websocket_set_callbacks(self->handle,
        ws_on_data_wrapper, ws_on_connected_wrapper,
        ws_on_disconnected_wrapper, ws_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_websocket_on_disconnected_obj, ml307mod_websocket_on_disconnected);

static mp_obj_t ml307mod_websocket_on_error(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_error_cb = callback;
    ml307_websocket_set_callbacks(self->handle,
        ws_on_data_wrapper, ws_on_connected_wrapper,
        ws_on_disconnected_wrapper, ws_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_websocket_on_error_obj, ml307mod_websocket_on_error);

static mp_obj_t ml307mod_websocket_last_error(mp_obj_t self_in) {
    ml307mod_websocket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_websocket_get_last_error(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_websocket_last_error_obj, ml307mod_websocket_last_error);

static const mp_rom_map_elem_t ml307mod_websocket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_websocket_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_header), MP_ROM_PTR(&ml307mod_websocket_set_header_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ml307mod_websocket_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ml307mod_websocket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&ml307mod_websocket_ping_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ml307mod_websocket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&ml307mod_websocket_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_data), MP_ROM_PTR(&ml307mod_websocket_on_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_connected), MP_ROM_PTR(&ml307mod_websocket_on_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_disconnected), MP_ROM_PTR(&ml307mod_websocket_on_disconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_error), MP_ROM_PTR(&ml307mod_websocket_on_error_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&ml307mod_websocket_last_error_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_websocket_locals_dict, ml307mod_websocket_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_websocket_type,
    MP_QSTR_WebSocket,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ml307mod_websocket_locals_dict
);

// ============================================================================
// TCP Class
// ============================================================================

typedef struct _ml307mod_tcp_obj_t {
    mp_obj_base_t base;
    ml307_tcp_handle_t handle;
    ml307mod_modem_obj_t *modem;
    mp_obj_t on_data_cb;
    mp_obj_t on_disconnected_cb;
} ml307mod_tcp_obj_t;

static void tcp_on_data_wrapper(void* user_data, const char* data, size_t len) {
    ml307mod_tcp_obj_t *self = (ml307mod_tcp_obj_t *)user_data;
    if (self->on_data_cb != mp_const_none) {
        mp_obj_t arg = mp_obj_new_bytes((const byte *)data, len);
        mp_call_function_1(self->on_data_cb, arg);
    }
}

static void tcp_on_disconnected_wrapper(void* user_data) {
    ml307mod_tcp_obj_t *self = (ml307mod_tcp_obj_t *)user_data;
    if (self->on_disconnected_cb != mp_const_none) {
        mp_call_function_0(self->on_disconnected_cb);
    }
}

static mp_obj_t ml307mod_modem_create_tcp(size_t n_args, const mp_obj_t *args) {
    ml307mod_modem_obj_t *modem = MP_OBJ_TO_PTR(args[0]);
    bool use_ssl = (n_args > 1) ? mp_obj_is_true(args[1]) : false;
    
    ml307_tcp_handle_t handle = ml307_tcp_create(modem->handle, use_ssl);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create TCP client"));
    }
    ml307mod_tcp_obj_t *tcp = mp_obj_malloc(ml307mod_tcp_obj_t, &ml307mod_tcp_type);
    tcp->handle = handle;
    tcp->modem = modem;
    tcp->on_data_cb = mp_const_none;
    tcp->on_disconnected_cb = mp_const_none;
    return MP_OBJ_FROM_PTR(tcp);
}

static mp_obj_t ml307mod_tcp_del(mp_obj_t self_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_tcp_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_tcp_del_obj, ml307mod_tcp_del);

static mp_obj_t ml307mod_tcp_connect(mp_obj_t self_in, mp_obj_t host_in, mp_obj_t port_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *host = mp_obj_str_get_str(host_in);
    int port = mp_obj_get_int(port_in);
    return mp_obj_new_bool(ml307_tcp_connect(self->handle, host, port));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ml307mod_tcp_connect_obj, ml307mod_tcp_connect);

static mp_obj_t ml307mod_tcp_disconnect(mp_obj_t self_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_tcp_disconnect(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_tcp_disconnect_obj, ml307mod_tcp_disconnect);

static mp_obj_t ml307mod_tcp_send(mp_obj_t self_in, mp_obj_t data_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int sent = ml307_tcp_send(self->handle, bufinfo.buf, bufinfo.len);
    return mp_obj_new_int(sent);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_tcp_send_obj, ml307mod_tcp_send);

static mp_obj_t ml307mod_tcp_is_connected(mp_obj_t self_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_tcp_is_connected(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_tcp_is_connected_obj, ml307mod_tcp_is_connected);

static mp_obj_t ml307mod_tcp_on_data(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_data_cb = callback;
    ml307_tcp_set_callbacks(self->handle, tcp_on_data_wrapper, tcp_on_disconnected_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_tcp_on_data_obj, ml307mod_tcp_on_data);

static mp_obj_t ml307mod_tcp_on_disconnected(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_disconnected_cb = callback;
    ml307_tcp_set_callbacks(self->handle, tcp_on_data_wrapper, tcp_on_disconnected_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_tcp_on_disconnected_obj, ml307mod_tcp_on_disconnected);

static mp_obj_t ml307mod_tcp_last_error(mp_obj_t self_in) {
    ml307mod_tcp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_tcp_get_last_error(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_tcp_last_error_obj, ml307mod_tcp_last_error);

static const mp_rom_map_elem_t ml307mod_tcp_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_tcp_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ml307mod_tcp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ml307mod_tcp_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ml307mod_tcp_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&ml307mod_tcp_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_data), MP_ROM_PTR(&ml307mod_tcp_on_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_disconnected), MP_ROM_PTR(&ml307mod_tcp_on_disconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&ml307mod_tcp_last_error_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_tcp_locals_dict, ml307mod_tcp_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_tcp_type,
    MP_QSTR_Tcp,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ml307mod_tcp_locals_dict
);

// ============================================================================
// UDP Class
// ============================================================================

typedef struct _ml307mod_udp_obj_t {
    mp_obj_base_t base;
    ml307_udp_handle_t handle;
    ml307mod_modem_obj_t *modem;
    mp_obj_t on_message_cb;
} ml307mod_udp_obj_t;

static void udp_on_message_wrapper(void* user_data, const char* data, size_t len) {
    ml307mod_udp_obj_t *self = (ml307mod_udp_obj_t *)user_data;
    if (self->on_message_cb != mp_const_none) {
        mp_obj_t arg = mp_obj_new_bytes((const byte *)data, len);
        mp_call_function_1(self->on_message_cb, arg);
    }
}

static mp_obj_t ml307mod_modem_create_udp(mp_obj_t self_in) {
    ml307mod_modem_obj_t *modem = MP_OBJ_TO_PTR(self_in);
    ml307_udp_handle_t handle = ml307_udp_create(modem->handle);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create UDP client"));
    }
    ml307mod_udp_obj_t *udp = mp_obj_malloc(ml307mod_udp_obj_t, &ml307mod_udp_type);
    udp->handle = handle;
    udp->modem = modem;
    udp->on_message_cb = mp_const_none;
    return MP_OBJ_FROM_PTR(udp);
}

static mp_obj_t ml307mod_udp_del(mp_obj_t self_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_udp_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_udp_del_obj, ml307mod_udp_del);

static mp_obj_t ml307mod_udp_connect(mp_obj_t self_in, mp_obj_t host_in, mp_obj_t port_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *host = mp_obj_str_get_str(host_in);
    int port = mp_obj_get_int(port_in);
    return mp_obj_new_bool(ml307_udp_connect(self->handle, host, port));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ml307mod_udp_connect_obj, ml307mod_udp_connect);

static mp_obj_t ml307mod_udp_disconnect(mp_obj_t self_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_udp_disconnect(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_udp_disconnect_obj, ml307mod_udp_disconnect);

static mp_obj_t ml307mod_udp_send(mp_obj_t self_in, mp_obj_t data_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int sent = ml307_udp_send(self->handle, bufinfo.buf, bufinfo.len);
    return mp_obj_new_int(sent);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_udp_send_obj, ml307mod_udp_send);

static mp_obj_t ml307mod_udp_is_connected(mp_obj_t self_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_udp_is_connected(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_udp_is_connected_obj, ml307mod_udp_is_connected);

static mp_obj_t ml307mod_udp_on_message(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_message_cb = callback;
    ml307_udp_set_callback(self->handle, udp_on_message_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_udp_on_message_obj, ml307mod_udp_on_message);

static mp_obj_t ml307mod_udp_last_error(mp_obj_t self_in) {
    ml307mod_udp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_udp_get_last_error(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_udp_last_error_obj, ml307mod_udp_last_error);

static const mp_rom_map_elem_t ml307mod_udp_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_udp_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ml307mod_udp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ml307mod_udp_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ml307mod_udp_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&ml307mod_udp_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_message), MP_ROM_PTR(&ml307mod_udp_on_message_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&ml307mod_udp_last_error_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_udp_locals_dict, ml307mod_udp_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_udp_type,
    MP_QSTR_Udp,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ml307mod_udp_locals_dict
);

// ============================================================================
// MQTT Class
// ============================================================================

typedef struct _ml307mod_mqtt_obj_t {
    mp_obj_base_t base;
    ml307_mqtt_handle_t handle;
    ml307mod_modem_obj_t *modem;
    mp_obj_t on_message_cb;
    mp_obj_t on_connected_cb;
    mp_obj_t on_disconnected_cb;
    mp_obj_t on_error_cb;
} ml307mod_mqtt_obj_t;

static void mqtt_on_message_wrapper(void* user_data, const char* topic, size_t topic_len,
                                     const char* payload, size_t payload_len) {
    ml307mod_mqtt_obj_t *self = (ml307mod_mqtt_obj_t *)user_data;
    if (self->on_message_cb != mp_const_none) {
        mp_obj_t args[2] = {
            mp_obj_new_str(topic, topic_len),
            mp_obj_new_bytes((const byte *)payload, payload_len)
        };
        mp_call_function_n_kw(self->on_message_cb, 2, 0, args);
    }
}

static void mqtt_on_connected_wrapper(void* user_data) {
    ml307mod_mqtt_obj_t *self = (ml307mod_mqtt_obj_t *)user_data;
    if (self->on_connected_cb != mp_const_none) {
        mp_call_function_0(self->on_connected_cb);
    }
}

static void mqtt_on_disconnected_wrapper(void* user_data) {
    ml307mod_mqtt_obj_t *self = (ml307mod_mqtt_obj_t *)user_data;
    if (self->on_disconnected_cb != mp_const_none) {
        mp_call_function_0(self->on_disconnected_cb);
    }
}

static void mqtt_on_error_wrapper(void* user_data, const char* error) {
    ml307mod_mqtt_obj_t *self = (ml307mod_mqtt_obj_t *)user_data;
    if (self->on_error_cb != mp_const_none) {
        mp_call_function_1(self->on_error_cb, mp_obj_new_str_via_qstr(error, strlen(error)));
    }
}

static mp_obj_t ml307mod_modem_create_mqtt(mp_obj_t self_in) {
    ml307mod_modem_obj_t *modem = MP_OBJ_TO_PTR(self_in);
    ml307_mqtt_handle_t handle = ml307_mqtt_create(modem->handle);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create MQTT client"));
    }
    ml307mod_mqtt_obj_t *mqtt = mp_obj_malloc(ml307mod_mqtt_obj_t, &ml307mod_mqtt_type);
    mqtt->handle = handle;
    mqtt->modem = modem;
    mqtt->on_message_cb = mp_const_none;
    mqtt->on_connected_cb = mp_const_none;
    mqtt->on_disconnected_cb = mp_const_none;
    mqtt->on_error_cb = mp_const_none;
    return MP_OBJ_FROM_PTR(mqtt);
}

static mp_obj_t ml307mod_mqtt_del(mp_obj_t self_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_mqtt_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_mqtt_del_obj, ml307mod_mqtt_del);

static mp_obj_t ml307mod_mqtt_set_keepalive(mp_obj_t self_in, mp_obj_t seconds_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_mqtt_set_keepalive(self->handle, mp_obj_get_int(seconds_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_set_keepalive_obj, ml307mod_mqtt_set_keepalive);

static mp_obj_t ml307mod_mqtt_connect(size_t n_args, const mp_obj_t *args) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *broker = mp_obj_str_get_str(args[1]);
    int port = mp_obj_get_int(args[2]);
    const char *client_id = mp_obj_str_get_str(args[3]);
    const char *username = (n_args > 4 && args[4] != mp_const_none) ? mp_obj_str_get_str(args[4]) : "";
    const char *password = (n_args > 5 && args[5] != mp_const_none) ? mp_obj_str_get_str(args[5]) : "";
    return mp_obj_new_bool(ml307_mqtt_connect(self->handle, broker, port, client_id, username, password));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_mqtt_connect_obj, 4, 6, ml307mod_mqtt_connect);

static mp_obj_t ml307mod_mqtt_disconnect(mp_obj_t self_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_mqtt_disconnect(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_mqtt_disconnect_obj, ml307mod_mqtt_disconnect);

static mp_obj_t ml307mod_mqtt_publish(size_t n_args, const mp_obj_t *args) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *topic = mp_obj_str_get_str(args[1]);
    const char *payload = mp_obj_str_get_str(args[2]);
    int qos = (n_args > 3) ? mp_obj_get_int(args[3]) : 0;
    return mp_obj_new_bool(ml307_mqtt_publish(self->handle, topic, payload, qos));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_mqtt_publish_obj, 3, 4, ml307mod_mqtt_publish);

static mp_obj_t ml307mod_mqtt_subscribe(size_t n_args, const mp_obj_t *args) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *topic = mp_obj_str_get_str(args[1]);
    int qos = (n_args > 2) ? mp_obj_get_int(args[2]) : 0;
    return mp_obj_new_bool(ml307_mqtt_subscribe(self->handle, topic, qos));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_mqtt_subscribe_obj, 2, 3, ml307mod_mqtt_subscribe);

static mp_obj_t ml307mod_mqtt_unsubscribe(mp_obj_t self_in, mp_obj_t topic_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *topic = mp_obj_str_get_str(topic_in);
    return mp_obj_new_bool(ml307_mqtt_unsubscribe(self->handle, topic));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_unsubscribe_obj, ml307mod_mqtt_unsubscribe);

static mp_obj_t ml307mod_mqtt_is_connected(mp_obj_t self_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_mqtt_is_connected(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_mqtt_is_connected_obj, ml307mod_mqtt_is_connected);

static mp_obj_t ml307mod_mqtt_on_message(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_message_cb = callback;
    ml307_mqtt_set_callbacks(self->handle, mqtt_on_message_wrapper, mqtt_on_connected_wrapper,
                             mqtt_on_disconnected_wrapper, mqtt_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_on_message_obj, ml307mod_mqtt_on_message);

static mp_obj_t ml307mod_mqtt_on_connected(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_connected_cb = callback;
    ml307_mqtt_set_callbacks(self->handle, mqtt_on_message_wrapper, mqtt_on_connected_wrapper,
                             mqtt_on_disconnected_wrapper, mqtt_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_on_connected_obj, ml307mod_mqtt_on_connected);

static mp_obj_t ml307mod_mqtt_on_disconnected(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_disconnected_cb = callback;
    ml307_mqtt_set_callbacks(self->handle, mqtt_on_message_wrapper, mqtt_on_connected_wrapper,
                             mqtt_on_disconnected_wrapper, mqtt_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_on_disconnected_obj, ml307mod_mqtt_on_disconnected);

static mp_obj_t ml307mod_mqtt_on_error(mp_obj_t self_in, mp_obj_t callback) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->on_error_cb = callback;
    ml307_mqtt_set_callbacks(self->handle, mqtt_on_message_wrapper, mqtt_on_connected_wrapper,
                             mqtt_on_disconnected_wrapper, mqtt_on_error_wrapper, self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307mod_mqtt_on_error_obj, ml307mod_mqtt_on_error);

static mp_obj_t ml307mod_mqtt_last_error(mp_obj_t self_in) {
    ml307mod_mqtt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_mqtt_get_last_error(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_mqtt_last_error_obj, ml307mod_mqtt_last_error);

static const mp_rom_map_elem_t ml307mod_mqtt_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ml307mod_mqtt_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_keepalive), MP_ROM_PTR(&ml307mod_mqtt_set_keepalive_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ml307mod_mqtt_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ml307mod_mqtt_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_publish), MP_ROM_PTR(&ml307mod_mqtt_publish_obj) },
    { MP_ROM_QSTR(MP_QSTR_subscribe), MP_ROM_PTR(&ml307mod_mqtt_subscribe_obj) },
    { MP_ROM_QSTR(MP_QSTR_unsubscribe), MP_ROM_PTR(&ml307mod_mqtt_unsubscribe_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&ml307mod_mqtt_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_message), MP_ROM_PTR(&ml307mod_mqtt_on_message_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_connected), MP_ROM_PTR(&ml307mod_mqtt_on_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_disconnected), MP_ROM_PTR(&ml307mod_mqtt_on_disconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_error), MP_ROM_PTR(&ml307mod_mqtt_on_error_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_error), MP_ROM_PTR(&ml307mod_mqtt_last_error_obj) },
};
static MP_DEFINE_CONST_DICT(ml307mod_mqtt_locals_dict, ml307mod_mqtt_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_mqtt_type,
    MP_QSTR_Mqtt,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ml307mod_mqtt_locals_dict
);

// ============================================================================
// Module Level Functions
// ============================================================================

/**
 * @brief Check if a modem instance is already initialized
 * @return True if modem is cached and available
 */
static mp_obj_t ml307mod_is_initialized(void) {
    return mp_obj_new_bool(ml307_is_initialized());
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307mod_is_initialized_obj, ml307mod_is_initialized);

/**
 * @brief Get the cached modem object if available
 * @return Cached Modem object or None
 * 
 * This function handles soft reboot by re-wrapping C layer cache
 * if Python layer cache is invalid.
 */
static mp_obj_t ml307mod_get_modem(void) {
    // Check if C layer has cache
    if (!ml307_is_initialized()) {
        return mp_const_none;
    }
    
    // C layer has cache, check if Python layer cache is valid
    if (g_cached_modem_obj != MP_OBJ_NULL) {
        if (mp_obj_is_type(g_cached_modem_obj, &ml307mod_modem_type)) {
            ml307mod_modem_obj_t *cached = MP_OBJ_TO_PTR(g_cached_modem_obj);
            if (cached->handle == ml307_get_cached()) {
                // Both caches valid
                return g_cached_modem_obj;
            }
        }
    }
    
    // Python cache invalid (soft reboot), re-wrap C layer cache
    // Note: We don't know original pin config, use defaults
    ml307mod_modem_obj_t *self = mp_obj_malloc(ml307mod_modem_obj_t, &ml307mod_modem_type);
    self->handle = ml307_get_cached();
    self->tx_pin = -1;  // Unknown after soft reboot
    self->rx_pin = -1;
    self->dtr_pin = -1;
    self->baud_rate = 0;
    
    g_cached_modem_obj = MP_OBJ_FROM_PTR(self);
    return g_cached_modem_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307mod_get_modem_obj, ml307mod_get_modem);

/**
 * @brief Force destroy the cached modem (use with caution)
 */
static mp_obj_t ml307mod_force_destroy(void) {
    ml307_force_destroy();
    g_cached_modem_obj = MP_OBJ_NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307mod_force_destroy_obj, ml307mod_force_destroy);

// ============================================================================
// Module Definition
// ============================================================================

static const mp_rom_map_elem_t ml307mod_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ml307) },
    
    // Classes
    { MP_ROM_QSTR(MP_QSTR_Modem), MP_ROM_PTR(&ml307mod_modem_type) },
    
    // Module level functions for singleton access
    { MP_ROM_QSTR(MP_QSTR_is_initialized), MP_ROM_PTR(&ml307mod_is_initialized_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_modem), MP_ROM_PTR(&ml307mod_get_modem_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_destroy), MP_ROM_PTR(&ml307mod_force_destroy_obj) },
    
    // Network status constants
    { MP_ROM_QSTR(MP_QSTR_STATUS_READY), MP_ROM_INT(ML307_STATUS_READY) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR), MP_ROM_INT(ML307_STATUS_ERROR) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR_INSERT_PIN), MP_ROM_INT(ML307_STATUS_ERROR_INSERT_PIN) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR_REGISTRATION_DENIED), MP_ROM_INT(ML307_STATUS_ERROR_REGISTRATION_DENIED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR_TIMEOUT), MP_ROM_INT(ML307_STATUS_ERROR_TIMEOUT) },
};
static MP_DEFINE_CONST_DICT(ml307mod_module_globals, ml307mod_module_globals_table);

const mp_obj_module_t ml307mod_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&ml307mod_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ml307, ml307mod_user_cmodule);

