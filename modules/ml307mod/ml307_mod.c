/*
 * ML307R MicroPython C Extension Module
 *
 * Exposes the ML307 AT engine to Python as the `_ml307` module.
 * Used by ml307_socket.py for high-performance 4G networking.
 *
 * Python API:
 *   import _ml307
 *   _ml307.init(tx=12, rx=11, baudrate=921600, apn="CMNET", debug=False)
 *   sid = _ml307.alloc()
 *   _ml307.connect(sid, "example.com", 443, ssl=True)
 *   _ml307.send(sid, b"data")
 *   data = _ml307.recv(sid, 4096, timeout_ms=5000)
 *   _ml307.close(sid)
 */

#include <string.h>
#include <stdarg.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mpprint.h"
#include "py/mpthread.h"

#include "ml307_at.h"

/* Single global modem instance */
static ml307_state_t ml307_state;
static bool ml307_inited = false;

/* ---- Logging: output to MicroPython REPL via mp_vprintf ---- */
static void ml307_mp_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mp_vprintf(&mp_plat_print, fmt, args);
    va_end(args);
}

/* ---- Helper: raise OSError with errno ---- */
static void raise_os_error(int err) {
    mp_raise_OSError(err);
}

/* ---- init(tx=12, rx=11, baudrate=921600, apn="CMNET", debug=False) ---- */
static mp_obj_t ml307_mod_init(size_t n_args, const mp_obj_t *pos_args,
                                mp_map_t *kw_args) {
    enum { ARG_tx, ARG_rx, ARG_baudrate, ARG_apn, ARG_debug };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_tx,       MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 12} },
        { MP_QSTR_rx,       MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 11} },
        { MP_QSTR_baudrate, MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 921600} },
        { MP_QSTR_apn,      MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_CMNET)} },
        { MP_QSTR_debug,    MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int tx = args[ARG_tx].u_int;
    int rx = args[ARG_rx].u_int;
    int baud = args[ARG_baudrate].u_int;
    const char *apn = mp_obj_str_get_str(args[ARG_apn].u_obj);
    bool debug = args[ARG_debug].u_bool;

    /* Always deinit to clean up resources from previous init (even failed ones).
     * Set uart_num so deinit deletes the correct UART, not UART0. */
    ml307_state.uart_num = UART_NUM_1;
    ml307_deinit(&ml307_state);
    ml307_inited = false;

    mp_printf(&mp_plat_print, "[4G] ML307 init: tx=%d rx=%d baud=%d\n",
              tx, rx, baud);

    /* Set logging callback so C code outputs to MicroPython REPL */
    ml307_set_log_fn(ml307_mp_log);

    int rc = ml307_init(&ml307_state, tx, rx, baud, apn, debug);

    if (rc != 0) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("ML307 init failed: %d"), rc);
    }

    ml307_inited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ml307_mod_init_obj, 0, ml307_mod_init);

/* ---- deinit() ---- */
static mp_obj_t ml307_mod_deinit(void) {
    if (ml307_inited) {
        ml307_deinit(&ml307_state);
        ml307_inited = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307_mod_deinit_obj, ml307_mod_deinit);

/* ---- Check initialized ---- */
static inline void check_init(void) {
    if (!ml307_inited) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("ML307 not initialized - call _ml307.init()"));
    }
}

/* ---- status() → dict ---- */
static mp_obj_t ml307_mod_status(void) {
    check_init();
    mp_obj_t dict = mp_obj_new_dict(4);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip),
        ml307_state.ip[0] ? mp_obj_new_str(ml307_state.ip, strlen(ml307_state.ip))
                          : mp_const_none);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_csq),
        MP_OBJ_NEW_SMALL_INT(ml307_state.csq));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_registered),
        mp_obj_new_bool(ml307_state.registered));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_initialized),
        mp_obj_new_bool(ml307_state.initialized));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307_mod_status_obj, ml307_mod_status);

