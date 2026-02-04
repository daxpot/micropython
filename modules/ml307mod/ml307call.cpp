/**
 * @file ml307call.cpp
 * @brief C wrapper implementation for esp-ml307 C++ library
 * 
 * This file provides C-compatible wrapper functions around the esp-ml307 
 * C++ classes for use by MicroPython bindings.
 */

#include "ml307call.h"
#include "at_modem.h"
#include "http.h"
#include "web_socket.h"
#include "tcp.h"
#include "udp.h"
#include "mqtt.h"
#include <driver/gpio.h>
#include <memory>
#include <cstring>
#include <string>

// ============================================================================
// Internal Structures
// ============================================================================

struct ml307_modem_context {
    std::unique_ptr<AtModem> modem;
};

struct ml307_http_context {
    std::unique_ptr<Http> http;
    ml307_modem_context* modem_ctx;
    std::string content;
};

struct ml307_websocket_context {
    std::unique_ptr<WebSocket> ws;
    ml307_modem_context* modem_ctx;
    ml307_ws_data_callback_t on_data;
    ml307_ws_event_callback_t on_connected;
    ml307_ws_event_callback_t on_disconnected;
    ml307_ws_error_callback_t on_error;
    void* user_data;
};

struct ml307_tcp_context {
    std::unique_ptr<Tcp> tcp;
    ml307_modem_context* modem_ctx;
    ml307_tcp_data_callback_t on_data;
    ml307_tcp_event_callback_t on_disconnected;
    void* user_data;
};

struct ml307_udp_context {
    std::unique_ptr<Udp> udp;
    ml307_modem_context* modem_ctx;
    ml307_udp_data_callback_t on_message;
    void* user_data;
};

struct ml307_mqtt_context {
    std::unique_ptr<Mqtt> mqtt;
    ml307_modem_context* modem_ctx;
    ml307_mqtt_message_callback_t on_message;
    ml307_mqtt_event_callback_t on_connected;
    ml307_mqtt_event_callback_t on_disconnected;
    ml307_mqtt_error_callback_t on_error;
    void* user_data;
};

// ============================================================================
// Helper Functions
// ============================================================================

static int copy_string_to_buffer(const std::string& src, char* buffer, int buffer_size) {
    if (!buffer || buffer_size <= 0) {
        return -1;
    }
    int len = static_cast<int>(src.length());
    if (len >= buffer_size) {
        len = buffer_size - 1;
    }
    std::memcpy(buffer, src.c_str(), len);
    buffer[len] = '\0';
    return len;
}

// ============================================================================
// Modem Singleton Cache
// ============================================================================

static struct {
    ml307_modem_context* ctx;
    int tx_pin;
    int rx_pin;
    int dtr_pin;
    int baud_rate;
    int ref_count;
} g_modem_cache = {nullptr, -1, -1, -1, 0, 0};

// ============================================================================
// Modem API Implementation
// ============================================================================

ml307_modem_handle_t ml307_detect(int tx_pin, int rx_pin, int dtr_pin, int baud_rate, int timeout_ms) {
    if (tx_pin < 0 || rx_pin < 0) {
        return nullptr;
    }
    
    if (baud_rate <= 0) {
        baud_rate = 115200;
    }
    
    // Check if we already have a cached modem with same configuration
    if (g_modem_cache.ctx != nullptr &&
        g_modem_cache.tx_pin == tx_pin &&
        g_modem_cache.rx_pin == rx_pin &&
        g_modem_cache.dtr_pin == dtr_pin &&
        g_modem_cache.baud_rate == baud_rate) {
        // Return cached instance, increment reference count
        g_modem_cache.ref_count++;
        return static_cast<ml307_modem_handle_t>(g_modem_cache.ctx);
    }
    
    // If there's an existing modem with different config, we can't create a new one
    // (UART resource conflict)
    if (g_modem_cache.ctx != nullptr) {
        // Different pin configuration - return existing one anyway with a warning
        // In practice, only one modem should exist
        g_modem_cache.ref_count++;
        return static_cast<ml307_modem_handle_t>(g_modem_cache.ctx);
    }
    
    gpio_num_t tx = static_cast<gpio_num_t>(tx_pin);
    gpio_num_t rx = static_cast<gpio_num_t>(rx_pin);
    gpio_num_t dtr = (dtr_pin >= 0) ? static_cast<gpio_num_t>(dtr_pin) : GPIO_NUM_NC;
    
    auto modem = AtModem::Detect(tx, rx, dtr, baud_rate, timeout_ms);
    if (!modem) {
        return nullptr;
    }
    
    auto* ctx = new (std::nothrow) ml307_modem_context();
    if (!ctx) {
        return nullptr;
    }
    ctx->modem = std::move(modem);
    
    // Cache the modem instance
    g_modem_cache.ctx = ctx;
    g_modem_cache.tx_pin = tx_pin;
    g_modem_cache.rx_pin = rx_pin;
    g_modem_cache.dtr_pin = dtr_pin;
    g_modem_cache.baud_rate = baud_rate;
    g_modem_cache.ref_count = 1;
    
    return static_cast<ml307_modem_handle_t>(ctx);
}

