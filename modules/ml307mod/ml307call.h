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
 * @brief Opaque handle to the modem instance
 */
typedef void* ml307_modem_handle_t;

/**
 * @brief Initialize and detect the modem
 * 
 * @param tx_pin UART TX pin number
 * @param rx_pin UART RX pin number
 * @param dtr_pin DTR pin number (use -1 for not connected)
 * @param baud_rate UART baud rate (default: 115200)
 * @param timeout_ms Detection timeout in ms (use -1 for default)
 * @return Modem handle, or NULL on failure
 */
ml307_modem_handle_t ml307_detect(int tx_pin, int rx_pin, int dtr_pin, int baud_rate, int timeout_ms);

/**
 * @brief Destroy the modem instance and free resources
 * 
 * @param handle Modem handle
 */
void ml307_destroy(ml307_modem_handle_t handle);

/**
 * @brief Wait for network to become ready
 * 
 * @param handle Modem handle
 * @param timeout_ms Timeout in milliseconds (-1 for default)
 * @return Network status code
 */
ml307_network_status_t ml307_wait_for_network_ready(ml307_modem_handle_t handle, int timeout_ms);

/**
 * @brief Check if network is ready
 * 
 * @param handle Modem handle
 * @return true if network is ready
 */
bool ml307_is_network_ready(ml307_modem_handle_t handle);

/**
 * @brief Reboot the modem
 * 
 * @param handle Modem handle
 */
void ml307_reboot(ml307_modem_handle_t handle);

/**
 * @brief Get modem IMEI
 * 
 * @param handle Modem handle
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Length of IMEI string, or -1 on error
 */
int ml307_get_imei(ml307_modem_handle_t handle, char* buffer, int buffer_size);

/**
 * @brief Get SIM card ICCID
 * 
 * @param handle Modem handle
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Length of ICCID string, or -1 on error
 */
int ml307_get_iccid(ml307_modem_handle_t handle, char* buffer, int buffer_size);

/**
 * @brief Get modem firmware revision
 * 
 * @param handle Modem handle
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Length of revision string, or -1 on error
 */
int ml307_get_module_revision(ml307_modem_handle_t handle, char* buffer, int buffer_size);

/**
 * @brief Get carrier name
 * 
 * @param handle Modem handle
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Length of carrier name string, or -1 on error
 */
int ml307_get_carrier_name(ml307_modem_handle_t handle, char* buffer, int buffer_size);

/**
 * @brief Get signal strength (CSQ value)
 * 
 * @param handle Modem handle
 * @return CSQ value (0-31), or -1 on error
 */
int ml307_get_csq(ml307_modem_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // ML307CALL_H

