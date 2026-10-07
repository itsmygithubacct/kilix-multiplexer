/* kmx-attach — render a remote session locally and type into it.
 *
 * The client receives each pane's screen plus a few dozen bytes saying where
 * the panes are, and draws the dividers and title bars itself.  That chrome
 * costs nothing on the wire, which is the whole argument for a layout plane:
 * a pixel protocol would re-encode those borders every frame.
 *
 *   kmx-attach --socket PATH [--no-predict] [--dump] [--send TEXT]
 *              [--seconds N]
 *
 * Ctrl-] detaches, Ctrl-O moves focus to the next pane.  --dump renders to
 * stdout without taking over the terminal, which is what makes the client
 * testable without a TTY. */
#define _GNU_SOURCE

#include "kilix_mux.h"
#include "kilix_mux_input.h"
#include "kmx_random.h"
#include "kilix_mux_modes.h"
#include "endpoint.h"
#include "kmx_input_transform.h"
#include "kmx_tls.h"
#include "kmx_encodec.h"
#include "kmx_read_clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static volatile sig_atomic_t resize_pending;
static volatile sig_atomic_t stop_pending;
extern char **environ;

static uint64_t now_millis(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void
handle_resize(int signal_number) {
    (void)signal_number;
    resize_pending = 1;
}

static void
handle_stop(int signal_number) {
    (void)signal_number;
    stop_pending = 1;
}

/* Stop reading typing before the queue gets deep, while retaining room for
 * one complete input frame and control traffic produced by incoming updates. */
#define KMX_OUT_SOFT_LIMIT (256u * 1024u)
#define KMX_OUT_CONTROL_RESERVE (64u * 1024u)
#define KMX_OUT_HARD_LIMIT \
    ((size_t)KMX_OUT_SOFT_LIMIT + KMX_MESSAGE_MAX + KMX_OUT_CONTROL_RESERVE)
#define KMX_OUT_WRITE_BUDGET (64u * 1024u)

static struct {
    kmx_buffer bytes;
    size_t offset;
    size_t tls_attempt;
    bool tls_wait_read;
    bool input_submitted;
    bool failed;
    uint64_t queued;
    uint64_t written;
} outgoing;

#define KMX_INPUT_JOURNAL_BYTES (256u * 1024u)
#define KMX_INPUT_JOURNAL_ENTRIES 4096u
#define KMX_INPUT_SELECT_MS 5000u
typedef struct input_entry {
    struct input_entry *next;
    uint64_t sequence;
    uint64_t generation;
    uint64_t wire_end;
    kmx_buffer payload;
} input_entry;

static struct {
    bool enabled;
    bool ready;
    bool identified;
    unsigned char epoch[KMX_INPUT_TOKEN_SIZE];
    unsigned char client_id[KMX_INPUT_TOKEN_SIZE];
    uint64_t acknowledged;
    uint64_t sequence;
    uint64_t eligible_sequence;
    uint64_t generation;
    uint64_t deadline;
    size_t bytes;
    size_t entries;
    input_entry *head;
    input_entry *tail;
    input_entry *replay;
} reliable_input;

static void
input_emitted_through(uint64_t through) {
    for (input_entry *entry = reliable_input.head; entry; entry = entry->next) {
        if (entry->sequence <= reliable_input.eligible_sequence) continue;
        if (entry->generation != reliable_input.generation || entry->wire_end > through) break;
        reliable_input.eligible_sequence = entry->sequence;
    }
}

static size_t
outgoing_pending(void) {
    return outgoing.bytes.size - outgoing.offset;
}

static int
outgoing_flush(int fd, kmx_tls_session *tls) {
    size_t pending = outgoing_pending();
    size_t attempt;
    long count;
    if (!pending) return 0;
    /* OpenSSL permits a moving address, but a WANT retry must retain the
     * original byte count even if additional complete frames were appended. */
    attempt = outgoing.tls_attempt ? outgoing.tls_attempt :
        (pending < KMX_OUT_WRITE_BUDGET ? pending : KMX_OUT_WRITE_BUDGET);
    if (tls) outgoing.tls_attempt = attempt;
    /* SSL_write can report WANT after emitting complete TLS records. A frame
     * wholly inside that attempt may already be accepted remotely even if
     * the call later fails. Plain send reports its exact completed prefix. */
    if (tls) input_emitted_through(outgoing.written + attempt);
    count = tls ? kmx_tls_write(tls, outgoing.bytes.data + outgoing.offset, attempt)
                : send(fd, outgoing.bytes.data + outgoing.offset, attempt,
                       MSG_NOSIGNAL | MSG_DONTWAIT);
    outgoing.tls_wait_read = tls && kmx_tls_write_wants_read(tls);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (!count || (size_t)count > attempt) return -1;
    outgoing.offset += (size_t)count;
    outgoing.written += (size_t)count;
    if (!tls) input_emitted_through(outgoing.written);
    outgoing.tls_attempt = 0;
    if (outgoing.offset == outgoing.bytes.size) {
        outgoing.offset = 0;
        outgoing.bytes.size = 0;
    }
    return 0;
}

static void
outgoing_discard(const char *reason) {
    size_t pending = outgoing_pending();
    if (!reliable_input.enabled && (pending || outgoing.input_submitted)) {
        fprintf(stderr,
            "kmx-attach: %s; discarded %zu pending transport bytes; "
            "prior input delivery is unconfirmed\n", reason, pending);
    }
    outgoing.offset = 0;
    outgoing.bytes.size = 0;
    outgoing.tls_attempt = 0;
    outgoing.tls_wait_read = false;
    outgoing.input_submitted = 0;
    outgoing.queued = outgoing.written = 0;
}

static int
write_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count;
        count = write(fd, cursor + done, size - done);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

static int
send_message(int fd, kmx_message_type type, const void *payload, size_t size) {
    kmx_buffer framed;
    kmx_result result;
    (void)fd;
    if (outgoing.failed) return -1;
    kmx_buffer_init(&framed);
    result = kmx_frame_encode(type, payload, size, &framed);
    if (outgoing.offset &&
        (outgoing.offset >= outgoing.bytes.size / 2 ||
         outgoing.offset >= KMX_OUT_WRITE_BUDGET)) {
        memmove(outgoing.bytes.data, outgoing.bytes.data + outgoing.offset,
                outgoing_pending());
        outgoing.bytes.size -= outgoing.offset;
        outgoing.offset = 0;
    }
    if (result == KMX_OK &&
        (outgoing.bytes.size > KMX_OUT_HARD_LIMIT ||
         framed.size > KMX_OUT_HARD_LIMIT - outgoing.bytes.size ||
         framed.size > UINT64_MAX - outgoing.queued)) result = KMX_ERR_LIMIT;
    if (result == KMX_OK) result = kmx_buffer_append(&outgoing.bytes, framed.data, framed.size);
    if (result == KMX_OK) outgoing.queued += framed.size;
    kmx_buffer_free(&framed);
    if (result != KMX_OK) {
        fprintf(stderr, "kmx-attach: outgoing frame queue exhausted or failed\n");
        outgoing.failed = true;
        stop_pending = 1;
        return -1;
    }
    if (type == KMX_MSG_INPUT && size) outgoing.input_submitted = true;
    return 0;
}

static bool
input_has_room(void) {
    return !reliable_input.enabled ||
        (reliable_input.ready && !reliable_input.replay &&
         reliable_input.bytes < KMX_INPUT_JOURNAL_BYTES &&
         reliable_input.entries < KMX_INPUT_JOURNAL_ENTRIES);
}

static int
input_open(int fd) {
    kmx_input_open request = {0};
    kmx_buffer payload;
    int result;
    if (!reliable_input.enabled) return 0;
    if (reliable_input.generation == UINT64_MAX) return -1;
    reliable_input.generation++;
    memcpy(request.epoch, reliable_input.epoch, sizeof request.epoch);
    memcpy(request.client_id, reliable_input.client_id, sizeof request.client_id);
    request.last_ack = reliable_input.acknowledged;
    reliable_input.ready = false;
    reliable_input.deadline = now_millis() + KMX_INPUT_SELECT_MS;
    kmx_buffer_init(&payload);
    result = kmx_input_open_encode(&request, &payload) != KMX_OK ||
        send_message(fd, KMX_MSG_INPUT_OPEN, payload.data, payload.size);
    kmx_buffer_free(&payload);
    return result ? -1 : 0;
}

static int
input_acknowledge(uint64_t accepted) {
    if (accepted < reliable_input.acknowledged || accepted > reliable_input.eligible_sequence) return -1;
    while (reliable_input.head && reliable_input.head->sequence <= accepted) {
        input_entry *entry = reliable_input.head;
        reliable_input.head = entry->next;
        if (reliable_input.replay == entry) reliable_input.replay = entry->next;
        reliable_input.bytes -= entry->payload.size - KMX_INPUT_DATA_HEADER_SIZE;
        reliable_input.entries--;
        kmx_buffer_free(&entry->payload);
        free(entry);
    }
    if (!reliable_input.head) reliable_input.tail = NULL;
    reliable_input.acknowledged = accepted;
    return 0;
}

static int
input_replay(int fd) {
    while (reliable_input.ready && reliable_input.replay &&
           outgoing_pending() < KMX_OUT_SOFT_LIMIT) {
        input_entry *entry = reliable_input.replay;
        if (send_message(fd, KMX_MSG_INPUT_DATA, entry->payload.data, entry->payload.size)) return -1;
        entry->generation = reliable_input.generation;
        entry->wire_end = outgoing.queued;
        reliable_input.replay = entry->next;
    }
    return 0;
}

static int
input_send(int fd, const void *data, size_t size) {
    input_entry *entry;
    kmx_input_data input;
    if (!reliable_input.enabled) return send_message(fd, KMX_MSG_INPUT, data, size);
    if (!size) return 0;
    if (!input_has_room() || size > KMX_INPUT_DATA_MAX || reliable_input.sequence == UINT64_MAX) return -1;
    entry = calloc(1, sizeof *entry);
    if (!entry) return -1;
    input = (kmx_input_data){ .pane = 0, .sequence = reliable_input.sequence + 1,
        .data = data, .size = size };
    kmx_buffer_init(&entry->payload);
    if (kmx_input_data_encode(&input, &entry->payload) != KMX_OK) {
        kmx_buffer_free(&entry->payload);
        free(entry);
        return -1;
    }
    entry->sequence = input.sequence;
    if (reliable_input.tail) reliable_input.tail->next = entry;
    else reliable_input.head = entry;
    reliable_input.tail = entry;
    reliable_input.bytes += size;
    reliable_input.entries++;
    reliable_input.sequence = input.sequence;
    if (send_message(fd, KMX_MSG_INPUT_DATA, entry->payload.data, entry->payload.size)) return -1;
    entry->generation = reliable_input.generation;
    entry->wire_end = outgoing.queued;
    return 0;
}

/* Release only a fully acknowledged journal. If this best-effort close cannot
 * reach the server, its finite disconnect grace still bounds the lease. */
static void
input_close(int fd, kmx_tls_session *tls) {
    kmx_input_ack close_request = { .accepted = reliable_input.acknowledged };
    kmx_buffer payload;
    uint64_t deadline;
    if (!reliable_input.enabled || !reliable_input.ready || reliable_input.head ||
        outgoing.failed || fd < 0) return;
    kmx_buffer_init(&payload);
    if (kmx_input_ack_encode(&close_request, &payload) == KMX_OK &&
        send_message(fd, KMX_MSG_INPUT_CLOSE, payload.data, payload.size) == 0) {
        deadline = now_millis() + 100;
        while (outgoing_pending() && now_millis() < deadline) {
            struct pollfd descriptor = { .fd = fd,
                .events = outgoing.tls_wait_read ? POLLIN : POLLOUT };
            if (poll(&descriptor, 1, 10) > 0 && outgoing_flush(fd, tls)) break;
        }
    }
    kmx_buffer_free(&payload);
}

static void
input_free(void) {
    while (reliable_input.head) {
        input_entry *entry = reliable_input.head;
        reliable_input.head = entry->next;
        kmx_buffer_free(&entry->payload);
        free(entry);
    }
}

static void
get_size(int *rows, int *cols) {
    struct winsize size;
    memset(&size, 0, sizeof size);
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col) {
        *rows = size.ws_row;
        *cols = size.ws_col;
        return;
    }
    *rows = 24;
    *cols = 80;
}

