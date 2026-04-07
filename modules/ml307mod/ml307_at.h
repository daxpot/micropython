/*
 * ML307R AT Command Engine
 * UART communication via UHCI DMA (ESP32-S3/C3/C6/P4) or standard UART driver.
 * AT response parsing, URC dispatch, socket management.
 * 
 * When ML307_USE_UHCI_DMA=1:
 *   UHCI DMA → ISR callback → queue → task → line_buf → parse
 */

#ifndef ML307_AT_H
#define ML307_AT_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits */
#define ML307_MAX_SOCKETS       6
#define ML307_SOCK_RXBUF_SIZE   8192    /* Ring buffer per socket */
#define ML307_LINE_BUF_SIZE     4096    /* Max AT line length (MIPURC can be long) */
#define ML307_RESP_BUF_SIZE     1024    /* AT response accumulator */
#define ML307_MAX_SEND_CHUNK    730     /* Max bytes per MIPSEND (1460/2 for HEX) */

/* Standard UART driver buffer sizes (used when UHCI not available) */
#define ML307_UART_RXBUF_SIZE   16384
#define ML307_UART_TXBUF_SIZE   2048

/* UHCI DMA buffer pool configuration */
#ifdef ML307_USE_UHCI_DMA
#define ML307_DMA_BUF_COUNT     12      /* Number of DMA buffers */
#define ML307_DMA_BUF_SIZE      512     /* Size of each DMA buffer */
#define ML307_RX_QUEUE_DEPTH    16      /* RX data queue depth */
#endif

/* Socket states */
#define ML307_SOCK_FREE         0
#define ML307_SOCK_ALLOCATED    1
#define ML307_SOCK_CONNECTING   2
#define ML307_SOCK_CONNECTED    3

/* Socket event bits */
#define SOCK_EVT_CONNECTED      (1 << 0)
#define SOCK_EVT_DISCONN        (1 << 1)
#define SOCK_EVT_ERROR          (1 << 2)
#define SOCK_EVT_DATA           (1 << 3)
#define SOCK_EVT_SEND_DONE      (1 << 4)

/* AT engine event bits */
#define AT_EVT_RESP_READY       (1 << 0)
#define AT_EVT_RESP_TRUNCATED   (1 << 1)  /* Response buffer was full */

/* Per-socket state */
typedef struct {
    int state;
    uint8_t *rx_buf;                /* Ring buffer */
    int rx_head;                    /* Write pointer */
    int rx_tail;                    /* Read pointer */
    bool disconnected;
    bool overflow;                  /* Set when ring buffer overflow occurred */
    int open_result;                /* MIPOPEN result: -1=pending, 0=ok, >0=error */
    EventGroupHandle_t event;
    SemaphoreHandle_t mutex;        /* Protect ring buffer */
} ml307_sock_t;

/* Main driver state */
typedef struct {
    /* UART */
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int baudrate;
    bool debug;

#ifdef ML307_USE_UHCI_DMA
    /* UHCI DMA controller (C++ object, managed as opaque pointer) */
    void *uhci;                     /* UartUhci* */
    
    /* RX data queue (DMA callback → task) */
    QueueHandle_t rx_queue;
    
    /* DMA overflow flag (set by overflow callback) */
    volatile bool dma_overflow;
#endif

    /* Background task */
    TaskHandle_t task_handle;
    volatile bool task_running;

    /* Line parser */
    char line_buf[ML307_LINE_BUF_SIZE];
    int line_len;

    /* AT command synchronization */
    SemaphoreHandle_t at_mutex;         /* One AT command at a time */
    EventGroupHandle_t at_event;
    char resp_buf[ML307_RESP_BUF_SIZE]; /* Response accumulator */
    int resp_len;
    volatile bool at_waiting;           /* True when waiting for AT response */
    volatile bool resp_truncated;       /* True if response buffer overflowed */
    int last_cme_error;                 /* Last +CME ERROR code (-1 = none) */
    char at_cmd_echo[64];              /* Last sent command (for echo skip) */

    /* Sockets */
    ml307_sock_t sock[ML307_MAX_SOCKETS];

    /* Network info */
    char ip[64];
    int csq;
    bool registered;
    bool initialized;
} ml307_state_t;

/* ---- Logging callback ---- */

/* Log function type — called from init path to print to MicroPython console.
 * Set via ml307_set_log_fn() before calling ml307_init(). */
typedef void (*ml307_log_fn_t)(const char *fmt, ...);

/* Set the logging callback. Pass NULL to disable logging. */
void ml307_set_log_fn(ml307_log_fn_t fn);

/* ---- Public API ---- */

/* Initialize UART, start background task, init modem.
 * Returns 0 on success, negative on error. */
int ml307_init(ml307_state_t *s, int tx_pin, int rx_pin, int baudrate,
               const char *apn, bool debug);

/* Deinitialize: stop task, close UART. */
void ml307_deinit(ml307_state_t *s);

/* Send raw AT command, wait for response.
 * resp: output buffer (caller-owned), resp_size: buffer size.
 * Returns 0=OK, -1=timeout, -2=ERROR response. */
int ml307_send_at(ml307_state_t *s, const char *cmd,
                  char *resp, int resp_size, int timeout_ms);

/* Allocate a free socket slot. Returns socket ID (0-5) or -1 if none free. */
int ml307_sock_alloc(ml307_state_t *s);

/* Free a socket slot. */
void ml307_sock_free(ml307_state_t *s, int sid);

/* Connect socket to host:port. ssl=true enables modem TLS.
 * Returns 0 on success, negative on error. */
int ml307_sock_connect(ml307_state_t *s, int sid, const char *host, int port,
                       bool ssl, int timeout_ms);

/* Send data. Returns bytes sent, or negative on error. */
int ml307_sock_send(ml307_state_t *s, int sid, const uint8_t *data, int len);

/* Receive data. Copies up to maxlen bytes into buf.
 * timeout_ms: -1=block forever, 0=non-blocking, >0=timeout.
 * Returns bytes read, 0 on disconnect/timeout, -1 on error. */
int ml307_sock_recv(ml307_state_t *s, int sid, uint8_t *buf, int maxlen,
                    int timeout_ms);

/* Bytes available in receive buffer. */
int ml307_sock_available(ml307_state_t *s, int sid);

/* Check if peer disconnected. */
bool ml307_sock_is_disconnected(ml307_state_t *s, int sid);

/* Check if socket had buffer overflow (and clear the flag). */
bool ml307_sock_check_overflow(ml307_state_t *s, int sid);

/* Close socket. */
void ml307_sock_close(ml307_state_t *s, int sid);

#ifdef ML307_USE_UHCI_DMA
/* Check if DMA had overflow (and clear the flag). */
bool ml307_check_dma_overflow(ml307_state_t *s);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ML307_AT_H */
