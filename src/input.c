#include "kilix_mux_input.h"

#include <string.h>

static bool
token_nonzero(const unsigned char token[KMX_INPUT_TOKEN_SIZE]) {
    unsigned char combined = 0;
    for (size_t i = 0; i < KMX_INPUT_TOKEN_SIZE; i++) combined |= token[i];
    return combined != 0;
}

static bool
status_valid(kmx_input_status status) {
    return status >= KMX_INPUT_READY && status <= KMX_INPUT_DENIED;
}

static void
put_u64(unsigned char *out, uint64_t value) {
    for (size_t i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (56 - i * 8));
}

static uint64_t
get_u64(const unsigned char *wire) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value = (value << 8) | wire[i];
    return value;
}

static void
put_u32(unsigned char *out, uint32_t value) {
    for (size_t i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (24 - i * 8));
}

static uint32_t
get_u32(const unsigned char *wire) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++) value = (value << 8) | wire[i];
    return value;
}

static kmx_result
append_payload(kmx_buffer *out, const unsigned char *wire, size_t size) {
    /* The generic buffer assumes its size arithmetic cannot overflow. */
    if (out->size > SIZE_MAX - size) return KMX_ERR_LIMIT;
    return kmx_buffer_append(out, wire, size);
}

kmx_result
kmx_input_open_encode(const kmx_input_open *value, kmx_buffer *out) {
    unsigned char wire[KMX_INPUT_OPEN_WIRE_SIZE] = {KMX_INPUT_WIRE_VERSION};
    if (!value || !out || !token_nonzero(value->client_id)) return KMX_ERR_INVALID;
    memcpy(wire + 4, value->epoch, KMX_INPUT_TOKEN_SIZE);
    memcpy(wire + 20, value->client_id, KMX_INPUT_TOKEN_SIZE);
    put_u64(wire + 36, value->last_ack);
    return append_payload(out, wire, sizeof wire);
}

kmx_result
kmx_input_open_decode(const void *wire, size_t size, kmx_input_open *out) {
    const unsigned char *bytes = wire;
    kmx_input_open value = {0};
    if (!wire || !out) return KMX_ERR_INVALID;
    if (size < KMX_INPUT_OPEN_WIRE_SIZE) return KMX_ERR_TRUNCATED;
    if (size != KMX_INPUT_OPEN_WIRE_SIZE || bytes[0] != KMX_INPUT_WIRE_VERSION ||
        bytes[1] || bytes[2] || bytes[3] || !token_nonzero(bytes + 20)) {
        return KMX_ERR_PROTOCOL;
    }
    memcpy(value.epoch, bytes + 4, KMX_INPUT_TOKEN_SIZE);
    memcpy(value.client_id, bytes + 20, KMX_INPUT_TOKEN_SIZE);
    value.last_ack = get_u64(bytes + 36);
    *out = value;
    return KMX_OK;
}

kmx_result
kmx_input_state_encode(const kmx_input_state *value, kmx_buffer *out) {
    unsigned char wire[KMX_INPUT_STATE_WIRE_SIZE] = {KMX_INPUT_WIRE_VERSION};
    if (!value || !out || !status_valid(value->status) ||
        !token_nonzero(value->epoch) || !token_nonzero(value->client_id)) {
        return KMX_ERR_INVALID;
    }
    wire[1] = (unsigned char)value->status;
    memcpy(wire + 4, value->epoch, KMX_INPUT_TOKEN_SIZE);
    memcpy(wire + 20, value->client_id, KMX_INPUT_TOKEN_SIZE);
    put_u64(wire + 36, value->accepted);
    put_u32(wire + 44, value->grace_ms);
    return append_payload(out, wire, sizeof wire);
}

