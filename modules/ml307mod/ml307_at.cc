/*
 * ML307R AT Command Engine Implementation
 *
 * When ML307_USE_UHCI_DMA=1:
 *   UHCI DMA receives data → ISR callback → FreeRTOS queue → background task
 * Otherwise:
 *   Standard UART driver with uart_read_bytes polling
 * 
 * Socket data arrives via +MIPURC, is HEX-decoded, and stored in ring buffers.
 * Python thread synchronizes via EventGroups (no polling needed).
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ml307_at.h"
#include "ml307_hex.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#ifdef ML307_USE_UHCI_DMA
#include "uart_uhci.h"
#endif

static const char *TAG = "ML307";

/* ---- Logging callback (outputs to MicroPython console) ---- */
static ml307_log_fn_t s_log_fn = NULL;

void ml307_set_log_fn(ml307_log_fn_t fn) {
    s_log_fn = fn;
}

/* Log macro: uses callback if set, otherwise falls back to printf. */
#define ML307_LOGF(fmt, ...) do { \
    if (s_log_fn) { s_log_fn(fmt, ##__VA_ARGS__); } \
    else { printf(fmt, ##__VA_ARGS__); } \
} while(0)

/* ---- Ring Buffer Helpers ---- */

static inline int ringbuf_available(ml307_sock_t *sk) {
    int avail = sk->rx_head - sk->rx_tail;
    if (avail < 0) avail += ML307_SOCK_RXBUF_SIZE;
    return avail;
}

static void ringbuf_write(ml307_sock_t *sk, const uint8_t *data, int len) {
    /* Check if there's enough space; if not, discard entire frame to avoid
     * partial/corrupt data (borrowed from esp-ml307's approach) */
    int free_space = ML307_SOCK_RXBUF_SIZE - 1 - ringbuf_available(sk);
    if (len > free_space) {
        sk->overflow = true;  /* Signal overflow to upper layer */
        return;  /* Discard entire frame rather than corrupt partial data */
    }
    for (int i = 0; i < len; i++) {
        int next = (sk->rx_head + 1) % ML307_SOCK_RXBUF_SIZE;
        sk->rx_buf[sk->rx_head] = data[i];
        sk->rx_head = next;
    }
}

static int ringbuf_read(ml307_sock_t *sk, uint8_t *buf, int maxlen) {
    int avail = ringbuf_available(sk);
    if (avail == 0) return 0;
    int n = (avail < maxlen) ? avail : maxlen;
    for (int i = 0; i < n; i++) {
        buf[i] = sk->rx_buf[sk->rx_tail];
        sk->rx_tail = (sk->rx_tail + 1) % ML307_SOCK_RXBUF_SIZE;
    }
    return n;
}

/* ---- URC Handlers ---- */

/* +MIPURC: "rtcp",<id>,<len>,<hex_data>
 * +MIPURC: "rudp",<id>,<len>,<hex_data>
 * +MIPURC: "disconn",<id> */
static void handle_mipurc(ml307_state_t *s, const char *args) {
    const char *p = args;
    while (*p == ' ') p++;

    /* Parse type string */
    if (*p != '"') return;
    p++;
    char type[16];
    int ti = 0;
    while (*p && *p != '"' && ti < (int)sizeof(type) - 1) {
        type[ti++] = *p++;
    }
    type[ti] = '\0';
    if (*p == '"') p++;
    if (*p == ',') p++;

    /* Parse socket ID */
    int sid = atoi(p);
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return;

    if (strcmp(type, "rtcp") == 0 || strcmp(type, "rudp") == 0) {
        /* Skip to data_len field */
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        /* int data_len = atoi(p); — we derive actual len from hex string */

        /* Skip to hex data */
        while (*p && *p != ',') p++;
        if (*p == ',') p++;

        /* Decode hex data directly into ring buffer */
        /* Compute safe hex_len bounded by line_buf end to prevent overread */
        const char *line_end = s->line_buf + ML307_LINE_BUF_SIZE;
        int hex_len = 0;
        const char *hp = p;
        while (hp < line_end && *hp && *hp != '\r' && *hp != '\n') {
            hp++;
            hex_len++;
        }
        if (hex_len < 2) return;

        /* Temporary decode buffer on stack (max ~730 bytes per URC) */
        uint8_t decode_buf[1024];
        int decode_len = hex_len / 2;
        if (decode_len > (int)sizeof(decode_buf)) {
            decode_len = sizeof(decode_buf);
        }
        ml307_hex_decode(p, decode_len * 2, decode_buf);

        xSemaphoreTake(s->sock[sid].mutex, portMAX_DELAY);
        ringbuf_write(&s->sock[sid], decode_buf, decode_len);
        xSemaphoreGive(s->sock[sid].mutex);

        xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_DATA);

        if (s->debug) {
            ESP_LOGI(TAG, "URC rtcp sid=%d len=%d", sid, decode_len);
        }
    } else if (strcmp(type, "disconn") == 0) {
        s->sock[sid].disconnected = true;
        xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_DISCONN | SOCK_EVT_DATA);
        if (s->debug) {
            ESP_LOGI(TAG, "URC disconn sid=%d", sid);
        }
    }
}

/* +MIPOPEN: <id>,<result>  (0=success) */
static void handle_mipopen(ml307_state_t *s, const char *args) {
    const char *p = args;
    while (*p == ' ') p++;
    int sid = atoi(p);
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
    int result = atoi(p);

    if (sid >= 0 && sid < ML307_MAX_SOCKETS) {
        s->sock[sid].open_result = result;
        if (result == 0) {
            s->sock[sid].state = ML307_SOCK_CONNECTED;
            xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_CONNECTED);
        } else {
            xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_ERROR);
        }
        if (s->debug) {
            ESP_LOGI(TAG, "MIPOPEN sid=%d result=%d", sid, result);
        }
    }
}

