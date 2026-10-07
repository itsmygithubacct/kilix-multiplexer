#include "kilix_mux_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
require(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL  %s\n", message);
    exit(1);
}

typedef enum { OPEN, STATE, DATA, ACK } kind;

static size_t
wire_size(kind type) {
    switch (type) {
        case OPEN: return KMX_INPUT_OPEN_WIRE_SIZE;
        case STATE: return KMX_INPUT_STATE_WIRE_SIZE;
        case DATA: return KMX_INPUT_DATA_HEADER_SIZE + 1u;
        case ACK: return KMX_INPUT_ACK_WIRE_SIZE;
    }
    abort();
}

/* Every failed decode must preserve every byte, including padding and the
 * DATA pointer. Also re-encode successes to check canonical wire bytes. */
#define CHECK_DECODE(name, type) do { \
    type value, saved; \
    memset(&value, 0xa5, sizeof value); \
    memcpy(&saved, &value, sizeof value); \
    result = kmx_input_##name##_decode(wire, size, &value); \
    if (result != KMX_OK) { \
        require(memcmp(&value, &saved, sizeof value) == 0, \
                "failed decode leaves entire output unchanged"); \
    } else { \
        require(kmx_input_##name##_encode(&value, &canonical) == KMX_OK, \
                "successful decode can be re-encoded"); \
    } \
} while (0)

static kmx_result
checked_decode(kind type, const void *wire, size_t size) {
    kmx_buffer canonical;
    kmx_result result = KMX_ERR_INVALID;
    kmx_buffer_init(&canonical);
    switch (type) {
        case OPEN: CHECK_DECODE(open, kmx_input_open); break;
        case STATE: CHECK_DECODE(state, kmx_input_state); break;
        case DATA: CHECK_DECODE(data, kmx_input_data); break;
        case ACK: CHECK_DECODE(ack, kmx_input_ack); break;
    }
    if (result == KMX_OK) {
        require(canonical.size == size && memcmp(canonical.data, wire, size) == 0,
                "successful decode re-encodes byte-for-byte");
    }
    kmx_buffer_free(&canonical);
    return result;
}

static void
fixture(kind type, unsigned char *wire) {
    memset(wire, 0, KMX_INPUT_STATE_WIRE_SIZE);
    wire[0] = 1;
    switch (type) {
        case OPEN: wire[20] = 0x80; break;
        case STATE: wire[4] = 0x80; wire[35] = 1; break;
        case DATA: wire[11] = 1; wire[12] = 0xff; break;
        case ACK: break;
    }
}