void ml307_destroy(ml307_modem_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    
    // Check if this is the cached modem
    if (ctx == g_modem_cache.ctx) {
        g_modem_cache.ref_count--;
        // Only actually destroy when ref_count reaches 0
        if (g_modem_cache.ref_count <= 0) {
            delete ctx;
            g_modem_cache.ctx = nullptr;
            g_modem_cache.tx_pin = -1;
            g_modem_cache.rx_pin = -1;
            g_modem_cache.dtr_pin = -1;
            g_modem_cache.baud_rate = 0;
            g_modem_cache.ref_count = 0;
        }
        return;
    }
    
    // Not cached (shouldn't happen), just delete
    delete ctx;
}

ml307_modem_handle_t ml307_get_cached(void) {
    return static_cast<ml307_modem_handle_t>(g_modem_cache.ctx);
}

bool ml307_is_initialized(void) {
    return g_modem_cache.ctx != nullptr;
}

void ml307_force_destroy(void) {
    if (g_modem_cache.ctx) {
        delete g_modem_cache.ctx;
        g_modem_cache.ctx = nullptr;
        g_modem_cache.tx_pin = -1;
        g_modem_cache.rx_pin = -1;
        g_modem_cache.dtr_pin = -1;
        g_modem_cache.baud_rate = 0;
        g_modem_cache.ref_count = 0;
    }
}

ml307_network_status_t ml307_wait_for_network_ready(ml307_modem_handle_t handle, int timeout_ms) {
    if (!handle) return ML307_STATUS_ERROR;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    NetworkStatus status = ctx->modem->WaitForNetworkReady(timeout_ms);
    
    switch (status) {
        case NetworkStatus::Ready: return ML307_STATUS_READY;
        case NetworkStatus::ErrorInsertPin: return ML307_STATUS_ERROR_INSERT_PIN;
        case NetworkStatus::ErrorRegistrationDenied: return ML307_STATUS_ERROR_REGISTRATION_DENIED;
        case NetworkStatus::ErrorTimeout: return ML307_STATUS_ERROR_TIMEOUT;
        default: return ML307_STATUS_ERROR;
    }
}

bool ml307_is_network_ready(ml307_modem_handle_t handle) {
    if (!handle) return false;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return ctx->modem->network_ready();
}

void ml307_reboot(ml307_modem_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    ctx->modem->Reboot();
}

int ml307_get_imei(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return copy_string_to_buffer(ctx->modem->GetImei(), buffer, buffer_size);
}

int ml307_get_iccid(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return copy_string_to_buffer(ctx->modem->GetIccid(), buffer, buffer_size);
}

int ml307_get_module_revision(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return copy_string_to_buffer(ctx->modem->GetModuleRevision(), buffer, buffer_size);
}

int ml307_get_carrier_name(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return copy_string_to_buffer(ctx->modem->GetCarrierName(), buffer, buffer_size);
}

int ml307_get_csq(ml307_modem_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return ctx->modem->GetCsq();
}

// ============================================================================
// HTTP API Implementation
// ============================================================================

ml307_http_handle_t ml307_http_create(ml307_modem_handle_t modem) {
    if (!modem) return nullptr;
    auto* modem_ctx = static_cast<ml307_modem_context*>(modem);
    
    auto* ctx = new (std::nothrow) ml307_http_context();
    if (!ctx) return nullptr;
    
    ctx->http = modem_ctx->modem->CreateHttp();
    if (!ctx->http) {
        delete ctx;
        return nullptr;
    }
    ctx->modem_ctx = modem_ctx;
    
    return static_cast<ml307_http_handle_t>(ctx);
}

void ml307_http_destroy(ml307_http_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    if (ctx->http) {
        ctx->http->Close();
    }
    delete ctx;
}

void ml307_http_set_timeout(ml307_http_handle_t handle, int timeout_ms) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    ctx->http->SetTimeout(timeout_ms);
}