/* +MIPCLOSE: <id> */
static void handle_mipclose(ml307_state_t *s, const char *args) {
    const char *p = args;
    while (*p == ' ') p++;
    int sid = atoi(p);
    if (sid >= 0 && sid < ML307_MAX_SOCKETS) {
        s->sock[sid].state = ML307_SOCK_FREE;
        xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_DISCONN);
    }
}

/* +MIPSEND: <id>,<result> */
static void handle_mipsend(ml307_state_t *s, const char *args) {
    const char *p = args;
    while (*p == ' ') p++;
    int sid = atoi(p);
    if (sid >= 0 && sid < ML307_MAX_SOCKETS) {
        xEventGroupSetBits(s->sock[sid].event, SOCK_EVT_SEND_DONE);
    }
}

/* ---- Line Processing ---- */

static bool is_urc(const char *line) {
    return (strncmp(line, "+MIPURC:", 8) == 0 ||
            strncmp(line, "+MIPOPEN:", 9) == 0 ||
            strncmp(line, "+MIPCLOSE:", 10) == 0 ||
            strncmp(line, "+MIPSEND:", 9) == 0);
}

static void dispatch_urc(ml307_state_t *s, const char *line) {
    if (strncmp(line, "+MIPURC:", 8) == 0) {
        handle_mipurc(s, line + 8);
    } else if (strncmp(line, "+MIPOPEN:", 9) == 0) {
        handle_mipopen(s, line + 9);
    } else if (strncmp(line, "+MIPCLOSE:", 10) == 0) {
        handle_mipclose(s, line + 10);
    } else if (strncmp(line, "+MIPSEND:", 9) == 0) {
        handle_mipsend(s, line + 9);
    }
}

static void process_line(ml307_state_t *s, const char *line) {
    /* Skip empty lines */
    if (line[0] == '\0') return;

    if (s->debug) {
        ESP_LOGI(TAG, "< %.*s", 120, line);
    }

    /* URC: dispatch immediately */
    if (is_urc(line)) {
        dispatch_urc(s, line);
        return;
    }

    /* If waiting for AT response */
    if (s->at_waiting) {
        /* Skip echo of sent command */
        if (s->at_cmd_echo[0] && strcmp(line, s->at_cmd_echo) == 0) {
            return;
        }

        /* Append line to response buffer */
        int line_len = strlen(line);
        if (s->resp_len + line_len + 2 < ML307_RESP_BUF_SIZE) {
            if (s->resp_len > 0) {
                s->resp_buf[s->resp_len++] = '\r';
                s->resp_buf[s->resp_len++] = '\n';
            }
            memcpy(s->resp_buf + s->resp_len, line, line_len);
            s->resp_len += line_len;
            s->resp_buf[s->resp_len] = '\0';
        } else {
            /* Response buffer full — mark truncation */
            s->resp_truncated = true;
        }

        /* Check for end-of-response markers */
        if (strcmp(line, "OK") == 0 ||
            strcmp(line, "ERROR") == 0 ||
            strncmp(line, "+CME ERROR:", 11) == 0 ||
            strncmp(line, "+CMS ERROR:", 11) == 0) {
            /* Parse CME/CMS error code for callers */
            if (strncmp(line, "+CME ERROR:", 11) == 0) {
                s->last_cme_error = atoi(line + 11);
            } else if (strncmp(line, "+CMS ERROR:", 11) == 0) {
                s->last_cme_error = atoi(line + 11);
            } else {
                s->last_cme_error = -1;
            }
            xEventGroupSetBits(s->at_event, AT_EVT_RESP_READY);
        }
    }
}

/* Process a byte from UART, accumulate lines */
static void process_byte(ml307_state_t *s, uint8_t ch) {
    if (ch == '\r') {
        /* Ignore CR, wait for LF */
        return;
    }
    if (ch == '\n') {
        /* End of line */
        if (s->line_len > 0) {
            s->line_buf[s->line_len] = '\0';
            process_line(s, s->line_buf);
            s->line_len = 0;
        }
        return;
    }
    /* Append character */
    if (s->line_len < ML307_LINE_BUF_SIZE - 1) {
        s->line_buf[s->line_len++] = (char)ch;
    }
    /* If buffer full, process what we have and reset */
    if (s->line_len >= ML307_LINE_BUF_SIZE - 1) {
        s->line_buf[s->line_len] = '\0';
        process_line(s, s->line_buf);
        s->line_len = 0;
    }
}

/* ---- Background Task ---- */

#ifdef ML307_USE_UHCI_DMA