static void
test_golden(void) {
    const unsigned char open_wire[] = {
        1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
        1, 2, 3, 4, 5, 6, 7, 8
    };
    const unsigned char state_wire[] = {
        1, 5, 0, 0,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
        1, 2, 3, 4, 5, 6, 7, 8, 0xab, 0xcd, 0xef, 1
    };
    const unsigned char data_wire[] = {
        1, 31, 0, 0, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0, 0x1b, 0xff
    };
    const unsigned char ack_wire[] = {
        1, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8
    };
    kmx_input_open open = {0}, decoded_open;
    kmx_input_state state = {0}, decoded_state;
    kmx_input_data data = {31, UINT64_C(0xfedcba9876543210), data_wire + 12, 3};
    kmx_input_data decoded_data;
    kmx_input_ack ack = {UINT64_C(0x0102030405060708)}, decoded_ack;
    kmx_buffer out;
    const unsigned char prefix[] = {0x99, 0x88};
    kmx_buffer_init(&out);
    for (size_t i = 0; i < KMX_INPUT_TOKEN_SIZE; i++) {
        open.client_id[i] = (unsigned char)(16 - i);
        state.client_id[i] = open.client_id[i];
        state.epoch[i] = (unsigned char)(i + 1);
    }
    open.last_ack = state.accepted = ack.accepted;
    state.status = KMX_INPUT_DENIED;
    state.grace_ms = UINT32_C(0xabcdef01);
    require(kmx_buffer_append(&out, prefix, sizeof prefix) == KMX_OK, "append prefix");
    require(kmx_input_open_encode(&open, &out) == KMX_OK, "encode OPEN");
    require(out.size == sizeof prefix + sizeof open_wire &&
            memcmp(out.data, prefix, sizeof prefix) == 0 &&
            memcmp(out.data + sizeof prefix, open_wire, sizeof open_wire) == 0,
            "OPEN appends exact golden bytes");
    require(kmx_input_open_decode(open_wire, sizeof open_wire, &decoded_open) == KMX_OK &&
            decoded_open.last_ack == open.last_ack &&
            memcmp(decoded_open.epoch, open.epoch, KMX_INPUT_TOKEN_SIZE) == 0 &&
            memcmp(decoded_open.client_id, open.client_id, KMX_INPUT_TOKEN_SIZE) == 0,
            "decode OPEN fields with zero epoch");
    kmx_buffer_reset(&out);
    require(kmx_input_state_encode(&state, &out) == KMX_OK &&
            out.size == sizeof state_wire && memcmp(out.data, state_wire, out.size) == 0,
            "STATE exact golden bytes");
    require(kmx_input_state_decode(state_wire, sizeof state_wire, &decoded_state) == KMX_OK &&
            decoded_state.status == state.status && decoded_state.accepted == state.accepted &&
            decoded_state.grace_ms == state.grace_ms &&
            memcmp(decoded_state.epoch, state.epoch, KMX_INPUT_TOKEN_SIZE) == 0 &&
            memcmp(decoded_state.client_id, state.client_id, KMX_INPUT_TOKEN_SIZE) == 0,
            "decode STATE fields");
    kmx_buffer_reset(&out);
    require(kmx_input_data_encode(&data, &out) == KMX_OK &&
            out.size == sizeof data_wire && memcmp(out.data, data_wire, out.size) == 0,
            "DATA exact golden bytes including binary input");
    require(kmx_input_data_decode(data_wire, sizeof data_wire, &decoded_data) == KMX_OK &&
            decoded_data.pane == data.pane && decoded_data.sequence == data.sequence &&
            decoded_data.data == data_wire + 12 && decoded_data.size == 3,
            "DATA borrows wire payload and decodes header");
    kmx_buffer_reset(&out);
    require(kmx_input_ack_encode(&ack, &out) == KMX_OK &&
            out.size == sizeof ack_wire && memcmp(out.data, ack_wire, out.size) == 0,
            "ACK exact golden bytes");
    require(kmx_input_ack_decode(ack_wire, sizeof ack_wire, &decoded_ack) == KMX_OK &&
            decoded_ack.accepted == ack.accepted, "decode ACK counter");
    kmx_buffer_free(&out);
    require(KMX_MSG_INPUT_OPEN == 15 && KMX_MSG_INPUT_STATE == 16 &&
            KMX_MSG_INPUT_DATA == 17 && KMX_MSG_INPUT_ACK == 18 &&
            KMX_MSG_INPUT_CLOSE == 19, "message type assignments");
}

static void
test_headers_and_sizes(void) {
    unsigned char storage[KMX_INPUT_STATE_WIRE_SIZE + 2], *wire = storage + 1;
    for (kind type = OPEN; type <= ACK; type++) {
        size_t length = wire_size(type);
        fixture(type, wire);
        require(checked_decode(type, wire, length) == KMX_OK, "unaligned valid payload");
        for (size_t n = 0; n < length; n++) {
            require(checked_decode(type, wire, n) == KMX_ERR_TRUNCATED,
                    "every prefix is truncated and preserves output");
        }
        if (type != DATA) {
            require(checked_decode(type, wire, length + 1) == KMX_ERR_PROTOCOL,
                    "fixed payload rejects trailing bytes");
            require(checked_decode(type, wire, SIZE_MAX) == KMX_ERR_PROTOCOL,
                    "fixed payload rejects impossible size before reading bytes");
        }
        for (unsigned version = 0; version <= 255; version++) {
            wire[0] = (unsigned char)version;
            require(checked_decode(type, wire, length) ==
                    (version == 1 ? KMX_OK : KMX_ERR_PROTOCOL),
                    "only version one is accepted");
        }
        wire[0] = 1;
        for (size_t offset = 1; offset < 4; offset++) {
            if ((type == STATE || type == DATA) && offset == 1) continue;
            for (unsigned byte = 1; byte <= 255; byte++) {
                wire[offset] = (unsigned char)byte;
                require(checked_decode(type, wire, length) == KMX_ERR_PROTOCOL,
                        "every nonzero flag or reserved byte is rejected");
            }
            wire[offset] = 0;
        }
        if (type == STATE) {
            for (unsigned status = 0; status <= 255; status++) {
                wire[1] = (unsigned char)status;
                require(checked_decode(type, wire, length) ==
                        (status <= 5 ? KMX_OK : KMX_ERR_PROTOCOL),
                        "status accepts exactly zero through five");
            }
        }
        if (type == DATA) {
            for (unsigned pane = 0; pane <= 255; pane++) {
                wire[1] = (unsigned char)pane;
                require(checked_decode(type, wire, length) ==
                        (pane < KMX_MAX_PANES ? KMX_OK : KMX_ERR_PROTOCOL),
                        "pane must be within maximum");
            }
            wire[1] = 0;
            memset(wire + 4, 0, 8);
            require(checked_decode(type, wire, length) == KMX_ERR_PROTOCOL,
                    "zero DATA sequence rejected");
        }
        require(checked_decode(type, NULL, length) == KMX_ERR_INVALID,
                "NULL wire rejected without output mutation");
    }
    require(kmx_input_open_decode(wire, 44, NULL) == KMX_ERR_INVALID &&
            kmx_input_state_decode(wire, 48, NULL) == KMX_ERR_INVALID &&
            kmx_input_data_decode(wire, 13, NULL) == KMX_ERR_INVALID &&
            kmx_input_ack_decode(wire, 12, NULL) == KMX_ERR_INVALID,
            "NULL decode outputs rejected");
}

