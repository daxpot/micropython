/*
 * ML307R HEX Encoding/Decoding
 * Inline functions for binary-safe AT command data transfer.
 */

#ifndef ML307_HEX_H
#define ML307_HEX_H

#include <stdint.h>
#include <stddef.h>

static const char ml307_hex_chars[] = "0123456789ABCDEF";

static inline uint8_t ml307_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

/* Encode binary data to hex string. out must have space for len*2 bytes (no NUL). */
static inline void ml307_hex_encode(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = ml307_hex_chars[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = ml307_hex_chars[in[i] & 0x0F];
    }
}

/* Decode hex string to binary. out must have space for hex_len/2 bytes.
 * Returns number of decoded bytes. */
static inline size_t ml307_hex_decode(const char *hex, size_t hex_len, uint8_t *out) {
    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; i++) {
        out[i] = (ml307_hex_nibble(hex[i * 2]) << 4) | ml307_hex_nibble(hex[i * 2 + 1]);
    }
    return n;
}

#endif /* ML307_HEX_H */