/* RX data item for queue */
struct ml307_rx_item_t {
    uint8_t *data;
    size_t size;
    UartUhci::RxBuffer *buffer;  /* For returning to DMA pool */
};

/* DMA RX callback (ISR context - must be fast!) */
static bool IRAM_ATTR ml307_dma_rx_callback(const UartUhci::RxEventData& data, void* user_data) {
    ml307_state_t *s = static_cast<ml307_state_t*>(user_data);
    
    ml307_rx_item_t item = {
        .data = data.buffer->data,
        .size = data.recv_size,
        .buffer = data.buffer,
    };
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(s->rx_queue, &item, &xHigherPriorityTaskWoken) != pdTRUE) {
        /* Queue full - return buffer immediately to avoid DMA stall */
        static_cast<UartUhci*>(s->uhci)->ReturnBuffer(data.buffer);
    }
    
    return xHigherPriorityTaskWoken == pdTRUE;
}

/* DMA overflow callback (ISR context) */
static bool IRAM_ATTR ml307_dma_overflow_callback(void* user_data) {
    ml307_state_t *s = static_cast<ml307_state_t*>(user_data);
    s->dma_overflow = true;
    return false;
}

/* Background task for UHCI DMA mode */
static void ml307_at_task(void *arg) {
    ml307_state_t *s = static_cast<ml307_state_t*>(arg);
    ml307_rx_item_t item;
    UartUhci *uhci = static_cast<UartUhci*>(s->uhci);
    
    ML307_LOGF("[ML307] DMA task started, uhci=%p, queue=%p\n", uhci, s->rx_queue);
    
    static int debug_count = 0;
    while (s->task_running) {
        /* Fix 6: Check DMA overflow atomically and notify all connected sockets */
        bool had_overflow = __atomic_exchange_n(&s->dma_overflow, false, __ATOMIC_SEQ_CST);
        if (had_overflow) {
            ESP_LOGW(TAG, "DMA overflow detected — disconnecting active sockets");
            for (int i = 0; i < ML307_MAX_SOCKETS; i++) {
                if (s->sock[i].state == ML307_SOCK_CONNECTED) {
                    s->sock[i].disconnected = true;
                    s->sock[i].overflow = true;
                    xEventGroupSetBits(s->sock[i].event,
                        SOCK_EVT_DISCONN | SOCK_EVT_DATA | SOCK_EVT_ERROR);
                }
            }
        }

        /* Wait for data from DMA queue */
        if (xQueueReceive(s->rx_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
            /* Debug: log first few receives */
            if (debug_count < 5) {
                ML307_LOGF("[ML307] DMA RX: %d bytes\n", (int)item.size);
                debug_count++;
            }
            
            /* Process received bytes */
            for (size_t i = 0; i < item.size; i++) {
                process_byte(s, item.data[i]);
            }
            
            /* Return buffer to DMA pool immediately */
            uhci->ReturnBuffer(item.buffer);
        }
    }
    ML307_LOGF("[ML307] DMA task exiting\n");
    vTaskDelete(NULL);
}

#else /* Standard UART mode */

static void ml307_at_task(void *arg) {
    ml307_state_t *s = (ml307_state_t *)arg;
    uint8_t chunk[512];

    while (s->task_running) {
        int len = uart_read_bytes(s->uart_num, chunk, sizeof(chunk), 1);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                process_byte(s, chunk[i]);
            }
            continue;
        }
        taskYIELD();
    }
    vTaskDelete(NULL);
}

#endif /* ML307_USE_UHCI_DMA */

/* ---- UART Transmit Helper ---- */

static int ml307_uart_write(ml307_state_t *s, const void *data, size_t len) {
#ifdef ML307_USE_UHCI_DMA
    UartUhci *uhci = static_cast<UartUhci*>(s->uhci);
    if (uhci) {
        esp_err_t err = uhci->Transmit(static_cast<const uint8_t*>(data), len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UHCI Transmit failed: %d", err);
            return -1;
        }
        return 0;
    }
    return -1;
#else
    int written = uart_write_bytes(s->uart_num, data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGE(TAG, "uart_write_bytes failed: wrote %d/%d", written, (int)len);
        return -1;
    }
    return 0;
#endif
}

/* ---- AT Command Send ---- */

