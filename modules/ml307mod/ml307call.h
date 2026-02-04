/**
 * @file ml307call.h
 * @brief C wrapper for esp-ml307 C++ library
 * 
 * This header provides C-compatible interfaces for MicroPython bindings
 * to access the ML307/EC801E/NT26K 4G modem functionality.
 */

#ifndef ML307CALL_H
#define ML307CALL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network status codes
 */
typedef enum {
    ML307_STATUS_READY = 0,
    ML307_STATUS_ERROR = 1,
    ML307_STATUS_ERROR_INSERT_PIN = -1,
    ML307_STATUS_ERROR_REGISTRATION_DENIED = -2,
    ML307_STATUS_ERROR_TIMEOUT = -3,
} ml307_network_status_t;

/**
 * @brief Opaque handles
 */
typedef void* ml307_modem_handle_t;
typedef void* ml307_http_handle_t;
typedef void* ml307_websocket_handle_t;
typedef void* ml307_tcp_handle_t;
typedef void* ml307_udp_handle_t;
typedef void* ml307_mqtt_handle_t;

// ============================================================================
// Modem API
// ============================================================================

/**
 * @brief Detect and initialize modem (singleton with caching)
 * 
 * If a modem has already been initialized with the same pin configuration,
 * returns the cached instance instead of reinitializing.
 * This prevents UART reinitialization issues.
 */
ml307_modem_handle_t ml307_detect(int tx_pin, int rx_pin, int dtr_pin, int baud_rate, int timeout_ms);

/**
 * @brief Get cached modem handle if available
 * @return Cached modem handle, or NULL if not initialized
 */
ml307_modem_handle_t ml307_get_cached(void);

/**
 * @brief Check if modem is already initialized
 * @return true if a modem instance exists in cache
 */
bool ml307_is_initialized(void);

/**
 * @brief Destroy modem instance (reference counted)
 * 
 * Only actually destroys when the last reference is released.
 */
void ml307_destroy(ml307_modem_handle_t handle);

/**
 * @brief Force destroy and clear cache regardless of reference count
 */
void ml307_force_destroy(void);
ml307_network_status_t ml307_wait_for_network_ready(ml307_modem_handle_t handle, int timeout_ms);
bool ml307_is_network_ready(ml307_modem_handle_t handle);
void ml307_reboot(ml307_modem_handle_t handle);
int ml307_get_imei(ml307_modem_handle_t handle, char* buffer, int buffer_size);
int ml307_get_iccid(ml307_modem_handle_t handle, char* buffer, int buffer_size);
int ml307_get_module_revision(ml307_modem_handle_t handle, char* buffer, int buffer_size);
int ml307_get_carrier_name(ml307_modem_handle_t handle, char* buffer, int buffer_size);
int ml307_get_csq(ml307_modem_handle_t handle);

// ============================================================================
// HTTP API
// ============================================================================

ml307_http_handle_t ml307_http_create(ml307_modem_handle_t modem);
void ml307_http_destroy(ml307_http_handle_t handle);
void ml307_http_set_timeout(ml307_http_handle_t handle, int timeout_ms);
void ml307_http_set_header(ml307_http_handle_t handle, const char* key, const char* value);
void ml307_http_set_content(ml307_http_handle_t handle, const char* content, size_t len);
bool ml307_http_open(ml307_http_handle_t handle, const char* method, const char* url);
void ml307_http_close(ml307_http_handle_t handle);
int ml307_http_get_status_code(ml307_http_handle_t handle);
size_t ml307_http_get_body_length(ml307_http_handle_t handle);
int ml307_http_read(ml307_http_handle_t handle, char* buffer, size_t buffer_size);
int ml307_http_read_all(ml307_http_handle_t handle, char* buffer, size_t buffer_size);
int ml307_http_get_last_error(ml307_http_handle_t handle);

// ============================================================================
// WebSocket API
// ============================================================================

/** @brief WebSocket data callback type */
typedef void (*ml307_ws_data_callback_t)(void* user_data, const char* data, size_t len, bool binary);
typedef void (*ml307_ws_event_callback_t)(void* user_data);
typedef void (*ml307_ws_error_callback_t)(void* user_data, int error);