static void
get_pixel_size(int *width, int *height) {
    struct winsize size;
    memset(&size, 0, sizeof size);
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 &&
        size.ws_xpixel && size.ws_ypixel) {
        *width = size.ws_xpixel;
        *height = size.ws_ypixel;
        return;
    }
    *width = 0;
    *height = 0;
}

static int
send_dimensions(int fd, kmx_message_type type, int rows, int cols) {
    unsigned char payload[4];
    payload[0] = (unsigned char)(rows >> 8);
    payload[1] = (unsigned char)rows;
    payload[2] = (unsigned char)(cols >> 8);
    payload[3] = (unsigned char)cols;
    return send_message(fd, type, payload, sizeof payload);
}

/* The role is declared in HELLO.  The server enforces it; this only says
 * which one is being asked for. */
static int
send_hello(int fd, int rows, int cols, bool view_only, const char *token) {
    unsigned char payload[5 + 64];
    size_t size = 5;
    payload[0] = (unsigned char)(rows >> 8);
    payload[1] = (unsigned char)rows;
    payload[2] = (unsigned char)(cols >> 8);
    payload[3] = (unsigned char)cols;
    payload[4] = view_only ? 1u : 0u;
    if (token) {
        size_t length = strlen(token);
        if (length > sizeof payload - 5) return -1;
        memcpy(payload + 5, token, length);
        size += length;
    }
    return send_message(fd, KMX_MSG_HELLO, payload, size);
}

static int send_audio_offer(int fd, const kmx_audio_caps *caps) {
    unsigned char payload[KMX_AUDIO_CAPS_BYTES];
    kmx_audio_caps_write(payload, caps);
    return send_message(fd, KMX_MSG_AUDIO_CAPS, payload, sizeof payload);
}

static int send_audio_profile_offer(int fd, uint32_t profiles) {
    unsigned char payload[KMX_AUDIO_PROFILE_BYTES];
    kmx_audio_profile offer = {0, profiles};
    if (!profiles) return 0;
    kmx_audio_profile_write(payload, &offer);
    return send_message(fd, KMX_MSG_AUDIO_PROFILE, payload, sizeof payload);
}

/* The loop polls this descriptor, so it has to be non-blocking.
 *
 * On the plain path a blocking socket is harmless - read() after POLLIN returns
 * whatever arrived.  Under TLS it is not: POLLIN says some bytes arrived, while
 * SSL_read cannot return until a whole record has, so it blocks inside a loop
 * that assumes it will not.  Measured against a server that wrote three bytes
 * of a five-byte record header and stopped: --seconds was ignored, Ctrl-] was
 * ignored, and the process had to be SIGKILLed.
 *
 * Called AFTER the TLS handshake, never before.  SSL_connect on a non-blocking
 * socket returns WANT_READ immediately and this client treats that as failure,
 * so setting the flag first turns every TLS attach into an instant silent
 * exit - which is what the first version of this fix did.  The handshake is
 * left blocking under a receive timeout: the client has nothing else to do at
 * that point, and a hang there is in front of whoever ran it. */
static void
make_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Reattach after the link drops.
 *
 * Worth doing rather than exiting because the session is not on this side:
 * the panes keep running, and their screens are state the server can describe
 * again from scratch.  A reattached client therefore needs no history - it is
 * simply sent the screen as it is now, which is the same message a first-time
 * client gets and is why reconnection is cheap here. */
static int
reconnect(const kmx_endpoint *endpoint, int seconds) {
    time_t deadline = time(NULL) + seconds;
    while (time(NULL) <= deadline) {
        int fd = kmx_endpoint_connect(endpoint);
        if (fd >= 0) {
            kmx_endpoint_tune(fd, endpoint);
            return fd;
        }
        usleep(400000);
    }
    return -1;
}

#define KMX_REMOTE_IMAGE_ID 2147483000u
#define KMX_GRAPHICS_CHUNK 4096u

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned char *
base64_encode(const unsigned char *data, size_t size, size_t *encoded_size) {
    unsigned char *out;
    size_t length;
    size_t input = 0;
    size_t output = 0;
    if (!data || !encoded_size || size > (SIZE_MAX - 2u) / 3u) return NULL;
    length = ((size + 2u) / 3u) * 4u;
    out = malloc(length ? length : 1);
    if (!out) return NULL;
    while (input + 3u <= size) {
        uint32_t value =
            (uint32_t)data[input] << 16 |
            (uint32_t)data[input + 1] << 8 |
            data[input + 2];
        out[output++] = (unsigned char)base64_table[(value >> 18) & 63u];
        out[output++] = (unsigned char)base64_table[(value >> 12) & 63u];
        out[output++] = (unsigned char)base64_table[(value >> 6) & 63u];
        out[output++] = (unsigned char)base64_table[value & 63u];
        input += 3;
    }
    if (input < size) {
        uint32_t value = (uint32_t)data[input] << 16;
        bool have_second = input + 1 < size;
        if (have_second) value |= (uint32_t)data[input + 1] << 8;
        out[output++] = (unsigned char)base64_table[(value >> 18) & 63u];
        out[output++] = (unsigned char)base64_table[(value >> 12) & 63u];
        out[output++] = have_second
            ? (unsigned char)base64_table[(value >> 6) & 63u] : '=';
        out[output++] = '=';
    }
    *encoded_size = output;
    return out;
}