int ml307_send_at(ml307_state_t *s, const char *cmd,
                  char *resp, int resp_size, int timeout_ms) {
    xSemaphoreTake(s->at_mutex, portMAX_DELAY);

    /* Prepare response state */
    s->resp_len = 0;
    s->resp_buf[0] = '\0';
    s->resp_truncated = false;
    s->last_cme_error = -1;
    xEventGroupClearBits(s->at_event, AT_EVT_RESP_READY);

    /* Store command for echo detection */
    strncpy(s->at_cmd_echo, cmd, sizeof(s->at_cmd_echo) - 1);
    s->at_cmd_echo[sizeof(s->at_cmd_echo) - 1] = '\0';
    s->at_waiting = true;

    /* Send command + CRLF */
    if (ml307_uart_write(s, cmd, strlen(cmd)) < 0 ||
        ml307_uart_write(s, "\r\n", 2) < 0) {
        s->at_waiting = false;
        s->at_cmd_echo[0] = '\0';
        xSemaphoreGive(s->at_mutex);
        return -3; /* UART write error */
    }

    /* Wait for response */
    EventBits_t bits = xEventGroupWaitBits(
        s->at_event, AT_EVT_RESP_READY,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    s->at_waiting = false;
    s->at_cmd_echo[0] = '\0';

    int result;
    if (!(bits & AT_EVT_RESP_READY)) {
        result = -1; /* Timeout */
        if (resp && resp_size > 0) {
            int len = s->resp_len;
            if (len >= resp_size) len = resp_size - 1;
            memcpy(resp, s->resp_buf, len);
            resp[len] = '\0';
        }
    } else {
        int len = s->resp_len;
        if (resp && resp_size > 0) {
            if (len >= resp_size) len = resp_size - 1;
            memcpy(resp, s->resp_buf, len);
            resp[len] = '\0';
        }
        if (s->resp_truncated) {
            ESP_LOGW(TAG, "AT response truncated (>%d bytes)", ML307_RESP_BUF_SIZE);
        }
        result = (strstr(s->resp_buf, "OK") != NULL) ? 0 : -2;
    }

    xSemaphoreGive(s->at_mutex);
    return result;
}

/* Convenience: send AT, don't need response */
static int send_at_ok(ml307_state_t *s, const char *cmd, int timeout_ms) {
    char resp[256];
    return ml307_send_at(s, cmd, resp, sizeof(resp), timeout_ms);
}

/* ---- Modem Initialization ---- */

/* Try to communicate at given baudrate. Returns true if AT->OK works. */
static bool try_baudrate(ml307_state_t *s, int baud) {
    uart_set_baudrate(s->uart_num, baud);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Let UART settle after baudrate change. We do NOT call uart_flush_input()
     * because the background AT task holds the UART rx_mux at higher priority,
     * which causes uart_flush_input() to deadlock (priority starvation). */
    vTaskDelay(pdMS_TO_TICKS(50));

    char resp[64];
    for (int i = 0; i < 3; i++) {
        if (ml307_send_at(s, "AT", resp, sizeof(resp), 500) == 0) {
            return true;
        }
    }
    return false;
}

static int init_modem(ml307_state_t *s, int target_baud, const char *apn) {
    char resp[256];

    // ML307_LOGF("[ML307] Auto-detecting baudrate (target=%d)...\n", target_baud);

    /* Auto-detect baudrate and switch to target */
    bool found_target = try_baudrate(s, target_baud);

    // ML307_LOGF("[ML307] Target baud result=%d\n", found_target);

    if (!found_target) {
        // ML307_LOGF("[ML307] Trying 115200 baud...\n");

        if (!try_baudrate(s, 115200)) {
            ML307_LOGF("[ML307] ERROR: Modem not responding!\n");
            return -1;
        }

        /* Switch to target baudrate */
        // ML307_LOGF("[ML307] Found at 115200, switching to %d...\n", target_baud);
        if (target_baud != 115200) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+IPR=%d", target_baud);
            ml307_send_at(s, cmd, resp, sizeof(resp), 1000);
            uart_set_baudrate(s->uart_num, target_baud);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!try_baudrate(s, target_baud)) {
                ML307_LOGF("[ML307] ERROR: Failed to switch baud!\n");
                return -1;
            }
            // ML307_LOGF("[ML307] Switched to %d baud OK\n", target_baud);
        }
    } else {
        // ML307_LOGF("[ML307] Modem responding at %d baud\n", target_baud);
    }

    /* Disable echo, enable verbose errors */
    // ML307_LOGF("[ML307] Configuring modem...\n");
    send_at_ok(s, "ATE0", 1000);
    send_at_ok(s, "AT+CMEE=2", 1000);

    /* Cleanup any leftover sockets */
    // ML307_LOGF("[ML307] Cleaning up old sockets...\n");
    for (int i = 0; i < ML307_MAX_SOCKETS; i++) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", i);
        ml307_send_at(s, cmd, resp, sizeof(resp), 1000);
    }

    /* Check SIM card - retry a few times as it may take time to initialize */
    ML307_LOGF("[ML307] Checking SIM card...\n");

    bool sim_ready = false;
    for (int i = 0; i < 10; i++) {  // Retry up to 10 times (10 seconds)
        if (ml307_send_at(s, "AT+CPIN?", resp, sizeof(resp), 5000) == 0 &&
            strstr(resp, "READY") != NULL) {
            sim_ready = true;
            break;
        }
        ML307_LOGF("[ML307] SIM not ready yet (%d/10): %s\n", i + 1, resp);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!sim_ready) {
        ML307_LOGF("[ML307] ERROR: SIM not ready after 10 attempts\n");
        return -2;
    }
    ML307_LOGF("[ML307] SIM OK\n");

    /* Check signal strength */
    ML307_LOGF("[ML307] Checking signal...\n");

    if (ml307_send_at(s, "AT+CSQ", resp, sizeof(resp), 3000) == 0) {
        char *p = strstr(resp, "+CSQ:");
        if (p) {
            s->csq = atoi(p + 5);
            ML307_LOGF("[ML307] Signal CSQ=%d\n", s->csq);
            if (s->csq < 5 || s->csq == 99) {
                ML307_LOGF("[ML307] WARNING: Weak signal CSQ=%d, continuing anyway...\n", s->csq);
                // Don't fail on weak signal - let registration try
            }
        }
    }

    /* Wait for 4G registration */
    ML307_LOGF("[ML307] Waiting for 4G registration...\n");

    s->registered = false;
    for (int i = 0; i < 60; i++) {  // Increase to 60 seconds
        if (ml307_send_at(s, "AT+CEREG?", resp, sizeof(resp), 3000) == 0) {
            char *p = strstr(resp, "+CEREG:");
            if (p) {
                /* Skip first number (n), get stat */
                p = strchr(p + 7, ',');
                if (p) {
                    int stat = atoi(p + 1);
                    if (i % 5 == 0 || stat == 1 || stat == 5) {
                        ML307_LOGF("[ML307] CEREG stat=%d (%d/60)\n", stat, i + 1);
                    }
                    if (stat == 1 || stat == 5) {
                        s->registered = true;
                        ML307_LOGF("[ML307] 4G registered!\n");
                        break;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!s->registered) {
        ML307_LOGF("[ML307] ERROR: 4G registration timeout after 60s!\n");
        return -4;
    }

    /* Set APN and activate PDP */
    // ML307_LOGF("[ML307] Setting APN and activating PDP...\n");

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    send_at_ok(s, cmd, 3000);
    ml307_send_at(s, "AT+CGACT=1,1", resp, sizeof(resp), 15000);

    /* Get IP address */
    s->ip[0] = '\0';
    if (ml307_send_at(s, "AT+CGPADDR=1", resp, sizeof(resp), 3000) == 0) {
        char *p = strstr(resp, "+CGPADDR:");
        if (p) {
            p = strchr(p, ',');
            if (p) {
                p++; /* skip comma */
                /* Extract IP, remove quotes */
                char *dst = s->ip;
                while (*p && *p != '\r' && *p != '\n' &&
                       (dst - s->ip) < (int)sizeof(s->ip) - 1) {
                    if (*p != '"') {
                        *dst++ = *p;
                    }
                    p++;
                }
                *dst = '\0';
            }
        }
    }

    if (s->ip[0]) {
        ML307_LOGF("[ML307] IP: %s\n", s->ip);
    }

    s->initialized = true;
    // ML307_LOGF("[ML307] Init complete!\n");
    return 0;
}

/* ---- Public API: Init/Deinit ---- */

int ml307_init(ml307_state_t *s, int tx_pin, int rx_pin, int baudrate,
               const char *apn, bool debug) {
    memset(s, 0, sizeof(*s));
    s->uart_num = UART_NUM_1;
    s->tx_pin = tx_pin;
    s->rx_pin = rx_pin;
    s->baudrate = baudrate;
    s->debug = debug;
    s->last_cme_error = -1;

    /* Create synchronization primitives */
    // ML307_LOGF("[ML307] Creating sync primitives...\n");
    s->at_mutex = xSemaphoreCreateMutex();
    s->at_event = xEventGroupCreate();
    if (!s->at_mutex || !s->at_event) {
        ML307_LOGF("[ML307] ERROR: Failed to create sync primitives\n");
        return -1;
    }

    /* Initialize socket slots */
    // ML307_LOGF("[ML307] Allocating socket buffers...\n");
    for (int i = 0; i < ML307_MAX_SOCKETS; i++) {
        s->sock[i].state = ML307_SOCK_FREE;
        s->sock[i].rx_buf = (uint8_t *)malloc(ML307_SOCK_RXBUF_SIZE);
        if (!s->sock[i].rx_buf) {
            ML307_LOGF("[ML307] ERROR: Failed to alloc socket %d buffer\n", i);
            ml307_deinit(s);
            return -1;
        }
        s->sock[i].rx_head = 0;
        s->sock[i].rx_tail = 0;
        s->sock[i].disconnected = false;
        s->sock[i].overflow = false;
        s->sock[i].open_result = -1;
        s->sock[i].event = xEventGroupCreate();
        s->sock[i].mutex = xSemaphoreCreateMutex();
        if (!s->sock[i].event || !s->sock[i].mutex) {
            ML307_LOGF("[ML307] ERROR: Failed to create socket %d sync\n", i);
            ml307_deinit(s);
            return -1;
        }
    }

    /* Setup UART */
    // ML307_LOGF("[ML307] Setting up UART%d (tx=%d, rx=%d)...\n",
    //        s->uart_num, tx_pin, rx_pin);

    /* Delete UART driver if already installed (e.g. from previous run or machine.UART) */
    uart_driver_delete(s->uart_num);

    /* Install UART driver */
    uart_config_t uart_cfg = {
        .baud_rate = 115200, /* Start at 115200, switch later if needed */
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(s->uart_num, &uart_cfg);
    if (err != ESP_OK) {
        ML307_LOGF("[ML307] ERROR: uart_param_config failed: %d\n", err);
        ml307_deinit(s);
        return -1;
    }
    err = uart_set_pin(s->uart_num, tx_pin, rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ML307_LOGF("[ML307] ERROR: uart_set_pin failed: %d\n", err);
        ml307_deinit(s);
        return -1;
    }

#ifdef ML307_USE_UHCI_DMA
    /* UHCI DMA mode: do NOT install UART driver (UHCI handles RX, direct FIFO for TX) */

    /* Create RX data queue */
    s->rx_queue = xQueueCreate(ML307_RX_QUEUE_DEPTH, sizeof(ml307_rx_item_t));
    if (!s->rx_queue) {
        ML307_LOGF("[ML307] ERROR: Failed to create RX queue\n");
        ml307_deinit(s);
        return -1;
    }

    /* Initialize UHCI DMA controller */
    s->uhci = new (std::nothrow) UartUhci();
    if (!s->uhci) {
        ML307_LOGF("[ML307] ERROR: Failed to create UartUhci\n");
        ml307_deinit(s);
        return -1;
    }

    UartUhci::Config uhci_cfg = {
        .uart_port = s->uart_num,
        .dma_burst_size = 16,
        .rx_pool = {
            .buffer_count = ML307_DMA_BUF_COUNT,
            .buffer_size = ML307_DMA_BUF_SIZE,
        },
    };
    
    UartUhci *uhci = static_cast<UartUhci*>(s->uhci);
    err = uhci->Init(uhci_cfg);
    if (err != ESP_OK) {
        ML307_LOGF("[ML307] ERROR: UHCI Init failed: %d\n", err);
        ml307_deinit(s);
        return -1;
    }

    /* Register DMA callbacks */
    uhci->SetRxCallback(ml307_dma_rx_callback, s);
    uhci->SetOverflowCallback(ml307_dma_overflow_callback, s);

    /* Start DMA receive */
    err = uhci->StartReceive();
    if (err != ESP_OK) {
        ML307_LOGF("[ML307] ERROR: UHCI StartReceive failed: %d\n", err);
        ml307_deinit(s);
        return -1;
    }

    ML307_LOGF("[ML307] UHCI DMA initialized (%d buffers x %d bytes)\n", 
               ML307_DMA_BUF_COUNT, ML307_DMA_BUF_SIZE);

#else /* Standard UART mode */
    err = uart_driver_install(s->uart_num,
                              ML307_UART_RXBUF_SIZE, ML307_UART_TXBUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ML307_LOGF("[ML307] ERROR: uart_driver_install failed: %d\n", err);
        ml307_deinit(s);
        return -1;
    }

    /* Enable UART RX timeout interrupt.
     * Without this, uart_read_bytes() only fires on RX FIFO full (120 bytes),
     * causing short AT responses like "OK\r\n" (4 bytes) to wait the full
     * uart_read_bytes() timeout instead of returning immediately.
     * 10 symbol periods at 921600 baud = ~0.1ms idle time before interrupt. */
    uart_set_rx_timeout(s->uart_num, 10);
#endif

    /* Start background task */
    // ML307_LOGF("[ML307] Starting background AT task...\n");
    s->task_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ml307_at_task, "ml307_at",
        6144, s,
        configMAX_PRIORITIES - 3,
        &s->task_handle,
        1  /* Pin to core 1 (MicroPython typically runs on core 0) */
    );
    if (ret != pdPASS) {
        ML307_LOGF("[ML307] ERROR: Failed to create AT task\n");
        s->task_running = false;
        ml307_deinit(s);
        return -1;
    }

    /* Initialize modem (auto-baud, SIM, 4G, APN) */
    // ML307_LOGF("[ML307] Starting modem init...\n");
    int rc = init_modem(s, baudrate, apn);
    if (rc != 0) {
        ML307_LOGF("[ML307] ERROR: Modem init failed: %d\n", rc);
        /* Fix 7: Clean up on init failure to allow safe re-init.
         * Previously left resources partially initialized. */
        ml307_deinit(s);
    }
    return rc;
}

void ml307_deinit(ml307_state_t *s) {
    /* Stop background task — must happen before deleting UART/resources */
    if (s->task_running) {
        s->task_running = false;
        if (s->task_handle) {
            /* Wait for task to notice task_running=false and exit. */
            vTaskDelay(pdMS_TO_TICKS(300));
            /* If still alive, force delete */
            eTaskState state = eTaskGetState(s->task_handle);
            if (state != eDeleted && state != eInvalid) {
                vTaskDelete(s->task_handle);
            }
            s->task_handle = NULL;
        }
    }

#ifdef ML307_USE_UHCI_DMA
    /* Stop and cleanup UHCI DMA */
    if (s->uhci) {
        UartUhci *uhci = static_cast<UartUhci*>(s->uhci);
        uhci->StopReceive();
        uhci->Deinit();
        delete uhci;
        s->uhci = nullptr;
    }
    
    /* Delete RX queue */
    if (s->rx_queue) {
        vQueueDelete(s->rx_queue);
        s->rx_queue = nullptr;
    }
    /* No UART driver to delete in UHCI mode */
#else
    /* Uninstall UART (safe to call even if not installed) */
    if (s->uart_num > 0) {
        uart_driver_delete(s->uart_num);
    }
#endif

    /* Free socket resources */
    for (int i = 0; i < ML307_MAX_SOCKETS; i++) {
        if (s->sock[i].rx_buf) {
            free(s->sock[i].rx_buf);
            s->sock[i].rx_buf = NULL;
        }
        if (s->sock[i].event) {
            vEventGroupDelete(s->sock[i].event);
            s->sock[i].event = NULL;
        }
        if (s->sock[i].mutex) {
            vSemaphoreDelete(s->sock[i].mutex);
            s->sock[i].mutex = NULL;
        }
    }

    if (s->at_mutex) {
        vSemaphoreDelete(s->at_mutex);
        s->at_mutex = NULL;
    }
    if (s->at_event) {
        vEventGroupDelete(s->at_event);
        s->at_event = NULL;
    }
}

/* ---- Public API: Socket Operations ---- */

int ml307_sock_alloc(ml307_state_t *s) {
    for (int i = 0; i < ML307_MAX_SOCKETS; i++) {
        if (s->sock[i].state == ML307_SOCK_FREE) {
            s->sock[i].state = ML307_SOCK_ALLOCATED;
            s->sock[i].rx_head = 0;
            s->sock[i].rx_tail = 0;
            s->sock[i].disconnected = false;
            s->sock[i].overflow = false;
            s->sock[i].open_result = -1;
            xEventGroupClearBits(s->sock[i].event,
                SOCK_EVT_CONNECTED | SOCK_EVT_DISCONN |
                SOCK_EVT_ERROR | SOCK_EVT_DATA | SOCK_EVT_SEND_DONE);
            return i;
        }
    }
    return -1; /* No free socket */
}

void ml307_sock_free(ml307_state_t *s, int sid) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return;
    s->sock[sid].state = ML307_SOCK_FREE;
    s->sock[sid].rx_head = 0;
    s->sock[sid].rx_tail = 0;
    s->sock[sid].disconnected = false;
    s->sock[sid].overflow = false;
    s->sock[sid].open_result = -1;
}

int ml307_sock_connect(ml307_state_t *s, int sid, const char *host, int port,
                       bool ssl, int timeout_ms) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return -1;
    if (s->sock[sid].state < ML307_SOCK_ALLOCATED) return -1;

    char cmd[256];
    char resp[256];

    /* Fix 5: Query MIPSTATE to detect stale modem-side connections
     * (borrowed from esp-ml307's Connect() pre-check) */
    snprintf(cmd, sizeof(cmd), "AT+MIPSTATE=%d", sid);
    if (ml307_send_at(s, cmd, resp, sizeof(resp), 3000) == 0) {
        /* If modem reports a non-INITIAL state, close it first */
        if (strstr(resp, "CONNECTED") || strstr(resp, "CONNECTING")) {
            if (s->debug) {
                ESP_LOGI(TAG, "Cleaning stale connection on sid=%d", sid);
            }
            snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", sid);
            ml307_send_at(s, cmd, resp, sizeof(resp), 5000);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Configure HEX encoding */
    snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"encoding\",%d,1,1", sid);
    if (send_at_ok(s, cmd, 1000) != 0) {
        ESP_LOGE(TAG, "Failed to configure encoding for sid=%d", sid);
        return -2;
    }

    /* Fix 1: Configure SSL — check result to avoid silent fallback to plaintext */
    snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",%d,%d,0", sid, ssl ? 1 : 0);
    if (send_at_ok(s, cmd, 1000) != 0) {
        ESP_LOGE(TAG, "SSL config failed for sid=%d (ssl=%d)", sid, ssl);
        if (ssl) {
            return -2; /* Fail if SSL was requested but couldn't be configured */
        }
        /* Non-SSL: warn but continue */
    }

    /* Open connection */
    s->sock[sid].state = ML307_SOCK_CONNECTING;
    s->sock[sid].open_result = -1;
    xEventGroupClearBits(s->sock[sid].event,
        SOCK_EVT_CONNECTED | SOCK_EVT_ERROR);

    snprintf(cmd, sizeof(cmd),
             "AT+MIPOPEN=%d,\"TCP\",\"%s\",%d,,0", sid, host, port);
    int rc = ml307_send_at(s, cmd, resp, sizeof(resp), 5000);
    if (rc < 0 && strstr(resp, "ERROR") != NULL) {
        s->sock[sid].state = ML307_SOCK_ALLOCATED;
        return -2; /* AT command error */
    }

    /* Wait for +MIPOPEN URC */
    EventBits_t bits = xEventGroupWaitBits(
        s->sock[sid].event,
        SOCK_EVT_CONNECTED | SOCK_EVT_ERROR,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & SOCK_EVT_CONNECTED) {
        return 0;
    }

    /* Failed or timeout */
    s->sock[sid].state = ML307_SOCK_ALLOCATED;
    if (bits & SOCK_EVT_ERROR) {
        ESP_LOGE(TAG, "Connect failed: MIPOPEN error=%d", s->sock[sid].open_result);
        return -3;
    }
    ESP_LOGE(TAG, "Connect timeout");
    return -4;
}

int ml307_sock_send(ml307_state_t *s, int sid, const uint8_t *data, int len) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return -1;
    if (s->sock[sid].state != ML307_SOCK_CONNECTED) return -1;

    /* TX buffer: prefix + hex data + CRLF */
    char *tx_buf = (char *)malloc(ML307_MAX_SEND_CHUNK * 2 + 64);
    if (!tx_buf) return -1;

    char resp[128];
    int total_sent = 0;

    while (total_sent < len) {
        int chunk = len - total_sent;
        if (chunk > ML307_MAX_SEND_CHUNK) chunk = ML307_MAX_SEND_CHUNK;

        /* Build AT+MIPSEND=<id>,<len>,<hex>\r\n */
        int pos = snprintf(tx_buf, 64, "AT+MIPSEND=%d,%d,", sid, chunk);
        ml307_hex_encode(data + total_sent, chunk, tx_buf + pos);
        pos += chunk * 2;

        if (s->debug) {
            ESP_LOGI(TAG, "> AT+MIPSEND=%d,%d,<hex %d chars>", sid, chunk, chunk * 2);
        }

        /* Send directly to UART (bypass send_at to avoid double-locking) */
        xSemaphoreTake(s->at_mutex, portMAX_DELAY);

        s->resp_len = 0;
        s->resp_buf[0] = '\0';
        s->resp_truncated = false;
        xEventGroupClearBits(s->at_event, AT_EVT_RESP_READY);
        s->at_cmd_echo[0] = '\0'; /* MIPSEND commands are too long to echo-match */
        s->at_waiting = true;

        /* Write command + CRLF to UART */
        int wr1 = ml307_uart_write(s, tx_buf, pos);
        int wr2 = ml307_uart_write(s, "\r\n", 2);

        if (wr1 < 0 || wr2 < 0) {
            s->at_waiting = false;
            xSemaphoreGive(s->at_mutex);
            if (total_sent == 0) {
                free(tx_buf);
                return -2;
            }
            break;
        }

        /* Calculate timeout based on data size and baud rate.
         * tx_time covers UART transmission; add small margin for modem processing.
         * With proper UART RX interrupt, response comes back in ~1-2ms. */
        int tx_time_ms = (pos * 10 * 1000) / s->baudrate;
        int wait_ms = tx_time_ms + 500;

        EventBits_t bits = xEventGroupWaitBits(
            s->at_event, AT_EVT_RESP_READY,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(wait_ms));

        s->at_waiting = false;

        /* Check result */
        bool ok = (bits & AT_EVT_RESP_READY) && strstr(s->resp_buf, "OK");

        xSemaphoreGive(s->at_mutex);

        if (!ok) {
            if (total_sent == 0) {
                free(tx_buf);
                return -2; /* Send error */
            }
            break; /* Partial send */
        }

        total_sent += chunk;
    }

    free(tx_buf);
    return total_sent;
}