void ml307_http_set_header(ml307_http_handle_t handle, const char* key, const char* value) {
    if (!handle || !key || !value) return;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    ctx->http->SetHeader(key, value);
}

void ml307_http_set_content(ml307_http_handle_t handle, const char* content, size_t len) {
    if (!handle || !content) return;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    ctx->content = std::string(content, len);
    ctx->http->SetContent(std::move(std::string(content, len)));
}

bool ml307_http_open(ml307_http_handle_t handle, const char* method, const char* url) {
    if (!handle || !method || !url) return false;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    return ctx->http->Open(method, url);
}

void ml307_http_close(ml307_http_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    ctx->http->Close();
}

int ml307_http_get_status_code(ml307_http_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    return ctx->http->GetStatusCode();
}

size_t ml307_http_get_body_length(ml307_http_handle_t handle) {
    if (!handle) return 0;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    return ctx->http->GetBodyLength();
}

int ml307_http_read(ml307_http_handle_t handle, char* buffer, size_t buffer_size) {
    if (!handle || !buffer) return -1;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    return ctx->http->Read(buffer, buffer_size);
}

int ml307_http_read_all(ml307_http_handle_t handle, char* buffer, size_t buffer_size) {
    if (!handle || !buffer) return -1;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    std::string body = ctx->http->ReadAll();
    return copy_string_to_buffer(body, buffer, static_cast<int>(buffer_size));
}

int ml307_http_get_last_error(ml307_http_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_http_context*>(handle);
    return ctx->http->GetLastError();
}

// ============================================================================
// WebSocket API Implementation
// ============================================================================

ml307_websocket_handle_t ml307_websocket_create(ml307_modem_handle_t modem) {
    if (!modem) return nullptr;
    auto* modem_ctx = static_cast<ml307_modem_context*>(modem);
    
    auto* ctx = new (std::nothrow) ml307_websocket_context();
    if (!ctx) return nullptr;
    
    ctx->ws = modem_ctx->modem->CreateWebSocket();
    if (!ctx->ws) {
        delete ctx;
        return nullptr;
    }
    ctx->modem_ctx = modem_ctx;
    ctx->on_data = nullptr;
    ctx->on_connected = nullptr;
    ctx->on_disconnected = nullptr;
    ctx->on_error = nullptr;
    ctx->user_data = nullptr;
    
    return static_cast<ml307_websocket_handle_t>(ctx);
}

void ml307_websocket_destroy(ml307_websocket_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    if (ctx->ws) {
        ctx->ws->Close();
    }
    delete ctx;
}

void ml307_websocket_set_header(ml307_websocket_handle_t handle, const char* key, const char* value) {
    if (!handle || !key || !value) return;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    ctx->ws->SetHeader(key, value);
}

bool ml307_websocket_connect(ml307_websocket_handle_t handle, const char* url) {
    if (!handle || !url) return false;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    return ctx->ws->Connect(url);
}

bool ml307_websocket_send(ml307_websocket_handle_t handle, const char* data, size_t len, bool binary) {
    if (!handle || !data) return false;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    return ctx->ws->Send(data, len, binary);
}

void ml307_websocket_ping(ml307_websocket_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    ctx->ws->Ping();
}

void ml307_websocket_close(ml307_websocket_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    ctx->ws->Close();
}

bool ml307_websocket_is_connected(ml307_websocket_handle_t handle) {
    if (!handle) return false;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    return ctx->ws->IsConnected();
}

void ml307_websocket_set_callbacks(ml307_websocket_handle_t handle,
                                   ml307_ws_data_callback_t on_data,
                                   ml307_ws_event_callback_t on_connected,
                                   ml307_ws_event_callback_t on_disconnected,
                                   ml307_ws_error_callback_t on_error,
                                   void* user_data) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    
    ctx->on_data = on_data;
    ctx->on_connected = on_connected;
    ctx->on_disconnected = on_disconnected;
    ctx->on_error = on_error;
    ctx->user_data = user_data;
    
    if (on_data) {
        ctx->ws->OnData([ctx](const char* data, size_t len, bool binary) {
            if (ctx->on_data) {
                ctx->on_data(ctx->user_data, data, len, binary);
            }
        });
    }
    
    if (on_connected) {
        ctx->ws->OnConnected([ctx]() {
            if (ctx->on_connected) {
                ctx->on_connected(ctx->user_data);
            }
        });
    }
    
    if (on_disconnected) {
        ctx->ws->OnDisconnected([ctx]() {
            if (ctx->on_disconnected) {
                ctx->on_disconnected(ctx->user_data);
            }
        });
    }
    
    if (on_error) {
        ctx->ws->OnError([ctx](int error) {
            if (ctx->on_error) {
                ctx->on_error(ctx->user_data, error);
            }
        });
    }
}