static void
test_tokens_and_counters(void) {
    unsigned char wire[KMX_INPUT_STATE_WIRE_SIZE];
    for (kind type = OPEN; type <= STATE; type++) {
        size_t length = wire_size(type);
        fixture(type, wire);
        memset(wire + 20, 0, KMX_INPUT_TOKEN_SIZE);
        require(checked_decode(type, wire, length) == KMX_ERR_PROTOCOL,
                "zero client ID rejected");
        for (size_t i = 0; i < KMX_INPUT_TOKEN_SIZE; i++) {
            wire[20 + i] = 1;
            require(checked_decode(type, wire, length) == KMX_OK,
                    "nonzero client ID byte at every token position accepted");
            wire[20 + i] = 0;
        }
        wire[20] = 1;
        memset(wire + 4, 0, KMX_INPUT_TOKEN_SIZE);
        require(checked_decode(type, wire, length) ==
                (type == OPEN ? KMX_OK : KMX_ERR_PROTOCOL),
                "only OPEN allows zero epoch");
        for (size_t i = 0; i < KMX_INPUT_TOKEN_SIZE; i++) {
            wire[4 + i] = 0x80;
            require(checked_decode(type, wire, length) == KMX_OK,
                    "nonzero epoch byte at every position accepted");
            wire[4 + i] = 0;
        }
    }
    for (kind type = OPEN; type <= ACK; type++) {
        size_t offset = type == OPEN || type == STATE ? 36 : 4;
        fixture(type, wire);
        memset(wire + offset, 0xff, 8);
        if (type == STATE) memset(wire + 44, 0xff, 4);
        require(checked_decode(type, wire, wire_size(type)) == KMX_OK,
                "maximum unsigned counter and grace accepted");
        memset(wire + offset, 0, 8);
        if (type == STATE) memset(wire + 44, 0, 4);
        require(checked_decode(type, wire, wire_size(type)) ==
                (type == DATA ? KMX_ERR_PROTOCOL : KMX_OK),
                "zero accepted/last_ack/grace valid, zero DATA sequence invalid");
    }
}

static void
expect_encode_failure(kind type, const void *value, kmx_buffer *out, kmx_result wanted) {
    unsigned char saved[64];
    size_t size = out->size;
    kmx_result result = KMX_OK;
    require(size <= sizeof saved, "encoder test prefix fits");
    memcpy(saved, out->data, size);
    switch (type) {
        case OPEN: result = kmx_input_open_encode(value, out); break;
        case STATE: result = kmx_input_state_encode(value, out); break;
        case DATA: result = kmx_input_data_encode(value, out); break;
        case ACK: result = kmx_input_ack_encode(value, out); break;
    }
    require(result == wanted, "invalid encode returns expected error");
    require(out->size == size && memcmp(out->data, saved, size) == 0,
            "failed encode preserves prefix bytes and size");
}

static void
test_encode_failures(void) {
    kmx_input_open open = {0};
    kmx_input_state state = {0};
    const unsigned char byte = 0;
    kmx_input_data data = {0, 1, &byte, 1};
    kmx_input_ack ack = {0};
    kmx_buffer out;
    kmx_buffer_init(&out);
    require(kmx_buffer_append(&out, "prefix", 6) == KMX_OK, "encoder prefix");
    for (kind type = OPEN; type <= ACK; type++) {
        expect_encode_failure(type, NULL, &out, KMX_ERR_INVALID);
    }
    require(kmx_input_open_encode(&open, NULL) == KMX_ERR_INVALID &&
            kmx_input_state_encode(&state, NULL) == KMX_ERR_INVALID &&
            kmx_input_data_encode(&data, NULL) == KMX_ERR_INVALID &&
            kmx_input_ack_encode(&ack, NULL) == KMX_ERR_INVALID,
            "NULL encode buffers rejected");
    expect_encode_failure(OPEN, &open, &out, KMX_ERR_INVALID);
    state.epoch[0] = 1;
    expect_encode_failure(STATE, &state, &out, KMX_ERR_INVALID);
    state.epoch[0] = 0;
    state.client_id[15] = 1;
    expect_encode_failure(STATE, &state, &out, KMX_ERR_INVALID);
    state.epoch[15] = 1;
    state.status = (kmx_input_status)-1;
    expect_encode_failure(STATE, &state, &out, KMX_ERR_INVALID);
    state.status = (kmx_input_status)6;
    expect_encode_failure(STATE, &state, &out, KMX_ERR_INVALID);
    state.status = (kmx_input_status)256;
    expect_encode_failure(STATE, &state, &out, KMX_ERR_INVALID);
    data.size = 0;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_INVALID);
    data.size = KMX_INPUT_DATA_MAX + 1u;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_LIMIT);
    data.size = SIZE_MAX;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_LIMIT);
    data.size = 1;
    data.data = NULL;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_INVALID);
    data.data = &byte;
    data.sequence = 0;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_INVALID);
    data.sequence = 1;
    data.pane = KMX_MAX_PANES;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_INVALID);
    data.pane = 255;
    expect_encode_failure(DATA, &data, &out, KMX_ERR_INVALID);
    kmx_buffer_free(&out);
}