int ml307_sock_recv(ml307_state_t *s, int sid, uint8_t *buf, int maxlen,
                    int timeout_ms) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return -1;

    /* Try to read from ring buffer immediately */
    xSemaphoreTake(s->sock[sid].mutex, portMAX_DELAY);
    int n = ringbuf_read(&s->sock[sid], buf, maxlen);
    xSemaphoreGive(s->sock[sid].mutex);
    if (n > 0) return n;

    /* Check disconnect */
    if (s->sock[sid].disconnected) return 0;

    /* Non-blocking */
    if (timeout_ms == 0) return 0;

    /* Block until data or timeout */
    TickType_t wait = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(
        s->sock[sid].event,
        SOCK_EVT_DATA | SOCK_EVT_DISCONN,
        pdTRUE, pdFALSE,
        wait);

    if (bits & SOCK_EVT_DATA) {
        xSemaphoreTake(s->sock[sid].mutex, portMAX_DELAY);
        n = ringbuf_read(&s->sock[sid], buf, maxlen);
        xSemaphoreGive(s->sock[sid].mutex);
        if (n > 0) return n;
    }

    if (s->sock[sid].disconnected) return 0;

    return 0; /* Timeout */
}

int ml307_sock_available(ml307_state_t *s, int sid) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return 0;
    xSemaphoreTake(s->sock[sid].mutex, portMAX_DELAY);
    int n = ringbuf_available(&s->sock[sid]);
    xSemaphoreGive(s->sock[sid].mutex);
    return n;
}