/* Present a decoded motion frame through the local Kilix graphics protocol.
 * Inline zlib is used because the pixels came from another machine: a local
 * shared-memory name would exist on the wrong host. */
static int
display_motion(
    const unsigned char *rgb,
    int width,
    int height,
    int columns,
    int rows
) {
    unsigned char *compressed = NULL;
    unsigned char *encoded = NULL;
    uLongf compressed_size;
    size_t encoded_size = 0;
    size_t offset = 0;
    bool first = true;
    int result = -1;

    if (!rgb || width <= 0 || height <= 0 || columns <= 0 || rows <= 0) return -1;
    compressed_size = compressBound((uLong)((size_t)width * (size_t)height * 3u));
    compressed = malloc((size_t)compressed_size);
    if (!compressed) goto done;
    if (compress2(
            compressed, &compressed_size, rgb,
            (uLong)((size_t)width * (size_t)height * 3u),
            Z_BEST_SPEED) != Z_OK) {
        goto done;
    }
    encoded = base64_encode(compressed, (size_t)compressed_size, &encoded_size);
    if (!encoded) goto done;
    if (write_all(
            STDOUT_FILENO, "\033[?2026h\033[H",
            sizeof "\033[?2026h\033[H" - 1) != 0) {
        goto done;
    }
    while (offset < encoded_size) {
        char control[256];
        size_t chunk = encoded_size - offset;
        int control_size;
        int more;
        if (chunk > KMX_GRAPHICS_CHUNK) chunk = KMX_GRAPHICS_CHUNK;
        more = offset + chunk < encoded_size;
        if (first) {
            control_size = snprintf(
                control, sizeof control,
                "\033_Ga=T,i=%u,p=1,z=-1,t=d,f=24,o=z,N=1,"
                "s=%d,v=%d,c=%d,r=%d,q=2,C=1,m=%d;",
                KMX_REMOTE_IMAGE_ID, width, height, columns, rows, more);
        } else {
            control_size = snprintf(
                control, sizeof control, "\033_Gm=%d;", more);
        }
        if (control_size < 0 || (size_t)control_size >= sizeof control ||
            write_all(STDOUT_FILENO, control, (size_t)control_size) != 0 ||
            write_all(STDOUT_FILENO, encoded + offset, chunk) != 0 ||
            write_all(STDOUT_FILENO, "\033\\", 2) != 0) {
            goto done;
        }
        first = false;
        offset += chunk;
    }
    if (write_all(
            STDOUT_FILENO, "\033[?2026l", sizeof "\033[?2026l" - 1) != 0) {
        goto done;
    }
    result = 0;
done:
    free(encoded);
    free(compressed);
    return result;
}

static int
enable_pixel_input(void) {
    static const char controls[] =
        "\033[?25l\033[>15u"
        "\033[?1003h\033[?1006h\033[?1016h\033[?2004h";
    return write_all(STDOUT_FILENO, controls, sizeof controls - 1);
}

static void
disable_pixel_input(void) {
    static const char controls[] =
        "\033[<u"
        "\033[?1003l\033[?1006l\033[?1016l\033[?2004l"
        "\033_Ga=d,d=A\033\\\033[?25h";
    (void)write_all(STDOUT_FILENO, controls, sizeof controls - 1);
}

static void
reset_terminal_input(void) {
    static const char controls[] =
        "\033[?1l\033[?2004l\033[?1004l"
        "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25h";
    (void)write_all(STDOUT_FILENO, controls, sizeof controls - 1);
}

static int
apply_terminal_input(uint32_t flags) {
    static const struct { uint32_t bit; unsigned int selector; } modes[] = {
        {KMX_MODE_APPLICATION_CURSOR, 1}, {KMX_MODE_BRACKETED_PASTE, 2004},
        {KMX_MODE_FOCUS_REPORT, 1004}, {KMX_MODE_MOUSE_CLICK, 1000},
        {KMX_MODE_MOUSE_DRAG, 1002}, {KMX_MODE_MOUSE_MOVE, 1003},
        {KMX_MODE_MOUSE_SGR, 1006},
    };
    char controls[160];
    size_t used = 0;
    /* Clear tracking selectors first: a later reset of another selector
     * would otherwise disable the requested one on libvterm terminals. */
    for (unsigned int enabled = 0; enabled <= 1; enabled++) {
        for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            int length;
            if (((flags & modes[i].bit) != 0) != (enabled != 0)) continue;
            length = snprintf(controls + used, sizeof controls - used,
                              "\033[?%u%c", modes[i].selector, enabled ? 'h' : 'l');
            if (length < 0 || (size_t)length >= sizeof controls - used) return -1;
            used += (size_t)length;
        }
    }
    return write_all(STDOUT_FILENO, controls, used);
}

typedef struct {
    int fd;
    pid_t child;
    unsigned char *pending;
    size_t pending_size;
    size_t pending_offset;
    size_t dropped;
    bool attempted;
    bool disabled;
} audio_output;

static bool
command_exists(const char *name) {
    const char *path = getenv("PATH");
    const char *cursor;
    if (!name || !*name || strchr(name, '/')) return name && access(name, X_OK) == 0;
    if (!path) return false;
    cursor = path;
    while (true) {
        const char *end = strchr(cursor, ':');
        size_t directory_size = end ? (size_t)(end - cursor) : strlen(cursor);
        char candidate[4096];
        int length;
        if (directory_size == 0) {
            length = snprintf(candidate, sizeof candidate, "./%s", name);
        } else {
            length = snprintf(
                candidate, sizeof candidate, "%.*s/%s",
                (int)directory_size, cursor, name);
        }
        if (length > 0 && (size_t)length < sizeof candidate &&
            access(candidate, X_OK) == 0) {
            return true;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return false;
}

static void
audio_output_start(
    audio_output *output,
    const char *command,
    uint32_t sample_rate,
    uint8_t channels,
    bool dump
) {
    int pipe_fds[2];
    pid_t child;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    char rate[48], channel_count[48];
    char **environment;
    size_t count = 0, copied = 0, index;
    int status, null_fd;
    const char *selected;
    char *arguments[4];
    if (!output || output->attempted) return;
    output->attempted = true;
    if ((command && strcmp(command, "none") == 0) || (!command && dump)) {
        output->disabled = true;
        return;
    }
    if (!command && !command_exists("pacat") && !command_exists("aplay")) {
        fprintf(stderr,
            "kmx-attach: no pacat or aplay found; audio is not being played\n");
        output->disabled = true;
        return;
    }
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        output->disabled = true;
        return;
    }
    /* Inference owns threads. Use spawn file actions instead of running
     * allocator/environment/shell setup inside a post-fork child. Only stdio
     * reaches the sink; model, queue and authenticated socket FDs stay private. */
    while (environ[count]) count++;
    environment = calloc(count + 3u, sizeof *environment);
    null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (!environment || null_fd < 0) {
        free(environment); if (null_fd >= 0) close(null_fd);
        close(pipe_fds[0]); close(pipe_fds[1]); output->disabled = true; return;
    }
    for (index = 0; index < count; index++)
        if (strncmp(environ[index], "KMX_AUDIO_RATE=", 15) && strncmp(environ[index], "KMX_AUDIO_CHANNELS=", 19))
            environment[copied++] = environ[index];
    snprintf(rate, sizeof rate, "KMX_AUDIO_RATE=%u", sample_rate);
    snprintf(channel_count, sizeof channel_count, "KMX_AUDIO_CHANNELS=%u", channels);
    environment[copied++] = rate; environment[copied++] = channel_count;
    selected = command ? command : command_exists("pacat") ?
        "exec pacat --playback --raw --format=s16le --rate \"$KMX_AUDIO_RATE\" --channels \"$KMX_AUDIO_CHANNELS\" --latency-msec=100" :
        "exec aplay -q -t raw -f S16_LE -r \"$KMX_AUDIO_RATE\" -c \"$KMX_AUDIO_CHANNELS\"";
    arguments[0] = (char *)"sh"; arguments[1] = (char *)"-c";
    arguments[2] = (char *)selected; arguments[3] = NULL;
    status = posix_spawn_file_actions_init(&actions);
    if (!status) {
        status = posix_spawnattr_init(&attributes);
        if (!status) {
            status = posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], STDIN_FILENO);
            if (!status) status = posix_spawn_file_actions_adddup2(&actions, null_fd, STDOUT_FILENO);
            if (!status) status = posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO);
            if (!status) status = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
            if (!status) status = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
            if (!status) status = posix_spawnattr_setpgroup(&attributes, 0);
            if (!status) status = posix_spawn(&child, "/bin/sh", &actions, &attributes, arguments, environment);
            posix_spawnattr_destroy(&attributes);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    free(environment); close(null_fd);
    if (status) { close(pipe_fds[0]); close(pipe_fds[1]); output->disabled = true; return; }
    close(pipe_fds[0]);
    output->fd = pipe_fds[1];
    output->child = child;
    make_non_blocking(output->fd);
}

