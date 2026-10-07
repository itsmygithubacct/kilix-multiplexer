/* Exercise the actual attach journal/transport functions without sockets.
 * Link with --wrap=send --wrap=kmx_tls_write
 *           --wrap=kmx_tls_write_wants_read. */
#define main kmx_attach_main
#include "../tools/kmx_attach.c"
#undef main

static kmx_buffer captured;
static kmx_buffer tls_first_offer;
static size_t mock_limit;
static int mock_error;
static bool mock_tls_read;
static size_t tls_calls;
static size_t tls_lengths[16];
static uint64_t tls_hashes[16];

static void
require(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL journal: %s\n", message);
    exit(1);
}

static uint64_t
hash_bytes(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

static long
mock_write(const void *data, size_t size) {
    size_t count;
    if (mock_error) {
        errno = mock_error;
        return -1;
    }
    count = mock_limit && mock_limit < size ? mock_limit : size;
    require(kmx_buffer_append(&captured, data, count) == KMX_OK,
            "capture allocation succeeds");
    return (long)count;
}

ssize_t
__wrap_send(int fd, const void *data, size_t size, int flags) {
    (void)fd;
    require((flags & MSG_NOSIGNAL) && (flags & MSG_DONTWAIT),
            "plain transport writes are nonblocking and suppress SIGPIPE");
    return (ssize_t)mock_write(data, size);
}

long
__wrap_kmx_tls_write(kmx_tls_session *session, const void *data, size_t size) {
    (void)session;
    require(tls_calls < sizeof tls_lengths / sizeof tls_lengths[0],
            "TLS test call log remains bounded");
    tls_lengths[tls_calls] = size;
    tls_hashes[tls_calls] = hash_bytes(data, size);
    if (!tls_calls) {
        require(kmx_buffer_append(&tls_first_offer, data, size) == KMX_OK,
                "retain first TLS offered prefix");
    }
    tls_calls++;
    return mock_write(data, size);
}

bool
__wrap_kmx_tls_write_wants_read(const kmx_tls_session *session) {
    (void)session;
    return mock_tls_read;
}

static void
fresh(void) {
    input_free();
    kmx_buffer_free(&outgoing.bytes);
    memset(&outgoing, 0, sizeof outgoing);
    memset(&reliable_input, 0, sizeof reliable_input);
    reliable_input.enabled = reliable_input.ready = reliable_input.identified = true;
    reliable_input.generation = 1;
    memset(reliable_input.epoch, 0x42, sizeof reliable_input.epoch);
    memset(reliable_input.client_id, 0x19, sizeof reliable_input.client_id);
    kmx_buffer_init(&outgoing.bytes);
    kmx_buffer_reset(&captured);
    kmx_buffer_reset(&tls_first_offer);
    mock_error = 0;
    mock_limit = 0;
    mock_tls_read = false;
    tls_calls = 0;
    memset(tls_lengths, 0, sizeof tls_lengths);
    memset(tls_hashes, 0, sizeof tls_hashes);
    stop_pending = 0;
}

static void
flush_plain(void) {
    mock_error = 0;
    mock_limit = 0;
    while (outgoing_pending()) require(outgoing_flush(123, NULL) == 0,
                                      "plain queue drains");
}

static void
check_captured_data(const uint64_t *sequences, const char *const *payloads,
                    const size_t *sizes, size_t count, size_t controls) {
    kmx_framer framer;
    size_t found = 0, found_controls = 0;
    kmx_framer_init(&framer);
    require(kmx_framer_push(&framer, captured.data, captured.size) == KMX_OK,
            "captured bytes enter real framer");
    for (;;) {
        bool available;
        kmx_message_type type;
        const unsigned char *payload;
        size_t size;
        require(kmx_framer_next(&framer, &available, &type, &payload, &size) == KMX_OK,
                "captured frames remain valid");
        if (!available) break;
        if (type == KMX_MSG_INPUT_DATA) {
            kmx_input_data decoded;
            require(found < count, "no duplicate/unexpected DATA frame");
            require(kmx_input_data_decode(payload, size, &decoded) == KMX_OK,
                    "captured DATA has strict valid payload");
            require(decoded.pane == 0 && decoded.sequence == sequences[found],
                    "DATA ordering and pane survive writes/replay");
            require(decoded.size == sizes[found] &&
                    !memcmp(decoded.data, payloads[found], sizes[found]),
                    "DATA payload survives writes/replay exactly");
            found++;
        } else {
            found_controls++;
        }
        kmx_framer_consume(&framer);
    }
    require(found == count && found_controls == controls && !framer.pending.size,
            "all captured frames are complete and accounted for");
    kmx_framer_free(&framer);
}

static void
test_plain_ack_ceiling(void) {
    const uint64_t sequences[] = {1, 2};
    const char *payloads[] = {"alpha", "beta"};
    const size_t sizes[] = {5, 4};
    uint64_t first_end;
    fresh();
    require(input_send(123, "alpha", 5) == 0 && input_send(123, "beta", 4) == 0,
            "two DATA records enter journal");
    first_end = reliable_input.head->wire_end;
    require(reliable_input.sequence == 2 && reliable_input.eligible_sequence == 0,
            "allocation is not emission");
    require(input_acknowledge(1) == -1 && reliable_input.entries == 2,
            "ACK before any emission cannot discard journal");
    mock_error = EAGAIN;
    require(outgoing_flush(123, NULL) == 0 && !outgoing.written,
            "EAGAIN retains complete transport queue");
    mock_error = 0;
    mock_limit = (size_t)first_end - 1;
    require(outgoing_flush(123, NULL) == 0 && reliable_input.eligible_sequence == 0,
            "partial plain DATA is ineligible");
    require(input_acknowledge(1) == -1 && reliable_input.bytes == 9,
            "partial plain frame cannot be falsely acknowledged");
    mock_limit = 1;
    require(outgoing_flush(123, NULL) == 0 && reliable_input.eligible_sequence == 1,
            "last byte makes exactly first frame eligible");
    require(input_acknowledge(1) == 0 && reliable_input.entries == 1 &&
            reliable_input.bytes == 4 && reliable_input.head->sequence == 2,
            "cumulative ACK frees only its accepted prefix");
    require(input_acknowledge(2) == -1, "unwritten suffix remains ineligible");
    flush_plain();
    require(input_acknowledge(2) == 0 && !reliable_input.head &&
            !reliable_input.tail && !reliable_input.entries && !reliable_input.bytes,
            "fully emitted/ACKed journal empties");
    require(input_acknowledge(1) == -1 && input_acknowledge(2) == 0,
            "ACK rollback rejected; repeated watermark harmless");
    check_captured_data(sequences, payloads, sizes, 2, 0);
}

static void
test_control_boundaries_and_compaction(void) {
    static char padding[40000];
    const uint64_t sequences[] = {1, 2};
    const char *payloads[] = {"one", "two"};
    const size_t sizes[] = {3, 3};
    uint64_t first_end, second_end, queued;
    fresh();
    memset(padding, 'c', sizeof padding);
    require(send_message(123, KMX_MSG_HELLO, padding, sizeof padding) == 0,
            "control prefix queued");
    require(input_send(123, "one", 3) == 0, "first DATA queued after control");
    first_end = reliable_input.head->wire_end;
    require(send_message(123, KMX_MSG_ACK, "ack", 3) == 0,
            "viewport ACK is independent control frame");
    require(input_send(123, "two", 3) == 0, "second DATA queued after ACK");
    second_end = reliable_input.tail->wire_end;
    mock_limit = (size_t)first_end - 1;
    require(outgoing_flush(123, NULL) == 0 && reliable_input.eligible_sequence == 0,
            "control bytes do not credit partial DATA");
    mock_limit = 1;
    require(outgoing_flush(123, NULL) == 0 && input_acknowledge(1) == 0,
            "first mixed frame credited at its own boundary");
    queued = outgoing.queued;
    require(send_message(123, KMX_MSG_RESIZE, "size", 4) == 0,
            "appending control compacts consumed transport prefix");
    require(!outgoing.offset && outgoing.queued > queued &&
            reliable_input.head->wire_end == second_end,
            "compaction preserves absolute DATA boundaries");
    mock_limit = (size_t)(second_end - outgoing.written) - 1;
    require(outgoing_flush(123, NULL) == 0 && input_acknowledge(2) == -1,
            "interleaved ACK and partial DATA do not credit second frame");
    mock_limit = 1;
    require(outgoing_flush(123, NULL) == 0 && input_acknowledge(2) == 0,
            "second boundary survives compaction");
    flush_plain();
    check_captured_data(sequences, payloads, sizes, 2, 3);
}

static void
test_loss_reconciliation_and_generation(void) {
    const char binary[] = "paste\033[200~line one\nline two\033[201~";
    const uint64_t sequences[] = {2};
    const char *payloads[] = {binary};
    const size_t sizes[] = {sizeof binary - 1};
    kmx_buffer saved;
    kmx_framer framer;
    kmx_input_open decoded;
    const unsigned char *payload;
    size_t size;
    kmx_message_type type;
    bool available;
    uint64_t generation;
    fresh();
    require(input_send(123, "accepted", 8) == 0, "first retained record queued");
    flush_plain();  /* Server may have accepted 1, ACK was lost. */
    require(input_send(123, binary, sizeof binary - 1) == 0, "second binary record queued");
    mock_limit = outgoing_pending() - 1;
    require(outgoing_flush(123, NULL) == 0 && reliable_input.eligible_sequence == 1,
            "second record interrupted mid-frame");
    kmx_buffer_init(&saved);
    require(kmx_buffer_append(&saved, reliable_input.tail->payload.data,
                             reliable_input.tail->payload.size) == KMX_OK,
            "retain expected encoded replay");
    generation = reliable_input.generation;
    reliable_input.ready = false;
    outgoing_discard("test loss");
    require(!outgoing.queued && !outgoing.written && !outgoing_pending(),
            "transport discarded separately from journal");
    require(input_open(123) == 0 && reliable_input.generation == generation + 1,
            "resume OPEN starts a new transport generation");
    kmx_buffer_reset(&captured);
    flush_plain();
    require(reliable_input.eligible_sequence == 1 && input_acknowledge(2) == -1,
            "new OPEN cannot credit stale wire offsets from old generation");
    kmx_framer_init(&framer);
    require(kmx_framer_push(&framer, captured.data, captured.size) == KMX_OK &&
            kmx_framer_next(&framer, &available, &type, &payload, &size) == KMX_OK &&
            available && type == KMX_MSG_INPUT_OPEN &&
            kmx_input_open_decode(payload, size, &decoded) == KMX_OK,
            "resume OPEN decoded from actual transport output");
    require(!memcmp(decoded.epoch, reliable_input.epoch, sizeof decoded.epoch) &&
            !memcmp(decoded.client_id, reliable_input.client_id, sizeof decoded.client_id) &&
            decoded.last_ack == 0,
            "lost ACK resumes same identity with only confirmed watermark");
    kmx_framer_free(&framer);
    require(input_acknowledge(1) == 0 && reliable_input.head->sequence == 2,
            "STATE ahead reconciles lost ACK without replaying accepted prefix");
    reliable_input.ready = true;
    reliable_input.replay = reliable_input.head;
    require(!input_has_room(), "new stdin waits behind ordered replay");
    require(input_replay(123) == 0 && !reliable_input.replay &&
            reliable_input.sequence == 2 &&
            reliable_input.head->generation == reliable_input.generation &&
            reliable_input.head->payload.size == saved.size &&
            !memcmp(reliable_input.head->payload.data, saved.data, saved.size),
            "replay retains exact original sequence and payload");
    kmx_buffer_reset(&captured);
    flush_plain();
    check_captured_data(sequences, payloads, sizes, 1, 0);
    require(input_acknowledge(2) == 0 && input_send(123, "next", 4) == 0 &&
            reliable_input.sequence == 3, "new input continues sequence after reconciliation");
    kmx_buffer_free(&saved);
}

static void
test_journal_limits(void) {
    static char block[KMX_INPUT_DATA_MAX];
    size_t bytes;
    fresh();
    memset(block, 'p', sizeof block);
    for (size_t i = 0; i < 7; i++) require(input_send(123, block, sizeof block) == 0,
                                        "fill byte journal below soft bound");
    require(input_send(123, block, sizeof block - 1) == 0 && input_has_room(),
            "one byte of soft headroom admits one bounded frame");
    require(input_send(123, block, sizeof block) == 0 && !input_has_room(),
            "one final 32KiB frame exhausts journal headroom");
    bytes = reliable_input.bytes;
    require(bytes == KMX_INPUT_JOURNAL_BYTES + KMX_INPUT_DATA_MAX - 1 &&
            input_send(123, "x", 1) == -1 && reliable_input.bytes == bytes,
            "journal bounded by soft bytes plus one frame");
    require(input_acknowledge(9) == -1, "memory allocation alone cannot relieve pressure");
    flush_plain();
    require(input_acknowledge(9) == 0 && input_has_room(),
            "validated acceptance restores headroom");

    fresh();
    for (size_t i = 0; i < KMX_INPUT_JOURNAL_ENTRIES; i++) {
        require(input_send(123, "x", 1) == 0, "small frame enters entry-bounded journal");
    }
    require(reliable_input.entries == KMX_INPUT_JOURNAL_ENTRIES &&
            reliable_input.bytes == KMX_INPUT_JOURNAL_ENTRIES && !input_has_room() &&
            input_send(123, "x", 1) == -1,
            "entry count prevents tiny-frame amplification");
    flush_plain();
    require(input_acknowledge(KMX_INPUT_JOURNAL_ENTRIES) == 0 && input_has_room(),
            "entry pressure clears only after valid ACK");
    reliable_input.ready = false;
    require(!input_has_room() && input_send(123, "x", 1) == -1,
            "OPEN selection gates all new input");
    reliable_input.ready = true;
    reliable_input.sequence = UINT64_MAX;
    require(input_send(123, "x", 1) == -1, "sequence never wraps");
    reliable_input.generation = UINT64_MAX;
    require(input_open(123) == -1, "transport generation never wraps");
}

static void
test_tls_offered_prefix_and_retry(void) {
    static char block[KMX_INPUT_DATA_MAX];
    const uint64_t sequences[] = {1, 2};
    const char *payloads[] = {block, block};
    const size_t sizes[] = {sizeof block, sizeof block};
    kmx_tls_session *tls = (kmx_tls_session *)(uintptr_t)1;
    fresh();
    memset(block, 't', sizeof block);
    require(input_send(123, block, sizeof block) == 0 &&
            input_send(123, block, sizeof block) == 0,
            "TLS test queues more than one write budget");
    require(reliable_input.head->wire_end < KMX_OUT_WRITE_BUDGET &&
            reliable_input.tail->wire_end > KMX_OUT_WRITE_BUDGET,
            "TLS offered buffer includes one full frame and partial suffix");
    mock_error = EAGAIN;
    mock_tls_read = true;
    require(outgoing_flush(123, tls) == 0 && outgoing.tls_wait_read &&
            outgoing.tls_attempt == KMX_OUT_WRITE_BUDGET && !outgoing.written,
            "TLS WANT_READ retains unchanged pending retry length");
    require(reliable_input.eligible_sequence == 1 && input_acknowledge(2) == -1,
            "TLS possible record progress credits full offered frames only");
    require(input_acknowledge(1) == 0 && reliable_input.entries == 1,
            "server can ACK full first frame while SSL_write reports WANT");
    require(send_message(123, KMX_MSG_ACK, "control", 7) == 0,
            "incoming control can append during TLS WANT");
    mock_error = 0;
    mock_tls_read = false;
    require(outgoing_flush(123, tls) == 0 && tls_calls == 2 &&
            tls_lengths[0] == tls_lengths[1] && tls_hashes[0] == tls_hashes[1],
            "appending controls does not alter TLS retry buffer or length");
    require(reliable_input.eligible_sequence == 1,
            "success of retry still excludes untouched second-frame tail");
    require(outgoing_flush(123, tls) == 0 && !outgoing_pending() &&
            input_acknowledge(2) == 0, "next TLS offered tail makes second frame eligible");
    check_captured_data(sequences, payloads, sizes, 2, 1);
}

static void
test_tls_moving_buffer_retry(void) {
    static char block[KMX_INPUT_DATA_MAX];
    kmx_tls_session *tls = (kmx_tls_session *)(uintptr_t)1;
    size_t pending;
    fresh();
    memset(block, 'm', sizeof block);
    for (size_t i = 0; i < 3; i++) require(input_send(123, block, sizeof block) == 0,
                                        "moving TLS retry test fills queue");
    require(outgoing_flush(123, tls) == 0 && outgoing.offset == KMX_OUT_WRITE_BUDGET,
            "first TLS budget consumes transport prefix");
    pending = outgoing_pending();
    mock_error = EAGAIN;
    require(outgoing_flush(123, tls) == 0 && outgoing.tls_attempt == pending,
            "remaining TLS prefix suspended");
    require(send_message(123, KMX_MSG_RESIZE, "size", 4) == 0 && !outgoing.offset,
            "control append moves suspended retry buffer");
    mock_error = 0;
    require(outgoing_flush(123, tls) == 0 && tls_calls == 3 &&
            tls_lengths[1] == tls_lengths[2] && tls_hashes[1] == tls_hashes[2],
            "moving address preserves TLS attempt bytes and length");
    require(input_acknowledge(3) == 0, "all wholly offered TLS frames remain resumable/ACKable");
}

int
main(void) {
    kmx_buffer_init(&captured);
    kmx_buffer_init(&tls_first_offer);
    test_plain_ack_ceiling();
    test_control_boundaries_and_compaction();
    test_loss_reconciliation_and_generation();
    test_journal_limits();
    test_tls_offered_prefix_and_retry();
    test_tls_moving_buffer_retry();
    fresh();
    kmx_buffer_free(&outgoing.bytes);
    kmx_buffer_free(&captured);
    kmx_buffer_free(&tls_first_offer);
    puts("PASS actual attach journal: partial/EAGAIN, ACK ceilings, mixed frames, loss/replay, bounded bytes/entries, TLS WANT/retry");
    return 0;
}