kmx_result
kmx_input_state_decode(const void *wire, size_t size, kmx_input_state *out) {
    const unsigned char *bytes = wire;
    kmx_input_state value = {0};
    if (!wire || !out) return KMX_ERR_INVALID;
    if (size < KMX_INPUT_STATE_WIRE_SIZE) return KMX_ERR_TRUNCATED;
    if (size != KMX_INPUT_STATE_WIRE_SIZE || bytes[0] != KMX_INPUT_WIRE_VERSION ||
        !status_valid((kmx_input_status)bytes[1]) || bytes[2] || bytes[3] ||
        !token_nonzero(bytes + 4) || !token_nonzero(bytes + 20)) {
        return KMX_ERR_PROTOCOL;
    }
    value.status = (kmx_input_status)bytes[1];
    memcpy(value.epoch, bytes + 4, KMX_INPUT_TOKEN_SIZE);
    memcpy(value.client_id, bytes + 20, KMX_INPUT_TOKEN_SIZE);
    value.accepted = get_u64(bytes + 36);
    value.grace_ms = get_u32(bytes + 44);
    *out = value;
    return KMX_OK;
}

kmx_result
kmx_input_data_encode(const kmx_input_data *value, kmx_buffer *out) {
    unsigned char wire[KMX_INPUT_DATA_HEADER_SIZE + KMX_INPUT_DATA_MAX];
    if (!value || !out || !value->data || !value->size ||
        value->pane >= KMX_MAX_PANES || !value->sequence) return KMX_ERR_INVALID;
    if (value->size > KMX_INPUT_DATA_MAX) return KMX_ERR_LIMIT;
    wire[0] = KMX_INPUT_WIRE_VERSION;
    wire[1] = value->pane;
    wire[2] = 0;
    wire[3] = 0;
    put_u64(wire + 4, value->sequence);
    /* Stage the bounded payload so one append is atomic, including when data
     * borrows an existing part of out and append reallocates that buffer. */
    memcpy(wire + KMX_INPUT_DATA_HEADER_SIZE, value->data, value->size);
    return append_payload(out, wire, KMX_INPUT_DATA_HEADER_SIZE + value->size);
}

kmx_result
kmx_input_data_decode(const void *wire, size_t size, kmx_input_data *out) {
    const unsigned char *bytes = wire;
    kmx_input_data value = {0};
    if (!wire || !out) return KMX_ERR_INVALID;
    if (size < KMX_INPUT_DATA_HEADER_SIZE + 1u) return KMX_ERR_TRUNCATED;
    if (size > KMX_INPUT_DATA_HEADER_SIZE + KMX_INPUT_DATA_MAX) return KMX_ERR_LIMIT;
    if (bytes[0] != KMX_INPUT_WIRE_VERSION || bytes[1] >= KMX_MAX_PANES ||
        bytes[2] || bytes[3]) return KMX_ERR_PROTOCOL;
    value.sequence = get_u64(bytes + 4);
    if (!value.sequence) return KMX_ERR_PROTOCOL;
    value.pane = bytes[1];
    value.data = bytes + KMX_INPUT_DATA_HEADER_SIZE;
    value.size = size - KMX_INPUT_DATA_HEADER_SIZE;
    *out = value;
    return KMX_OK;
}

kmx_result
kmx_input_ack_encode(const kmx_input_ack *value, kmx_buffer *out) {
    unsigned char wire[KMX_INPUT_ACK_WIRE_SIZE] = {KMX_INPUT_WIRE_VERSION};
    if (!value || !out) return KMX_ERR_INVALID;
    put_u64(wire + 4, value->accepted);
    return append_payload(out, wire, sizeof wire);
}

kmx_result
kmx_input_ack_decode(const void *wire, size_t size, kmx_input_ack *out) {
    const unsigned char *bytes = wire;
    kmx_input_ack value = {0};
    if (!wire || !out) return KMX_ERR_INVALID;
    if (size < KMX_INPUT_ACK_WIRE_SIZE) return KMX_ERR_TRUNCATED;
    if (size != KMX_INPUT_ACK_WIRE_SIZE || bytes[0] != KMX_INPUT_WIRE_VERSION ||
        bytes[1] || bytes[2] || bytes[3]) return KMX_ERR_PROTOCOL;
    value.accepted = get_u64(bytes + 4);
    *out = value;
    return KMX_OK;
}