static void
test_data_bounds_and_alias(void) {
    unsigned char payload[KMX_INPUT_DATA_MAX];
    unsigned char too_large[KMX_INPUT_DATA_HEADER_SIZE + KMX_INPUT_DATA_MAX + 1u];
    kmx_input_data value = {KMX_MAX_PANES - 1, UINT64_MAX, payload, sizeof payload};
    kmx_input_data decoded;
    kmx_buffer out;
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (unsigned char)i;
    kmx_buffer_init(&out);
    require(kmx_input_data_encode(&value, &out) == KMX_OK &&
            out.size == KMX_INPUT_DATA_HEADER_SIZE + sizeof payload,
            "maximum DATA payload encoded");
    require(kmx_input_data_decode(out.data, out.size, &decoded) == KMX_OK &&
            decoded.size == sizeof payload && decoded.sequence == UINT64_MAX &&
            decoded.pane == KMX_MAX_PANES - 1 &&
            decoded.data == out.data + KMX_INPUT_DATA_HEADER_SIZE &&
            memcmp(decoded.data, payload, sizeof payload) == 0,
            "maximum DATA payload decoded exactly");
    require(checked_decode(DATA, out.data, out.size) == KMX_OK, "maximum canonical DATA");
    memcpy(too_large, out.data, out.size);
    too_large[sizeof too_large - 1] = 0;
    require(checked_decode(DATA, too_large, sizeof too_large) == KMX_ERR_LIMIT &&
            checked_decode(DATA, too_large, SIZE_MAX) == KMX_ERR_LIMIT,
            "oversized DATA is bounded before reading payload");
    /* Re-encode borrowed bytes into the same buffer, forcing growth while the
     * source pointer still refers to its old allocation. */
    size_t old_size = out.size;
    require(out.capacity < old_size * 2, "alias test forces capacity growth");
    require(kmx_input_data_encode(&decoded, &out) == KMX_OK &&
            out.size == old_size * 2 &&
            memcmp(out.data, out.data + old_size, old_size) == 0,
            "DATA append preserves payload borrowed from destination");
    kmx_buffer_free(&out);
}

static uint32_t
random_u32(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void
test_mutated_decoders(void) {
    unsigned char wire[KMX_INPUT_STATE_WIRE_SIZE + 32];
    uint32_t random = UINT32_C(0x719bbdfa);
    size_t successes = 0, failures = 0;
    for (size_t iteration = 0; iteration < 20000; iteration++) {
        kind type = (kind)(random_u32(&random) % 4);
        size_t length = wire_size(type);
        memset(wire, 0xcc, sizeof wire);
        fixture(type, wire);
        /* Alternate unconstrained bytes, near-valid field mutations and
         * size mutations so fuzz-like coverage includes actual successes. */
        if (iteration % 3 == 0) {
            for (size_t i = 0; i < length; i++) wire[i] = (unsigned char)random_u32(&random);
        } else if (iteration % 3 == 1) {
            size_t offset = random_u32(&random) % length;
            wire[offset] ^= (unsigned char)(1u << (random_u32(&random) % 8));
        } else {
            length = random_u32(&random) % sizeof wire;
        }
        if (checked_decode(type, wire, length) == KMX_OK) successes++;
        else failures++;
    }
    require(successes > 1000 && failures > 1000,
            "seeded mutations exercise successes and unchanged failures");
}

int
main(void) {
    test_golden();
    test_headers_and_sizes();
    test_tokens_and_counters();
    test_encode_failures();
    test_data_bounds_and_alias();
    test_mutated_decoders();
    puts("input wire codecs: golden, boundary, strict-header and 20000 mutation cases passed");
    return 0;
}
