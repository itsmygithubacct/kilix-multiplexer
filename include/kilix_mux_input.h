#ifndef KILIX_MUX_INPUT_H
#define KILIX_MUX_INPUT_H

#include "kilix_mux.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional acknowledged-input payloads. Legacy INPUT remains unchanged.
 * These helpers implement wire validation only, not session ownership,
 * sequence ordering, retransmission, or acknowledgement policy. */
/* INPUT_CLOSE uses the ACK payload: the accepted counter the client knows. */

#define KMX_INPUT_WIRE_VERSION 1u
#define KMX_INPUT_TOKEN_SIZE 16u
#define KMX_INPUT_OPEN_WIRE_SIZE 44u
#define KMX_INPUT_STATE_WIRE_SIZE 48u
#define KMX_INPUT_DATA_HEADER_SIZE 12u
#define KMX_INPUT_DATA_MAX 32768u
#define KMX_INPUT_ACK_WIRE_SIZE 12u

typedef enum {
    KMX_INPUT_READY = 0,
    KMX_INPUT_BUSY = 1,
    KMX_INPUT_EXPIRED = 2,
    KMX_INPUT_EPOCH = 3,
    KMX_INPUT_LIMIT = 4,
    KMX_INPUT_DENIED = 5
} kmx_input_status;

typedef struct {
    unsigned char epoch[KMX_INPUT_TOKEN_SIZE]; /* zero requests a new session */
    unsigned char client_id[KMX_INPUT_TOKEN_SIZE]; /* must be nonzero */
    uint64_t last_ack;
} kmx_input_open;

typedef struct {
    kmx_input_status status;
    unsigned char epoch[KMX_INPUT_TOKEN_SIZE]; /* must be nonzero */
    unsigned char client_id[KMX_INPUT_TOKEN_SIZE]; /* must be nonzero */
    uint64_t accepted;
    uint32_t grace_ms;
} kmx_input_state;

typedef struct {
    uint8_t pane; /* less than KMX_MAX_PANES */
    uint64_t sequence; /* nonzero */
    const unsigned char *data;
    size_t size; /* 1..KMX_INPUT_DATA_MAX */
} kmx_input_data;

typedef struct {
    uint64_t accepted; /* zero is valid before any input is accepted */
} kmx_input_ack;

/* Encoders append a payload, without KMX framing, to an initialized buffer.
 * They leave existing bytes and size unchanged on failure. Integers are big
 * endian; version is 1; all flags and reserved bytes are zero. */
kmx_result kmx_input_open_encode(const kmx_input_open *value, kmx_buffer *out);
kmx_result kmx_input_state_encode(const kmx_input_state *value, kmx_buffer *out);
kmx_result kmx_input_data_encode(const kmx_input_data *value, kmx_buffer *out);
kmx_result kmx_input_ack_encode(const kmx_input_ack *value, kmx_buffer *out);

/* Decode exactly one complete payload. Every failure leaves *out unchanged.
 * DATA borrows its bytes from wire; retain wire until those bytes are consumed.
 * Short payloads return TRUNCATED, oversized fixed payloads return PROTOCOL,
 * DATA above its maximum returns LIMIT, and malformed fields return PROTOCOL.
 * NULL arguments return INVALID. */
kmx_result kmx_input_open_decode(const void *wire, size_t size, kmx_input_open *out);
kmx_result kmx_input_state_decode(const void *wire, size_t size, kmx_input_state *out);
kmx_result kmx_input_data_decode(const void *wire, size_t size, kmx_input_data *out);
kmx_result kmx_input_ack_decode(const void *wire, size_t size, kmx_input_ack *out);

#ifdef __cplusplus
}
#endif

#endif