int ml307_websocket_get_last_error(ml307_websocket_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_websocket_context*>(handle);
    return ctx->ws->GetLastError();
}

// ============================================================================
// TCP API Implementation
// ============================================================================

ml307_tcp_handle_t ml307_tcp_create(ml307_modem_handle_t modem, bool use_ssl) {
    if (!modem) return nullptr;
    auto* modem_ctx = static_cast<ml307_modem_context*>(modem);
    
    auto* ctx = new (std::nothrow) ml307_tcp_context();
    if (!ctx) return nullptr;
    
    if (use_ssl) {
        ctx->tcp = modem_ctx->modem->CreateSsl();
    } else {
        ctx->tcp = modem_ctx->modem->CreateTcp();
    }
    
    if (!ctx->tcp) {
        delete ctx;
        return nullptr;
    }
    ctx->modem_ctx = modem_ctx;
    ctx->on_data = nullptr;
    ctx->on_disconnected = nullptr;
    ctx->user_data = nullptr;
    
    return static_cast<ml307_tcp_handle_t>(ctx);
}

void ml307_tcp_destroy(ml307_tcp_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    if (ctx->tcp) {
        ctx->tcp->Disconnect();
    }
    delete ctx;
}

bool ml307_tcp_connect(ml307_tcp_handle_t handle, const char* host, int port) {
    if (!handle || !host) return false;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    return ctx->tcp->Connect(host, port);
}

void ml307_tcp_disconnect(ml307_tcp_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    ctx->tcp->Disconnect();
}

int ml307_tcp_send(ml307_tcp_handle_t handle, const char* data, size_t len) {
    if (!handle || !data) return -1;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    return ctx->tcp->Send(std::string(data, len));
}

bool ml307_tcp_is_connected(ml307_tcp_handle_t handle) {
    if (!handle) return false;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    return ctx->tcp->connected();
}

void ml307_tcp_set_callbacks(ml307_tcp_handle_t handle,
                             ml307_tcp_data_callback_t on_data,
                             ml307_tcp_event_callback_t on_disconnected,
                             void* user_data) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    
    ctx->on_data = on_data;
    ctx->on_disconnected = on_disconnected;
    ctx->user_data = user_data;
    
    if (on_data) {
        ctx->tcp->OnStream([ctx](const std::string& data) {
            if (ctx->on_data) {
                ctx->on_data(ctx->user_data, data.c_str(), data.length());
            }
        });
    }
    
    if (on_disconnected) {
        ctx->tcp->OnDisconnected([ctx]() {
            if (ctx->on_disconnected) {
                ctx->on_disconnected(ctx->user_data);
            }
        });
    }
}

int ml307_tcp_get_last_error(ml307_tcp_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_tcp_context*>(handle);
    return ctx->tcp->GetLastError();
}

// ============================================================================
// UDP API Implementation
// ============================================================================

ml307_udp_handle_t ml307_udp_create(ml307_modem_handle_t modem) {
    if (!modem) return nullptr;
    auto* modem_ctx = static_cast<ml307_modem_context*>(modem);
    
    auto* ctx = new (std::nothrow) ml307_udp_context();
    if (!ctx) return nullptr;
    
    ctx->udp = modem_ctx->modem->CreateUdp();
    if (!ctx->udp) {
        delete ctx;
        return nullptr;
    }
    ctx->modem_ctx = modem_ctx;
    ctx->on_message = nullptr;
    ctx->user_data = nullptr;
    
    return static_cast<ml307_udp_handle_t>(ctx);
}

void ml307_udp_destroy(ml307_udp_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    if (ctx->udp) {
        ctx->udp->Disconnect();
    }
    delete ctx;
}

bool ml307_udp_connect(ml307_udp_handle_t handle, const char* host, int port) {
    if (!handle || !host) return false;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    return ctx->udp->Connect(host, port);
}

void ml307_udp_disconnect(ml307_udp_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    ctx->udp->Disconnect();
}

int ml307_udp_send(ml307_udp_handle_t handle, const char* data, size_t len) {
    if (!handle || !data) return -1;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    return ctx->udp->Send(std::string(data, len));
}

bool ml307_udp_is_connected(ml307_udp_handle_t handle) {
    if (!handle) return false;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    return ctx->udp->connected();
}