/* ---- is_connected() → bool ---- */
static mp_obj_t ml307_mod_is_connected(void) {
    if (!ml307_inited) return mp_const_false;
    return mp_obj_new_bool(ml307_state.initialized && ml307_state.ip[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307_mod_is_connected_obj, ml307_mod_is_connected);

/* ---- alloc() → int ---- */
static mp_obj_t ml307_mod_alloc(void) {
    check_init();
    int sid = ml307_sock_alloc(&ml307_state);
    if (sid < 0) {
        raise_os_error(23); /* ENFILE */
    }
    return MP_OBJ_NEW_SMALL_INT(sid);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ml307_mod_alloc_obj, ml307_mod_alloc);

/* ---- free(sid) ---- */
static mp_obj_t ml307_mod_free(mp_obj_t sid_obj) {
    check_init();
    int sid = mp_obj_get_int(sid_obj);
    ml307_sock_free(&ml307_state, sid);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307_mod_free_obj, ml307_mod_free);

/* ---- connect(sid, host, port, ssl=False, timeout=20000) ---- */
static mp_obj_t ml307_mod_connect(size_t n_args, const mp_obj_t *pos_args,
                                   mp_map_t *kw_args) {
    enum { ARG_sid, ARG_host, ARG_port, ARG_ssl, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sid,     MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_host,    MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_port,    MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_ssl,     MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 20000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    check_init();
    int sid = args[ARG_sid].u_int;
    const char *host = mp_obj_str_get_str(args[ARG_host].u_obj);
    int port = args[ARG_port].u_int;
    bool ssl = args[ARG_ssl].u_bool;
    int timeout = args[ARG_timeout].u_int;

    MP_THREAD_GIL_EXIT();
    int rc = ml307_sock_connect(&ml307_state, sid, host, port, ssl, timeout);
    MP_THREAD_GIL_ENTER();

    if (rc != 0) {
        raise_os_error(111); /* ECONNREFUSED */
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ml307_mod_connect_obj, 3, ml307_mod_connect);

/* ---- send(sid, data) → int ---- */
static mp_obj_t ml307_mod_send(mp_obj_t sid_obj, mp_obj_t data_obj) {
    check_init();
    int sid = mp_obj_get_int(sid_obj);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);

    MP_THREAD_GIL_EXIT();
    int n = ml307_sock_send(&ml307_state, sid, bufinfo.buf, bufinfo.len);
    MP_THREAD_GIL_ENTER();

    if (n < 0) {
        raise_os_error(32); /* EPIPE */
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ml307_mod_send_obj, ml307_mod_send);

/* ---- recv(sid, maxlen, timeout_ms=-1) → bytes ---- */
static mp_obj_t ml307_mod_recv(size_t n_args, const mp_obj_t *args) {
    check_init();
    int sid = mp_obj_get_int(args[0]);
    int maxlen = mp_obj_get_int(args[1]);
    int timeout_ms = (n_args > 2) ? mp_obj_get_int(args[2]) : -1;

    if (maxlen <= 0) maxlen = 4096;

    /* Allocate temp buffer */
    uint8_t *buf = m_new(uint8_t, maxlen);

    MP_THREAD_GIL_EXIT();
    int n = ml307_sock_recv(&ml307_state, sid, buf, maxlen, timeout_ms);
    MP_THREAD_GIL_ENTER();

    if (n < 0) {
        m_del(uint8_t, buf, maxlen);
        raise_os_error(107); /* ENOTCONN */
    }

    mp_obj_t result = mp_obj_new_bytes(buf, n);
    m_del(uint8_t, buf, maxlen);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307_mod_recv_obj, 2, 3, ml307_mod_recv);

/* ---- available(sid) → int ---- */
static mp_obj_t ml307_mod_available(mp_obj_t sid_obj) {
    check_init();
    return MP_OBJ_NEW_SMALL_INT(
        ml307_sock_available(&ml307_state, mp_obj_get_int(sid_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307_mod_available_obj, ml307_mod_available);

/* ---- is_disconnected(sid) → bool ---- */
static mp_obj_t ml307_mod_is_disconnected(mp_obj_t sid_obj) {
    check_init();
    return mp_obj_new_bool(
        ml307_sock_is_disconnected(&ml307_state, mp_obj_get_int(sid_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307_mod_is_disconnected_obj, ml307_mod_is_disconnected);

/* ---- close(sid) ---- */
static mp_obj_t ml307_mod_close(mp_obj_t sid_obj) {
    check_init();
    int sid = mp_obj_get_int(sid_obj);
    MP_THREAD_GIL_EXIT();
    ml307_sock_close(&ml307_state, sid);
    MP_THREAD_GIL_ENTER();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ml307_mod_close_obj, ml307_mod_close);

/* ---- at_cmd(cmd, timeout=3000) → str ---- */
static mp_obj_t ml307_mod_at_cmd(size_t n_args, const mp_obj_t *args) {
    check_init();
    const char *cmd = mp_obj_str_get_str(args[0]);
    int timeout = (n_args > 1) ? mp_obj_get_int(args[1]) : 3000;

    char resp[512];

    MP_THREAD_GIL_EXIT();
    ml307_send_at(&ml307_state, cmd, resp, sizeof(resp), timeout);
    MP_THREAD_GIL_ENTER();

    return mp_obj_new_str(resp, strlen(resp));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ml307_mod_at_cmd_obj, 1, 2, ml307_mod_at_cmd);

/* ---- Module Definition ---- */

static const mp_rom_map_elem_t ml307_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),        MP_ROM_QSTR(MP_QSTR__ml307) },
    { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&ml307_mod_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&ml307_mod_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_status),          MP_ROM_PTR(&ml307_mod_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_connected),    MP_ROM_PTR(&ml307_mod_is_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_alloc),           MP_ROM_PTR(&ml307_mod_alloc_obj) },
    { MP_ROM_QSTR(MP_QSTR_free),            MP_ROM_PTR(&ml307_mod_free_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect),         MP_ROM_PTR(&ml307_mod_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),            MP_ROM_PTR(&ml307_mod_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),            MP_ROM_PTR(&ml307_mod_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_available),       MP_ROM_PTR(&ml307_mod_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_disconnected), MP_ROM_PTR(&ml307_mod_is_disconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),           MP_ROM_PTR(&ml307_mod_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_at_cmd),          MP_ROM_PTR(&ml307_mod_at_cmd_obj) },
};
static MP_DEFINE_CONST_DICT(ml307_module_globals, ml307_module_globals_table);

const mp_obj_module_t ml307_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ml307_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__ml307, ml307_cmodule);
