/**
 * @file ml307call.cpp
 * @brief C wrapper implementation for esp-ml307 C++ library
 * 
 * This file provides C-compatible wrapper functions around the esp-ml307 
 * C++ classes for use by MicroPython bindings.
 */

#include "ml307call.h"
#include "at_modem.h"
#include <driver/gpio.h>
#include <memory>
#include <cstring>

/**
 * @brief Internal structure to hold the modem instance
 */
struct ml307_modem_context {
    std::unique_ptr<AtModem> modem;
};

ml307_modem_handle_t ml307_detect(int tx_pin, int rx_pin, int dtr_pin, int baud_rate, int timeout_ms) {
    // Validate parameters
    if (tx_pin < 0 || rx_pin < 0) {
        return nullptr;
    }
    
    // Convert pin numbers to gpio_num_t
    gpio_num_t tx = static_cast<gpio_num_t>(tx_pin);
    gpio_num_t rx = static_cast<gpio_num_t>(rx_pin);
    gpio_num_t dtr = (dtr_pin >= 0) ? static_cast<gpio_num_t>(dtr_pin) : GPIO_NUM_NC;
    
    // Use default baud rate if not specified
    if (baud_rate <= 0) {
        baud_rate = 115200;
    }
    
    // Detect and create modem instance
    auto modem = AtModem::Detect(tx, rx, dtr, baud_rate, timeout_ms);
    if (!modem) {
        return nullptr;
    }
    
    // Create context and move modem ownership
    auto* ctx = new (std::nothrow) ml307_modem_context();
    if (!ctx) {
        return nullptr;
    }
    ctx->modem = std::move(modem);
    
    return static_cast<ml307_modem_handle_t>(ctx);
}

void ml307_destroy(ml307_modem_handle_t handle) {
    if (!handle) {
        return;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    delete ctx;
}

ml307_network_status_t ml307_wait_for_network_ready(ml307_modem_handle_t handle, int timeout_ms) {
    if (!handle) {
        return ML307_STATUS_ERROR;
    }
    
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    NetworkStatus status = ctx->modem->WaitForNetworkReady(timeout_ms);
    
    // Convert C++ enum to C enum
    switch (status) {
        case NetworkStatus::Ready:
            return ML307_STATUS_READY;
        case NetworkStatus::ErrorInsertPin:
            return ML307_STATUS_ERROR_INSERT_PIN;
        case NetworkStatus::ErrorRegistrationDenied:
            return ML307_STATUS_ERROR_REGISTRATION_DENIED;
        case NetworkStatus::ErrorTimeout:
            return ML307_STATUS_ERROR_TIMEOUT;
        case NetworkStatus::Error:
        default:
            return ML307_STATUS_ERROR;
    }
}

bool ml307_is_network_ready(ml307_modem_handle_t handle) {
    if (!handle) {
        return false;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return ctx->modem->network_ready();
}

void ml307_reboot(ml307_modem_handle_t handle) {
    if (!handle) {
        return;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    ctx->modem->Reboot();
}

/**
 * @brief Helper function to copy C++ string to C buffer
 */
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

int ml307_get_imei(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) {
        return -1;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    std::string imei = ctx->modem->GetImei();
    return copy_string_to_buffer(imei, buffer, buffer_size);
}

int ml307_get_iccid(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) {
        return -1;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    std::string iccid = ctx->modem->GetIccid();
    return copy_string_to_buffer(iccid, buffer, buffer_size);
}

int ml307_get_module_revision(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) {
        return -1;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    std::string revision = ctx->modem->GetModuleRevision();
    return copy_string_to_buffer(revision, buffer, buffer_size);
}

int ml307_get_carrier_name(ml307_modem_handle_t handle, char* buffer, int buffer_size) {
    if (!handle) {
        return -1;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    std::string carrier = ctx->modem->GetCarrierName();
    return copy_string_to_buffer(carrier, buffer, buffer_size);
}

int ml307_get_csq(ml307_modem_handle_t handle) {
    if (!handle) {
        return -1;
    }
    auto* ctx = static_cast<ml307_modem_context*>(handle);
    return ctx->modem->GetCsq();
}