void ml307_udp_set_callback(ml307_udp_handle_t handle,
                            ml307_udp_data_callback_t on_message,
                            void* user_data) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    
    ctx->on_message = on_message;
    ctx->user_data = user_data;
    
    if (on_message) {
        ctx->udp->OnMessage([ctx](const std::string& data) {
            if (ctx->on_message) {
                ctx->on_message(ctx->user_data, data.c_str(), data.length());
            }
        });
    }
}

int ml307_udp_get_last_error(ml307_udp_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_udp_context*>(handle);
    return ctx->udp->GetLastError();
}

// ============================================================================
// MQTT API Implementation
// ============================================================================

ml307_mqtt_handle_t ml307_mqtt_create(ml307_modem_handle_t modem) {
    if (!modem) return nullptr;
    auto* modem_ctx = static_cast<ml307_modem_context*>(modem);
    
    auto* ctx = new (std::nothrow) ml307_mqtt_context();
    if (!ctx) return nullptr;
    
    ctx->mqtt = modem_ctx->modem->CreateMqtt();
    if (!ctx->mqtt) {
        delete ctx;
        return nullptr;
    }
    ctx->modem_ctx = modem_ctx;
    ctx->on_message = nullptr;
    ctx->on_connected = nullptr;
    ctx->on_disconnected = nullptr;
    ctx->on_error = nullptr;
    ctx->user_data = nullptr;
    
    return static_cast<ml307_mqtt_handle_t>(ctx);
}

void ml307_mqtt_destroy(ml307_mqtt_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    if (ctx->mqtt) {
        ctx->mqtt->Disconnect();
    }
    delete ctx;
}

void ml307_mqtt_set_keepalive(ml307_mqtt_handle_t handle, int seconds) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    ctx->mqtt->SetKeepAlive(seconds);
}

bool ml307_mqtt_connect(ml307_mqtt_handle_t handle, const char* broker, int port,
                        const char* client_id, const char* username, const char* password) {
    if (!handle || !broker || !client_id) return false;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->Connect(broker, port, client_id, 
                              username ? username : "", 
                              password ? password : "");
}

void ml307_mqtt_disconnect(ml307_mqtt_handle_t handle) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    ctx->mqtt->Disconnect();
}

bool ml307_mqtt_publish(ml307_mqtt_handle_t handle, const char* topic, const char* payload, int qos) {
    if (!handle || !topic || !payload) return false;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->Publish(topic, payload, qos);
}

bool ml307_mqtt_subscribe(ml307_mqtt_handle_t handle, const char* topic, int qos) {
    if (!handle || !topic) return false;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->Subscribe(topic, qos);
}

bool ml307_mqtt_unsubscribe(ml307_mqtt_handle_t handle, const char* topic) {
    if (!handle || !topic) return false;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->Unsubscribe(topic);
}

bool ml307_mqtt_is_connected(ml307_mqtt_handle_t handle) {
    if (!handle) return false;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->IsConnected();
}

void ml307_mqtt_set_callbacks(ml307_mqtt_handle_t handle,
                              ml307_mqtt_message_callback_t on_message,
                              ml307_mqtt_event_callback_t on_connected,
                              ml307_mqtt_event_callback_t on_disconnected,
                              ml307_mqtt_error_callback_t on_error,
                              void* user_data) {
    if (!handle) return;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    
    ctx->on_message = on_message;
    ctx->on_connected = on_connected;
    ctx->on_disconnected = on_disconnected;
    ctx->on_error = on_error;
    ctx->user_data = user_data;
    
    if (on_message) {
        ctx->mqtt->OnMessage([ctx](const std::string& topic, const std::string& payload) {
            if (ctx->on_message) {
                ctx->on_message(ctx->user_data, topic.c_str(), topic.length(),
                               payload.c_str(), payload.length());
            }
        });
    }
    
    if (on_connected) {
        ctx->mqtt->OnConnected([ctx]() {
            if (ctx->on_connected) {
                ctx->on_connected(ctx->user_data);
            }
        });
    }
    
    if (on_disconnected) {
        ctx->mqtt->OnDisconnected([ctx]() {
            if (ctx->on_disconnected) {
                ctx->on_disconnected(ctx->user_data);
            }
        });
    }
    
    if (on_error) {
        ctx->mqtt->OnError([ctx](const std::string& error) {
            if (ctx->on_error) {
                ctx->on_error(ctx->user_data, error.c_str());
            }
        });
    }
}

int ml307_mqtt_get_last_error(ml307_mqtt_handle_t handle) {
    if (!handle) return -1;
    auto* ctx = static_cast<ml307_mqtt_context*>(handle);
    return ctx->mqtt->GetLastError();
}