static void
audio_output_flush(audio_output *output) {
    while (output && output->fd >= 0 &&
           output->pending_offset < output->pending_size) {
        ssize_t count = write(
            output->fd,
            output->pending + output->pending_offset,
            output->pending_size - output->pending_offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close(output->fd);
            output->fd = -1;
            break;
        }
        if (count == 0) return;
        output->pending_offset += (size_t)count;
    }
    if (output && output->pending_offset == output->pending_size) {
        free(output->pending);
        output->pending = NULL;
        output->pending_size = 0;
        output->pending_offset = 0;
    }
}

static void
audio_output_offer(audio_output *output, const unsigned char *pcm, size_t size) {
    if (!output || output->fd < 0 || !pcm || !size) return;
    audio_output_flush(output);
    if (output->pending) {
        output->dropped++;
        return;
    }
    output->pending = malloc(size);
    if (!output->pending) {
        output->dropped++;
        return;
    }
    memcpy(output->pending, pcm, size);
    output->pending_size = size;
    audio_output_flush(output);
}

static void
audio_output_stop(audio_output *output) {
    if (!output) return;
    audio_output_flush(output);
    free(output->pending);
    output->pending = NULL;
    if (output->fd >= 0) close(output->fd);
    output->fd = -1;
    if (output->child > 0) {
        int attempt, phase;
        siginfo_t state = {0};
        /* Keep the leader unreaped until its process group is signaled. Its
         * PID then cannot be reused as another process group's identity. */
        for (phase = 0; phase < 2 && !state.si_pid; phase++) {
            if (phase) kill(-output->child, SIGTERM);
            for (attempt = 0; attempt < 20; attempt++) {
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
                if (waitid(P_PID, (id_t)output->child, &state, WEXITED | WNOHANG | WNOWAIT) == 0 && state.si_pid) break;
                nanosleep(&pause, NULL);
            }
        }
        kill(-output->child, SIGKILL);
        while (waitpid(output->child, NULL, 0) < 0 && errno == EINTR) {}
        output->child = -1;
    }
}

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *send_text = NULL;
    bool predict = true;
    bool dump = false;
    bool view_only = false;
    bool pixel_input = false;
    const char *token = NULL;
    const char *fingerprint = NULL;
    const char *audio_output_command = NULL;
    kmx_audio_mode audio_mode = KMX_AUDIO_AUTO;
    unsigned audio_bitrate = 6;
    unsigned audio_threads = 2;
    const char *development_audio_assets = NULL;
    kmx_encodec *audio_codec = NULL;
    kmx_encodec *audio_codecs[2] = {NULL, NULL};
    uint32_t audio_profiles = 0, audio_profile = 0;
    bool audio_profile_selected = false;
    kmx_audio_caps audio_offer = {0}, audio_selection = {0};
    bool audio_selected = false, audio_encodec = false;
    bool audio_backpressure = false;
    kmx_read_clock read_clock = {0};
    uint64_t audio_select_deadline = 0;
    kmx_tls_client *tls_client = NULL;
    kmx_tls_session *tls = NULL;
    int reconnect_seconds = 30;
    bool pane_ended = false;
    int run_seconds = 0;
    time_t started_at;
    bool sent_once = false;
    size_t send_offset = 0;
    int index = 1;
    int fd;
    kmx_endpoint endpoint;
    struct termios saved;
    struct termios raw;
    bool have_termios = false;
    kmx_receiver *receivers[KMX_MAX_PANES];
    kmx_grid panes[KMX_MAX_PANES];
    kmx_layout layout;
    kmx_render *render = NULL;
    kmx_predictor *predictor = NULL;
    kmx_image_cache *images = NULL;
    kmx_motion_sink *motion = NULL;
    kmx_audio_sink *audio = NULL;
    unsigned long frames_seen = 0;
    unsigned long blocks_seen = 0;
    unsigned long preselection_audio_discarded = 0;
    audio_output player;
    kmx_framer framer;
    kmx_grid screen;
    unsigned char buffer[65536];
    size_t focus_hint = 0;
    int rows;
    int cols;
    int local_pixel_width = 0;
    int local_pixel_height = 0;
    int remote_pixel_width = 0;
    int remote_pixel_height = 0;
    bool pixel_input_enabled = false;
    uint32_t terminal_input_flags = 0;
    bool tls_read_wants_write = false;
    kmx_buffer pixel_input_pending;
    kmx_buffer pixel_input_output;
    int exit_code = 0;

    memset(receivers, 0, sizeof receivers);
    memset(panes, 0, sizeof panes);
    memset(&player, 0, sizeof player);
    player.fd = -1;
    player.child = -1;
    kmx_buffer_init(&pixel_input_pending);
    kmx_buffer_init(&pixel_input_output);

    /* Answered before anything else is parsed, so it works regardless of
     * whether the rest of the command line is right. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("kmx-attach %s\n", KMX_VERSION);
        return 0;
    }

    while (index < argc) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--no-predict") == 0) {
            predict = false;
        } else if (strcmp(argv[index], "--reconnect") == 0 && index + 1 < argc) {
            reconnect_seconds = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--reliable-input") == 0) {
            reliable_input.enabled = true;
        } else if (strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token = argv[++index];
        } else if (strcmp(argv[index], "--tls-fingerprint") == 0 && index + 1 < argc) {
            fingerprint = argv[++index];
        } else if (strcmp(argv[index], "--view") == 0) {
            view_only = true;
            predict = false;
        } else if (strcmp(argv[index], "--pixel-input") == 0) {
            pixel_input = true;
            predict = false;
        } else if (strcmp(argv[index], "--dump") == 0) {
            dump = true;
        } else if (strcmp(argv[index], "--send") == 0 && index + 1 < argc) {
            send_text = argv[++index];
        } else if (strcmp(argv[index], "--seconds") == 0 && index + 1 < argc) {
            run_seconds = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--audio-output") == 0 &&
                   index + 1 < argc) {
            audio_output_command = argv[++index];
        } else if (strcmp(argv[index], "--no-audio") == 0) {
            audio_output_command = "none";
        } else if (strcmp(argv[index], "--audio-codec") == 0 && index + 1 < argc) {
            if (kmx_audio_mode_parse(argv[++index], &audio_mode)) return 2;
        } else if (strcmp(argv[index], "--audio-bitrate") == 0 && index + 1 < argc) {
            if (kmx_audio_bitrate_parse(argv[++index], &audio_bitrate)) return 2;
        } else if (strcmp(argv[index], "--audio-threads") == 0 && index + 1 < argc) {
            if (kmx_audio_threads_parse(argv[++index], &audio_threads)) return 2;
        } else if (strcmp(argv[index], "--development-encodec-assets") == 0 && index + 1 < argc) {
            development_audio_assets = argv[++index];
        } else {
            fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict]"
                            " [--view] [--token TOKEN]\n"
                            "       [--tls-fingerprint HEX] [--reconnect N]"
                            "       [--dump] [--send TEXT] [--seconds N]\n"
                            "       [--audio-output COMMAND|--no-audio]"
                            " [--audio-codec auto|encodec|pcm] [--audio-bitrate 3|6|12]"
                            " [--audio-threads 2|4]"
                            " [--pixel-input] [--reliable-input]\n");
            return 2;
        }
        index++;
    }
    if (!socket_path) {
        fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict]"
                        " [--dump] [--send TEXT] [--seconds N]\n");
        return 2;
    }
    if (reliable_input.enabled && (view_only || pixel_input)) {
        fprintf(stderr, "kmx-attach: --reliable-input requires a text controller\n");
        return 2;
    }
    if (reliable_input.enabled &&
        kmx_random_bytes(reliable_input.client_id, sizeof reliable_input.client_id)) {
        fprintf(stderr, "kmx-attach: cannot generate input identity\n");
        return 1;
    }

    get_size(&rows, &cols);
    get_pixel_size(&local_pixel_width, &local_pixel_height);
    if (!kmx_endpoint_parse(socket_path, &endpoint)) {
        fprintf(stderr, "kmx-attach: cannot make sense of '%s'\n", socket_path);
        return 2;
    }
    if (audio_mode != KMX_AUDIO_PCM) {
        if (development_audio_assets) fprintf(stderr, "kmx-attach: DEVELOPMENT graph path; no installed admission\n");
        audio_codec = kmx_encodec_open_with_threads(false, audio_bitrate, 24000, 1,
                                      getenv("KILIX_CONTENT_ROOT"), development_audio_assets, audio_threads);
        audio_codecs[0] = audio_codec;
        if (audio_codec) {
            audio_profiles = KMX_AUDIO_PROFILE_BIT(KMX_AUDIO_PROFILE_C0);
            audio_codecs[1] = kmx_encodec_open_profile(false, audio_bitrate, 24000, 1,
                                      getenv("KILIX_CONTENT_ROOT"), development_audio_assets, audio_threads, 1);
            if (audio_codecs[1]) audio_profiles |= KMX_AUDIO_PROFILE_BIT(KMX_AUDIO_PROFILE_C5_R4);
        }
        if (!audio_codec) {
            fprintf(stderr, "kmx-attach: EnCodec unavailable; %s\n",
                audio_mode == KMX_AUDIO_ENCODEC ? "explicit selection refused" : "PCM fallback selected");
            if (audio_mode == KMX_AUDIO_ENCODEC) return 1;
        }
    }

    fd = kmx_endpoint_connect(&endpoint);
    if (fd < 0) {
        fprintf(stderr, "kmx-attach: connect: %s\n", strerror(errno));
        for (unsigned profile = 0; profile < 2; profile++) kmx_encodec_close(audio_codecs[profile]);
        return 1;
    }
    kmx_endpoint_tune(fd, &endpoint);
    if (fingerprint) {
        tls_client = kmx_tls_client_create(fingerprint);
        if (!tls_client) {
            fprintf(stderr, "kmx-attach: a fingerprint is %d hex characters\n",
                    KMX_TLS_FINGERPRINT_HEX);
            close(fd);
            for (unsigned profile = 0; profile < 2; profile++) kmx_encodec_close(audio_codecs[profile]);
            return 2;
        }
        {
            /* The handshake runs blocking, under a timeout so a server that
             * accepts and then says nothing cannot hold this forever. */
            struct timeval limit = {.tv_sec = 10, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
        }
        tls = kmx_tls_client_connect(tls_client, fd);
        if (!tls) {
            /* Either the handshake failed or the certificate is not the one
             * named.  Both mean: do not talk to this. */
            fprintf(stderr,
                "kmx-attach: the server did not present the expected "
                "certificate\n");
            close(fd);
            for (unsigned profile = 0; profile < 2; profile++) kmx_encodec_close(audio_codecs[profile]);
            return 1;
        }
        {
            struct timeval none = {.tv_sec = 0, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
        }
    }
    /* Only now: the loop from here on polls, and must never block in it. */
    make_non_blocking(fd);

    kmx_layout_init(&layout, rows, cols);
    if (kmx_render_create(&render) != KMX_OK ||
        kmx_predictor_create(&predictor) != KMX_OK ||
        kmx_image_cache_create(&images, 256, 32u * 1024u * 1024u) != KMX_OK ||
        kmx_motion_sink_create(&motion) != KMX_OK ||
        kmx_audio_sink_create(&audio) != KMX_OK ||
        kmx_grid_init(&screen, rows, cols) != KMX_OK) {
        fprintf(stderr, "kmx-attach: out of memory\n");
        for (unsigned profile = 0; profile < 2; profile++) kmx_encodec_close(audio_codecs[profile]);
        return 1;
    }
    kmx_framer_init(&framer);

    audio_offer.codecs = audio_mode == KMX_AUDIO_ENCODEC ? 0 : KMX_AUDIO_CODEC_PCM;
    if (audio_codec) audio_offer.codecs |= KMX_AUDIO_CODEC_ENCODEC;
    audio_offer.rates = audio_codec ? kmx_audio_rate_bit(audio_bitrate) : 0;
    audio_offer.maximum = KMX_ENCODEC_PACKET_MAX;
    if (send_hello(fd, rows, cols, view_only, token) || input_open(fd) ||
        send_audio_profile_offer(fd, audio_profiles) || send_audio_offer(fd, &audio_offer)) {
        stop_pending = 1; exit_code = 1;
    }
    audio_select_deadline = now_millis() + 2000u;
    if (!view_only && !reliable_input.enabled) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);

    if (!dump && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) have_termios = true;
    }
    if (pixel_input && !view_only && have_termios &&
        enable_pixel_input() == 0) {
        pixel_input_enabled = true;
    } else if (have_termios && !view_only && !pixel_input) {
        reset_terminal_input();
    }
    {
        /* sigaction with no SA_RESTART, deliberately.  glibc's signal() sets
         * SA_RESTART, so the handler ran, set its flag, and the interrupted
         * call restarted - leaving a client that had been asked to stop
         * carrying on regardless.  With the read no longer able to block that
         * is less critical than it was, and it is still what was meant. */
        struct sigaction action;
        memset(&action, 0, sizeof action);
        sigemptyset(&action.sa_mask);
        action.sa_handler = handle_resize;
        sigaction(SIGWINCH, &action, NULL);
        action.sa_handler = handle_stop;
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
    }
    signal(SIGPIPE, SIG_IGN);
    started_at = time(NULL);

    while (!stop_pending) {
        struct pollfd descriptors[3];
        int ready;
        bool redraw = false;
        bool write_failed = false;

        if (resize_pending && !dump) {
            resize_pending = 0;
            get_size(&rows, &cols);
            get_pixel_size(&local_pixel_width, &local_pixel_height);
            if (!view_only && (!reliable_input.enabled || reliable_input.ready)) {
                send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);
            }
            /* The screen is about to be described differently, so anything
             * predicted about the old one is void. */
            kmx_predictor_reset(predictor);
            kmx_render_invalidate(render);
        }

        /* TLS can need reads to complete a write. Keep that transport progress
         * enabled even while the application audio decoder is backpressured. */
        descriptors[0].events = (short)((!audio_backpressure || outgoing.tls_wait_read ? POLLIN : 0) |
            ((outgoing_pending() && !outgoing.tls_wait_read) ||
             (!audio_backpressure && tls_read_wants_write) ? POLLOUT : 0));
        descriptors[0].fd = descriptors[0].events ? fd : -1;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = dump || outgoing_pending() >= KMX_OUT_SOFT_LIMIT ||
            !input_has_room() ? 0 : POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = kmx_encodec_event_fd(audio_codec);
        descriptors[2].events = POLLIN;
        descriptors[2].revents = 0;
        {
            kmx_message_type pending_type;
            const unsigned char *pending_payload;
            size_t pending_size;
            bool complete = false;
            kmx_result pending_result = kmx_framer_next(&framer, &complete, &pending_type,
                                                       &pending_payload, &pending_size);
            bool processable = pending_result != KMX_OK ||
                (complete && (!audio_backpressure || kmx_encodec_packet_ready(audio_codec)));
            ready = poll(descriptors, 3, processable ? 0 : 50);
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (run_seconds > 0 && time(NULL) - started_at >= run_seconds) break;
        if (reliable_input.enabled && !reliable_input.ready && now_millis() > reliable_input.deadline) {
            fprintf(stderr, "kmx-attach: peer did not confirm reliable input; no input was replayed\n");
            exit_code = 1; break;
        }
        if (input_replay(fd)) { exit_code = 1; break; }
        if (audio_mode == KMX_AUDIO_ENCODEC && !audio_selected && now_millis() > audio_select_deadline) {
            fprintf(stderr, "kmx-attach: peer did not select the requested EnCodec profile\n");
            exit_code = 1; break;
        }
        if (send_text && !sent_once && input_has_room() && outgoing_pending() < KMX_OUT_SOFT_LIMIT) {
            size_t size = strlen(send_text) - send_offset;
            if (reliable_input.enabled && size > KMX_INPUT_DATA_MAX) size = KMX_INPUT_DATA_MAX;
            /* Sent even as a viewer, deliberately: the point of the test is
             * that the server refuses it, not that the client withholds it. */
            if (input_send(fd, send_text + send_offset, size) != 0) { exit_code = 1; break; }
            if (predict && kmx_predictor_type(predictor, send_text + send_offset, size)) {
                redraw = true;
            }
            send_offset += size;
            sent_once = send_text[send_offset] == '\0';
        }

        if (outgoing_pending() &&
            (descriptors[0].revents & (POLLERR | POLLHUP |
                (outgoing.tls_wait_read ? POLLIN : POLLOUT)))) {
            write_failed = outgoing_flush(fd, tls) != 0;
        }
        if (write_failed || (!audio_backpressure &&
            ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) ||
             (tls_read_wants_write && (descriptors[0].revents & POLLOUT))))) {
            ssize_t count = 0;
            if (!write_failed && tls) {
                count = kmx_tls_read(tls, buffer, sizeof buffer);
                tls_read_wants_write = kmx_tls_wants_write(tls);
            } else if (!write_failed) {
                count = read(fd, buffer, sizeof buffer);
            }
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) count = 0;
            if (count == 0) {
                int replacement;
                if (reliable_input.enabled) {
                    reliable_input.ready = false;
                    fprintf(stderr, "kmx-attach: connection lost; retaining %zu unacknowledged input bytes\n",
                        reliable_input.bytes);
                }
                outgoing_discard("connection lost");
                kmx_buffer_reset(&pixel_input_pending);
                if (have_termios && !reliable_input.enabled) (void)tcflush(STDIN_FILENO, TCIFLUSH);
                tls_read_wants_write = false;
                /* Release input capture before waiting for a replacement
                 * connection; the old focused pane no longer owns it. */
                if (pixel_input_enabled) {
                    disable_pixel_input();
                    pixel_input_enabled = false;
                }
                if (have_termios) reset_terminal_input();
                terminal_input_flags = 0;
                /* A pane that ended is not a link that dropped; do not chase
                 * a session that is over. */
                if (pane_ended || reconnect_seconds <= 0) break;
                /* The TLS session goes first, while its descriptor is still
                 * its own.  Freeing it after the close would send close_notify
                 * to whatever number the kernel has since handed out - which,
                 * because descriptors are reused lowest-first, is reliably the
                 * replacement connection about to be handshaked on. */
                if (tls) {
                    kmx_tls_session_free(tls);
                    tls = NULL;
                }
                close(fd);
                replacement = reconnect(&endpoint, reconnect_seconds);
                if (replacement < 0) {
                    fd = -1;
                    break;
                }

                fd = replacement;
                if (tls_client) {
                    /* A new connection is a new handshake, and the
                     * fingerprint is checked again: a server that changed
                     * identity while we were away is not the same server. */
                    {
                        struct timeval limit = {.tv_sec = 10, .tv_usec = 0};
                        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
                        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
                    }
                    tls = kmx_tls_client_connect(tls_client, fd);
                    if (!tls) {
                        /* Closed rather than abandoned: leaking it is harmless
                         * only because the process is about to exit, which is
                         * not a property worth relying on. */
                        close(fd);
                        fd = -1;
                        break;
                    }
                    {
                        struct timeval none = {.tv_sec = 0, .tv_usec = 0};
                        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
                        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
                    }
                }
                make_non_blocking(fd);
                /* Everything held about the old connection is now a guess:
                 * the panes may have been rearranged, and this client holds
                 * nothing the server knows about. */
                {
                    size_t slot;
                    for (slot = 0; slot < KMX_MAX_PANES; slot++) {
                        if (receivers[slot]) {
                            kmx_receiver_free(receivers[slot]);
                            receivers[slot] = NULL;
                        }
                    }
                }
                kmx_framer_free(&framer);
                kmx_framer_init(&framer);
                memset(&read_clock, 0, sizeof read_clock); audio_backpressure = false;
                kmx_predictor_reset(predictor);
                kmx_render_invalidate(render);
                layout.pane_count = 0;
                for (unsigned profile = 0; profile < 2; profile++) kmx_encodec_restart(audio_codecs[profile]);
                audio_codec = audio_codecs[0];
                audio_profile = KMX_AUDIO_PROFILE_C0;
                audio_profile_selected = false;
                audio_output_stop(&player);
                memset(&player, 0, sizeof player); player.fd = player.child = -1;
                audio_selected = audio_encodec = false;
                if (send_hello(fd, rows, cols, view_only, token) || input_open(fd) ||
                    send_audio_profile_offer(fd, audio_profiles) || send_audio_offer(fd, &audio_offer)) {
                    exit_code = 1; break;
                }
                audio_select_deadline = now_millis() + 2000u;
                if (!view_only && !reliable_input.enabled) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);
                if (pixel_input && !view_only && have_termios &&
                    enable_pixel_input() == 0) pixel_input_enabled = true;
                continue;
            }
            /* Conservatively attribute retained bytes to the oldest read.
             * Backpressure never refreshes a packet's 250ms age budget. */
            if (count > 0) {
                kmx_read_clock_push(&read_clock, framer.pending.size, now_millis());
                if (kmx_framer_push(&framer, buffer, (size_t)count) != KMX_OK) break;
            }
        }
        audio_backpressure = false;
        while (true) {
                kmx_message_type type;
                const unsigned char *payload;
                size_t size;
                bool available = false;
                if (kmx_framer_next(
                        &framer, &available, &type, &payload, &size) != KMX_OK) {
                    stop_pending = 1;
                    if (reliable_input.enabled) exit_code = 1;
                    break;
                }
                if (!available) break;
                if (type == KMX_MSG_INPUT_STATE && reliable_input.enabled) {
                    kmx_input_state state;
                    if (reliable_input.ready || kmx_input_state_decode(payload, size, &state) != KMX_OK ||
                        memcmp(state.client_id, reliable_input.client_id, sizeof state.client_id)) {
                        fprintf(stderr, "kmx-attach: invalid reliable input state\n");
                        stop_pending = 1; exit_code = 1; break;
                    }
                    if (state.status != KMX_INPUT_READY) {
                        static const char *const reasons[] = {
                            "ready", "another controller owns input", "input session expired",
                            "server epoch changed", "input limit", "input mode denied"
                        };
                        fprintf(stderr, "kmx-attach: reliable input refused: %s; no input was replayed\n",
                            reasons[state.status]);
                        stop_pending = 1; exit_code = 1; break;
                    }
                    if (!state.grace_ms ||
                        (reliable_input.identified && memcmp(state.epoch, reliable_input.epoch, sizeof state.epoch)) ||
                        input_acknowledge(state.accepted) != 0) {
                        fprintf(stderr, "kmx-attach: inconsistent input epoch or acknowledgement; no input was replayed\n");
                        stop_pending = 1; exit_code = 1; break;
                    }
                    memcpy(reliable_input.epoch, state.epoch, sizeof state.epoch);
                    reliable_input.identified = true;
                    reliable_input.ready = true;
                    reliable_input.replay = reliable_input.head;
                    fprintf(stderr, "kmx-attach: reliable input ready; server accepted through %llu\n",
                        (unsigned long long)state.accepted);
                    if (send_dimensions(fd, KMX_MSG_RESIZE, rows, cols) || input_replay(fd)) {
                        stop_pending = 1; exit_code = 1; break;
                    }
                } else if (type == KMX_MSG_INPUT_ACK && reliable_input.enabled) {
                    kmx_input_ack ack;
                    if (!reliable_input.ready || kmx_input_ack_decode(payload, size, &ack) != KMX_OK ||
                        input_acknowledge(ack.accepted) != 0) {
                        fprintf(stderr, "kmx-attach: invalid input acknowledgement\n");
                        stop_pending = 1; exit_code = 1; break;
                    }
                } else if (type == KMX_MSG_LAYOUT) {
                    kmx_layout received;
                    if (kmx_layout_apply(&received, payload, size) == KMX_OK) {
                        size_t slot;
                        layout = received;
                        if (reliable_input.enabled && layout.pane_count != 1) {
                            fprintf(stderr, "kmx-attach: reliable input requires a single text pane\n");
                            stop_pending = 1; exit_code = 1; break;
                        }
                        if (terminal_input_flags && have_termios &&
                            !view_only && !pixel_input) {
                            if (apply_terminal_input(0) != 0) {
                                stop_pending = 1; exit_code = 1; break;
                            }
                            terminal_input_flags = 0;
                        }
                        for (slot = 0; slot < layout.pane_count; slot++) {
                            const kmx_pane_info *info = &layout.panes[slot];
                            if (!receivers[slot] &&
                                kmx_receiver_create(
                                    &receivers[slot], info->rows, info->cols) != KMX_OK) {
                                stop_pending = 1;
                                break;
                            }
                            if (info->focused) focus_hint = slot;
                        }
                        /* The arrangement moved, so what is on screen is no
                         * longer a guide to what to draw next. */
                        kmx_render_invalidate(render);
                        kmx_predictor_reset(predictor);
                        redraw = true;
                    }
                } else if (type == KMX_MSG_TERMINAL_MODES) {
                    uint8_t pane;
                    uint32_t flags;
                    if (kmx_modes_decode(payload, size, &pane, &flags) != KMX_OK ||
                        pane >= layout.pane_count || pane >= KMX_MAX_PANES ||
                        !layout.panes[pane].focused) {
                        fprintf(stderr, "kmx-attach: invalid terminal mode state\n");
                        stop_pending = 1; exit_code = 1; break;
                    }
                    if (have_termios && !dump && !view_only && !pixel_input) {
                        /* Mouse coordinates currently describe the whole
                         * local terminal. Multiple pane geometry needs an
                         * input mapping before capture can be enabled. */
                        if (layout.pane_count != 1) flags &= ~KMX_MODE_MOUSE_TRACKING;
                        if (flags != terminal_input_flags &&
                            apply_terminal_input(flags) != 0) {
                            stop_pending = 1; exit_code = 1; break;
                        }
                        terminal_input_flags = flags;
                    }
                } else if (type == KMX_MSG_CELLS && size >= 1) {
                    size_t which = payload[0];
                    if (which < KMX_MAX_PANES && receivers[which]) {
                        uint64_t sequence = 0;
                        if (kmx_receiver_apply(
                                receivers[which], payload + 1, size - 1,
                                &sequence) == KMX_OK) {
                            unsigned char ack[9];
                            size_t position;
                            ack[0] = (unsigned char)which;
                            for (position = 0; position < 8; position++) {
                                ack[position + 1] =
                                    (unsigned char)(sequence >> (8 * (7 - position)));
                            }
                            send_message(fd, KMX_MSG_ACK, ack, sizeof ack);
                            redraw = true;
                        }
                    }
                } else if (type == KMX_MSG_IMAGE) {
                    kmx_image_message image;
                    if (kmx_image_decode(payload, size, &image) == KMX_OK) {
                        const unsigned char *bytes = image.data;
                        size_t length = image.size;
                        if (image.has_data) {
                            (void)kmx_image_cache_put(
                                images, &image.key, image.data, image.size);
                        } else {
                            /* A reference to a picture already held: this is
                             * what makes a repeat cost sixteen bytes. */
                            bytes = kmx_image_cache_get(images, &image.key, &length);
                        }
                        if (bytes && length) {
                            /* Replayed as the escape it originally was, so the
                             * local terminal places it exactly as the remote
                             * one would have. */
                            (void)write_all(STDOUT_FILENO, "\033_", 2);
                            (void)write_all(STDOUT_FILENO, bytes, length);
                            (void)write_all(STDOUT_FILENO, "\033\\", 2);
                        }
                    }
                } else if (type == KMX_MSG_FRAME && size >= 1) {
                    /* payload[0] is the pane; one pixel pane for now. */
                    if (kmx_motion_sink_apply(motion, payload + 1, size - 1) == KMX_OK) {
                        int frame_width = 0;
                        int frame_height = 0;
                        const unsigned char *pixels =
                            kmx_motion_sink_pixels(motion, &frame_width, &frame_height);
                        remote_pixel_width = frame_width;
                        remote_pixel_height = frame_height;
                        frames_seen++;
                        if (dump && pixels) {
                            /* A checksum rather than the pixels: enough for a
                             * test to prove the frame arrived intact, without
                             * writing megabytes to a log. */
                            unsigned long sum = 0;
                            long index;
                            long total = (long)frame_width * frame_height * 3;
                            for (index = 0; index < total; index++) {
                                sum = sum * 131u + pixels[index];
                            }
                            printf("KMX_FRAME %dx%d #%lu sum=%lu\n",
                                   frame_width, frame_height, frames_seen, sum);
                            fflush(stdout);
                        } else if (pixels &&
                                   display_motion(
                                       pixels, frame_width, frame_height,
                                       cols, rows) != 0) {
                            exit_code = 1;
                            stop_pending = 1;
                        }
                    }
                } else if (type == KMX_MSG_AUDIO_PROFILE) {
                    kmx_audio_profile selection;
                    if (!audio_profiles || kmx_audio_profile_read(&selection, payload, size) ||
                        selection.kind != 1 || !(audio_profiles & KMX_AUDIO_PROFILE_BIT(selection.value)) ||
                        (audio_profile_selected && selection.value != audio_profile) ||
                        (audio_selected && (!audio_profile_selected || !audio_encodec))) {
                        fprintf(stderr, "kmx-attach: incompatible audio profile refused\n");
                        exit_code = 1; stop_pending = 1; break;
                    }
                    audio_profile_selected = true;
                    audio_profile = selection.value;
                } else if (type == KMX_MSG_AUDIO_CAPS) {
                    kmx_audio_caps selection;
                    if (kmx_audio_caps_read(&selection, payload, size) || selection.kind != 1 ||
                        !(selection.codecs & audio_offer.codecs) ||
                        (audio_profile_selected && selection.codecs != KMX_AUDIO_CODEC_ENCODEC) ||
                        (selection.codecs == KMX_AUDIO_CODEC_ENCODEC &&
                         (!audio_codec || selection.rates != audio_offer.rates)) ||
                        (audio_selected && (selection.codecs != audio_selection.codecs ||
                         selection.rates != audio_selection.rates || selection.maximum != audio_selection.maximum))) {
                        fprintf(stderr, "kmx-attach: incompatible audio selection refused\n");
                        exit_code = 1; stop_pending = 1; break;
                    }
                    if (!audio_selected) {
                        audio_selected = true; audio_selection = selection;
                        audio_encodec = selection.codecs == KMX_AUDIO_CODEC_ENCODEC;
                        /* An old server sends no marker: its selection uses
                         * the already-prewarmed C0 decoder on both ends. */
                        if (audio_encodec) audio_codec = audio_codecs[audio_profile];
                        if (audio_encodec && player.attempted) {
                            audio_output_stop(&player);
                            memset(&player, 0, sizeof player); player.fd = player.child = -1;
                        }
                        if (dump) {
                            printf("KMX_AUDIO_CODEC %s bitrate=%u threads=%u profile=%s\n", audio_encodec ? "encodec-24k-mono-v1" : "pcm-s16le-zstd-v1",
                                   audio_encodec ? audio_bitrate : 0u, audio_encodec ? audio_threads : 0u,
                                   audio_encodec && audio_profile ? "C5-R4" : "C0");
                            fflush(stdout);
                        }
                    }
                } else if (type == KMX_MSG_AUDIO && audio_encodec) {
                    if (size < 4 || size > KMX_ENCODEC_PACKET_MAX || memcmp(payload, "KMA\2", 4)) {
                        exit_code = 1; stop_pending = 1; break;
                    }
                    /* Leave this complete frame in the existing bounded
                     * framer while the two decoder input slots are full.
                     * Resume on the codec event even without a socket read. */
                    if (!kmx_encodec_packet_ready(audio_codec)) {
                        audio_backpressure = true; break;
                    }
                    (void)kmx_encodec_offer_packet_at(audio_codec, payload, size, read_clock.earliest_ms);
                } else if (type == KMX_MSG_AUDIO && audio_mode == KMX_AUDIO_ENCODEC && !audio_selected) {
                    /* A server in auto mode sends legacy PCM to every greeted
                     * peer whose AUDIO_CAPS offer it has not yet read. Over TLS
                     * the HELLO and the offer can arrive in separate reads, so
                     * a block can precede the selection. Before selection that
                     * is a race, not a protocol violation: discard and count it.
                     * The selection deadline above still bounds the wait. */
                    preselection_audio_discarded++;
                } else if (type == KMX_MSG_AUDIO) {
                    if (audio_mode == KMX_AUDIO_ENCODEC) { exit_code = 1; stop_pending = 1; break; }
                    if (kmx_audio_sink_apply(audio, payload, size) == KMX_OK) {
                        size_t block = 0;
                        uint64_t when = 0;
                        blocks_seen++;
                        {
                            const unsigned char *pcm =
                                kmx_audio_sink_pcm(audio, &block, &when);
                            if (pcm) {
                                audio_output_start(
                                    &player, audio_output_command,
                                    kmx_audio_sink_sample_rate(audio),
                                    kmx_audio_sink_channels(audio), dump);
                                audio_output_offer(&player, pcm, block);
                                if (dump) {
                                    printf("KMX_AUDIO %zu #%lu at=%llu gap=%llu\n",
                                           block, blocks_seen,
                                           (unsigned long long)when,
                                           (unsigned long long)
                                               kmx_audio_sink_gap_millis(audio));
                                    fflush(stdout);
                                }
                            }
                        }
                    }
                } else if (type == KMX_MSG_EXIT) {
                    pane_ended = true;
                    stop_pending = 1;
                }
                {
                    size_t before = framer.pending.size;
                    kmx_framer_consume(&framer);
                    kmx_read_clock_consume(&read_clock, before - framer.pending.size);
                }
        }

        if (audio_codec) {
            kmx_encodec_output output;
            while (kmx_encodec_receive(audio_codec, &output)) {
                unsigned char pcm[KMX_ENCODEC_SAMPLES * 2u];
                size_t sample;
                if (!audio_encodec) continue;
                for (sample = 0; sample < output.size / 2u; sample++) {
                    uint16_t value = (uint16_t)output.pcm[sample];
                    pcm[sample * 2u] = (unsigned char)value;
                    pcm[sample * 2u + 1u] = (unsigned char)(value >> 8);
                }
                blocks_seen++;
                audio_output_start(&player, audio_output_command, 24000, 1, dump);
                audio_output_offer(&player, pcm, output.size);
                if (dump) {
                    printf("KMX_AUDIO %zu #%lu at=%llu epoch=%llu flags=%u\n", output.size, blocks_seen,
                        (unsigned long long)output.pts_ms, (unsigned long long)output.epoch, output.flags);
                    fflush(stdout);
                }
            }
        }

        if ((descriptors[1].revents & POLLIN) && input_has_room() &&
            outgoing_pending() < KMX_OUT_SOFT_LIMIT && !stop_pending) {
            ssize_t count = read(STDIN_FILENO, buffer,
                reliable_input.enabled ? KMX_INPUT_DATA_MAX : sizeof buffer);
            if (count > 0) {
                if (memchr(buffer, 0x1d, (size_t)count)) break; /* Ctrl-] */
                if (pixel_input) {
                    bool detach = false;
                    if (kmx_pixel_input_transform(
                            &pixel_input_pending, buffer, (size_t)count,
                            local_pixel_width, local_pixel_height,
                            remote_pixel_width, remote_pixel_height,
                            &pixel_input_output, &detach) != 0) {
                        break;
                    }
                    if (detach) break;
                    if (pixel_input_output.size &&
                        send_message(
                            fd, KMX_MSG_INPUT,
                            pixel_input_output.data,
                            pixel_input_output.size) != 0) {
                        break;
                    }
                } else if (memchr(buffer, 0x0f, (size_t)count) &&
                           layout.pane_count > 1) {
                    /* Ctrl-O: move focus on.  The server owns focus, so this
                     * asks rather than assumes. */
                    unsigned char wanted =
                        (unsigned char)((focus_hint + 1) % layout.pane_count);
                    send_message(fd, KMX_MSG_FOCUS, &wanted, 1);
                    kmx_predictor_reset(predictor);
                } else {
                    if (input_send(fd, buffer, (size_t)count) != 0) { exit_code = 1; break; }
                    if (predict &&
                        kmx_predictor_type(predictor, buffer, (size_t)count)) {
                        redraw = true;
                    }
                }
            } else if (count == 0 && dump) {
                break;
            }
        }

        if (redraw && layout.pane_count) {
            const kmx_grid *sources[KMX_MAX_PANES];
            kmx_buffer painted;
            size_t slot;
            bool ready_to_draw = true;
            for (slot = 0; slot < layout.pane_count; slot++) {
                if (!receivers[slot]) {
                    ready_to_draw = false;
                    break;
                }
                sources[slot] = kmx_receiver_grid(receivers[slot]);
            }
            if (!ready_to_draw) continue;
            if (kmx_layout_composite(
                    &layout, sources, layout.pane_count, &screen) != KMX_OK) {
                break;
            }
            if (predict) {
                kmx_predictor_reconcile(predictor, &screen);
                kmx_predictor_overlay(predictor, &screen);
            }
            kmx_buffer_init(&painted);
            if (kmx_render_frame(render, &screen, &painted) == KMX_OK) {
                if (write_all(STDOUT_FILENO, painted.data, painted.size) != 0) {
                    exit_code = 1;
                    kmx_buffer_free(&painted);
                    break;
                }
            }
            kmx_buffer_free(&painted);
        }
        audio_output_flush(&player);
    }

    if (reliable_input.enabled && send_text && !sent_once) {
        fprintf(stderr, "kmx-attach: --send ended before all input was submitted\n");
        exit_code = 1;
    }
    if (!exit_code) input_close(fd, tls);
    if (reliable_input.head) {
        fprintf(stderr, "kmx-attach: leaving with %zu unacknowledged input bytes; acceptance is uncertain\n",
            reliable_input.bytes);
        exit_code = 1;
    }
    input_free();
    if (reliable_input.enabled && have_termios) (void)tcflush(STDIN_FILENO, TCIFLUSH);
    if (pixel_input_enabled) disable_pixel_input();
    if (have_termios) reset_terminal_input();
    if (have_termios) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (outgoing_pending()) outgoing_discard("leaving session");
    if (outgoing.failed) exit_code = 1;
    if (reliable_input.enabled && exit_code && fd >= 0) {
        /* SSL_shutdown must not advance a pending application write after a
         * protocol error. Keep the fd allocated until TLS cleanup finishes. */
        (void)shutdown(fd, SHUT_RDWR);
    }
    if (!dump) {
        char remove_image[64];
        int length = snprintf(
            remove_image, sizeof remove_image,
            "\033_Ga=d,d=I,i=%u,q=2\033\\", KMX_REMOTE_IMAGE_ID);
        if (length > 0 && (size_t)length < sizeof remove_image) {
            (void)write_all(STDOUT_FILENO, remove_image, (size_t)length);
        }
        (void)write_all(STDOUT_FILENO, "\033[0m\r\n", 6);
    }
    {
        size_t slot;
        for (slot = 0; slot < KMX_MAX_PANES; slot++) {
            if (receivers[slot]) kmx_receiver_free(receivers[slot]);
            kmx_grid_free(&panes[slot]);
        }
    }
    kmx_framer_free(&framer);
    kmx_buffer_free(&outgoing.bytes);
    kmx_buffer_free(&pixel_input_output);
    kmx_buffer_free(&pixel_input_pending);
    kmx_image_cache_free(images);
    kmx_motion_sink_free(motion);
    kmx_audio_sink_free(audio);
    kmx_grid_free(&screen);
    kmx_predictor_free(predictor);
    kmx_render_free(render);
    audio_output_stop(&player);
    bool had_audio_codec = audio_codecs[0] != NULL;
    kmx_encodec_stats audio_total = {0};
    for (unsigned profile = 0; profile < 2; profile++) if (audio_codecs[profile]) {
        kmx_encodec_stats stats = kmx_encodec_statistics(audio_codecs[profile]);
        audio_total.calls += stats.calls; audio_total.inference_ns += stats.inference_ns;
        audio_total.input_drops += stats.input_drops; audio_total.output_drops += stats.output_drops;
        audio_total.discontinuities += stats.discontinuities;
        fprintf(stderr, "kmx-attach: encodec_profile calls=%llu rtf=%.6f input_drops=%llu output_drops=%llu discontinuities=%llu profile=%s\n",
            (unsigned long long)stats.calls, stats.calls ? (double)stats.inference_ns / ((double)stats.calls * 40000000.0) : 0.0,
            (unsigned long long)stats.input_drops, (unsigned long long)stats.output_drops,
            (unsigned long long)stats.discontinuities, profile ? "C5-R4" : "C0");
        kmx_encodec_close(audio_codecs[profile]);
    }
    if (had_audio_codec) fprintf(stderr, "kmx-attach: encodec calls=%llu rtf=%.6f input_drops=%llu output_drops=%llu discontinuities=%llu\n",
        (unsigned long long)audio_total.calls,
        audio_total.calls ? (double)audio_total.inference_ns / ((double)audio_total.calls * 40000000.0) : 0.0,
        (unsigned long long)audio_total.input_drops, (unsigned long long)audio_total.output_drops,
        (unsigned long long)audio_total.discontinuities);
    if (preselection_audio_discarded) {
        fprintf(stderr, "kmx-attach: discarded %lu audio block(s) received before the EnCodec selection\n",
                preselection_audio_discarded);
    }
    if (player.dropped) {
        fprintf(stderr, "kmx-attach: dropped %zu audio block(s) at playback\n",
                player.dropped);
    }
    kmx_tls_session_free(tls);
    kmx_tls_client_free(tls_client);
    if (fd >= 0) close(fd);
    return exit_code;
}