bool ml307_sock_is_disconnected(ml307_state_t *s, int sid) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return true;
    return s->sock[sid].disconnected;
}

bool ml307_sock_check_overflow(ml307_state_t *s, int sid) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return false;
    bool was_overflow = s->sock[sid].overflow;
    s->sock[sid].overflow = false;  /* Clear on read */
    return was_overflow;
}

#ifdef ML307_USE_UHCI_DMA
bool ml307_check_dma_overflow(ml307_state_t *s) {
    bool was_overflow = s->dma_overflow;
    s->dma_overflow = false;  /* Clear on read */
    
    /* Also check UHCI internal overflow flag */
    if (s->uhci) {
        UartUhci *uhci = static_cast<UartUhci*>(s->uhci);
        if (uhci->CheckAndClearOverflow()) {
            was_overflow = true;
        }
    }
    return was_overflow;
}
#endif

void ml307_sock_close(ml307_state_t *s, int sid) {
    if (sid < 0 || sid >= ML307_MAX_SOCKETS) return;
    if (s->sock[sid].state == ML307_SOCK_CONNECTED ||
        s->sock[sid].state == ML307_SOCK_CONNECTING) {
        char cmd[24];
        char resp[64];
        snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", sid);
        ml307_send_at(s, cmd, resp, sizeof(resp), 5000);
        /* Ignore errors (551 = already disconnected) */
    }
    ml307_sock_free(s, sid);
}