ml307_websocket_handle_t ml307_websocket_create(ml307_modem_handle_t modem);
void ml307_websocket_destroy(ml307_websocket_handle_t handle);
void ml307_websocket_set_header(ml307_websocket_handle_t handle, const char* key, const char* value);
bool ml307_websocket_connect(ml307_websocket_handle_t handle, const char* url);
bool ml307_websocket_send(ml307_websocket_handle_t handle, const char* data, size_t len, bool binary);
void ml307_websocket_ping(ml307_websocket_handle_t handle);
void ml307_websocket_close(ml307_websocket_handle_t handle);
bool ml307_websocket_is_connected(ml307_websocket_handle_t handle);
void ml307_websocket_set_callbacks(ml307_websocket_handle_t handle,
                                   ml307_ws_data_callback_t on_data,
                                   ml307_ws_event_callback_t on_connected,
                                   ml307_ws_event_callback_t on_disconnected,
                                   ml307_ws_error_callback_t on_error,
                                   void* user_data);
int ml307_websocket_get_last_error(ml307_websocket_handle_t handle);

// ============================================================================
// TCP API
// ============================================================================

/** @brief TCP data callback type */
typedef void (*ml307_tcp_data_callback_t)(void* user_data, const char* data, size_t len);
typedef void (*ml307_tcp_event_callback_t)(void* user_data);

ml307_tcp_handle_t ml307_tcp_create(ml307_modem_handle_t modem, bool use_ssl);
void ml307_tcp_destroy(ml307_tcp_handle_t handle);
bool ml307_tcp_connect(ml307_tcp_handle_t handle, const char* host, int port);
void ml307_tcp_disconnect(ml307_tcp_handle_t handle);
int ml307_tcp_send(ml307_tcp_handle_t handle, const char* data, size_t len);
bool ml307_tcp_is_connected(ml307_tcp_handle_t handle);
void ml307_tcp_set_callbacks(ml307_tcp_handle_t handle,
                             ml307_tcp_data_callback_t on_data,
                             ml307_tcp_event_callback_t on_disconnected,
                             void* user_data);
int ml307_tcp_get_last_error(ml307_tcp_handle_t handle);

// ============================================================================
// UDP API
// ============================================================================

/** @brief UDP data callback type */
typedef void (*ml307_udp_data_callback_t)(void* user_data, const char* data, size_t len);

ml307_udp_handle_t ml307_udp_create(ml307_modem_handle_t modem);
void ml307_udp_destroy(ml307_udp_handle_t handle);
bool ml307_udp_connect(ml307_udp_handle_t handle, const char* host, int port);
void ml307_udp_disconnect(ml307_udp_handle_t handle);
int ml307_udp_send(ml307_udp_handle_t handle, const char* data, size_t len);
bool ml307_udp_is_connected(ml307_udp_handle_t handle);
void ml307_udp_set_callback(ml307_udp_handle_t handle,
                            ml307_udp_data_callback_t on_message,
                            void* user_data);
int ml307_udp_get_last_error(ml307_udp_handle_t handle);

// ============================================================================
// MQTT API
// ============================================================================

/** @brief MQTT message callback type */
typedef void (*ml307_mqtt_message_callback_t)(void* user_data, const char* topic, size_t topic_len,
                                               const char* payload, size_t payload_len);
typedef void (*ml307_mqtt_event_callback_t)(void* user_data);
typedef void (*ml307_mqtt_error_callback_t)(void* user_data, const char* error);

ml307_mqtt_handle_t ml307_mqtt_create(ml307_modem_handle_t modem);
void ml307_mqtt_destroy(ml307_mqtt_handle_t handle);
void ml307_mqtt_set_keepalive(ml307_mqtt_handle_t handle, int seconds);
bool ml307_mqtt_connect(ml307_mqtt_handle_t handle, const char* broker, int port,
                        const char* client_id, const char* username, const char* password);
void ml307_mqtt_disconnect(ml307_mqtt_handle_t handle);
bool ml307_mqtt_publish(ml307_mqtt_handle_t handle, const char* topic, const char* payload, int qos);
bool ml307_mqtt_subscribe(ml307_mqtt_handle_t handle, const char* topic, int qos);
bool ml307_mqtt_unsubscribe(ml307_mqtt_handle_t handle, const char* topic);
bool ml307_mqtt_is_connected(ml307_mqtt_handle_t handle);
void ml307_mqtt_set_callbacks(ml307_mqtt_handle_t handle,
                              ml307_mqtt_message_callback_t on_message,
                              ml307_mqtt_event_callback_t on_connected,
                              ml307_mqtt_event_callback_t on_disconnected,
                              ml307_mqtt_error_callback_t on_error,
                              void* user_data);
int ml307_mqtt_get_last_error(ml307_mqtt_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // ML307CALL_H

