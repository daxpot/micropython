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
#include "ml307call.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"

// ----------- ML307 Modem 对象 -----------

typedef struct _ml307mod_modem_obj_t {
    mp_obj_base_t base;
    ml307_modem_handle_t handle;
    int tx_pin;
    int rx_pin;
    int dtr_pin;
    int baud_rate;
    TaskHandle_t uart_event_task;
    QueueHandle_t uart_queue;
} ml307mod_modem_obj_t;

/**
 * @brief Destructor for modem object (called by GC)
 */
static mp_obj_t ml307mod_modem_del(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_del_obj, ml307mod_modem_del);

/**
 * @brief Create new Modem object
 * 
 * Args:
 *   tx_pin (int): UART TX pin number
 *   rx_pin (int): UART RX pin number
 *   dtr_pin (int, optional): DTR pin number, default -1 (not connected)
 *   baud_rate (int, optional): UART baud rate, default 115200
 *   timeout_ms (int, optional): Detection timeout in ms, default -1 (auto)
 */
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
    
    // Validate pin numbers
    if (tx_pin < 0 || rx_pin < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("tx_pin and rx_pin must be valid GPIO numbers"));
    }
    // Detect and initialize modem
    ml307_modem_handle_t handle = ml307_detect(tx_pin, rx_pin, dtr_pin, baud_rate, timeout_ms);
    if (!handle) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Modem detection failed"));
    }

    ml307mod_modem_obj_t *self = mp_obj_malloc(ml307mod_modem_obj_t, type);
    // Create object
    self->handle = handle;
    self->tx_pin = tx_pin;
    self->rx_pin = rx_pin;
    self->dtr_pin = dtr_pin;
    self->baud_rate = baud_rate;
    
    return MP_OBJ_FROM_PTR(self);
}


/**
 * @brief Wait for network to become ready
 * 
 * Args:
 *   timeout_ms (int, optional): Timeout in milliseconds, default 30000
 * 
 * Returns:
 *   int: Network status code (STATUS_READY, STATUS_ERROR, etc.)
 */
static mp_obj_t ml307mod_modem_wait_for_network(size_t n_args, const mp_obj_t *args) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    
    int timeout_ms = 30000; // Default 30 seconds
    if (n_args > 1) {
        timeout_ms = mp_obj_get_int(args[1]);
    }
    
    ml307_network_status_t status = ml307_wait_for_network_ready(self->handle, timeout_ms);
    return mp_obj_new_int(status);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307mod_modem_wait_for_network_obj, 1, 2, ml307mod_modem_wait_for_network);

/**
 * @brief Check if network is ready
 * 
 * Returns:
 *   bool: True if network is ready
 */
static mp_obj_t ml307mod_modem_is_ready(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(ml307_is_network_ready(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_is_ready_obj, ml307mod_modem_is_ready);

/**
 * @brief Reboot the modem
 */
static mp_obj_t ml307mod_modem_reboot(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    ml307_reboot(self->handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_reboot_obj, ml307mod_modem_reboot);

/**
 * @brief Get modem IMEI
 * 
 * Returns:
 *   str: IMEI string
 */
static mp_obj_t ml307mod_modem_imei(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[32];
    int len = ml307_get_imei(self->handle, buffer, sizeof(buffer));
    if (len < 0) {
        return mp_const_none;
    }
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_imei_obj, ml307mod_modem_imei);

/**
 * @brief Get SIM card ICCID
 * 
 * Returns:
 *   str: ICCID string
 */
static mp_obj_t ml307mod_modem_iccid(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[32];
    int len = ml307_get_iccid(self->handle, buffer, sizeof(buffer));
    if (len < 0) {
        return mp_const_none;
    }
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_iccid_obj, ml307mod_modem_iccid);

/**
 * @brief Get modem firmware revision
 * 
 * Returns:
 *   str: Firmware revision string
 */
static mp_obj_t ml307mod_modem_revision(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[64];
    int len = ml307_get_module_revision(self->handle, buffer, sizeof(buffer));
    if (len < 0) {
        return mp_const_none;
    }
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_revision_obj, ml307mod_modem_revision);

/**
 * @brief Get carrier name
 * 
 * Returns:
 *   str: Carrier name string
 */
static mp_obj_t ml307mod_modem_carrier(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char buffer[64];
    int len = ml307_get_carrier_name(self->handle, buffer, sizeof(buffer));
    if (len < 0) {
        return mp_const_none;
    }
    return mp_obj_new_str(buffer, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_carrier_obj, ml307mod_modem_carrier);

/**
 * @brief Get signal strength (CSQ value)
 * 
 * Returns:
 *   int: CSQ value (0-31), or -1 on error
 */
static mp_obj_t ml307mod_modem_csq(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(ml307_get_csq(self->handle));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_csq_obj, ml307mod_modem_csq);

/**
 * @brief Close modem connection and release resources
 */
static mp_obj_t ml307mod_modem_close(mp_obj_t self_in) {
    ml307mod_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle) {
        ml307_destroy(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307mod_modem_close_obj, ml307mod_modem_close);

// Modem class methods table
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
};
static MP_DEFINE_CONST_DICT(ml307mod_modem_locals_dict, ml307mod_modem_locals_dict_table);

// Modem type definition
static MP_DEFINE_CONST_OBJ_TYPE(
    ml307mod_modem_type,
    MP_QSTR_Modem,
    MP_TYPE_FLAG_NONE,
    make_new, ml307mod_modem_make_new,
    locals_dict, &ml307mod_modem_locals_dict
);

// ----------- 模块定义 -----------

static const mp_rom_map_elem_t ml307mod_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ml307) },
    
    // Classes
    { MP_ROM_QSTR(MP_QSTR_Modem), MP_ROM_PTR(&ml307mod_modem_type) },
    
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

