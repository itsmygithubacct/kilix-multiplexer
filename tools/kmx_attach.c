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
#include "endpoint.h"
#include "kmx_tls.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

/* The one place that knows whether this connection is wrapped. */
static kmx_tls_session *active_tls;

static int
write_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count;
        if (active_tls && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
            count = kmx_tls_write(active_tls, cursor + done, size - done);
            if (count < 0 && errno == EAGAIN) {
                /* Waited on rather than spun on.  The descriptor is
                 * non-blocking now, so retrying immediately is a hot loop. */
                struct pollfd waiting;
                waiting.fd = fd;
                waiting.events = POLLOUT;
                waiting.revents = 0;
                if (poll(&waiting, 1, 5000) <= 0) return -1;
                continue;
            }
        } else {
            count = write(fd, cursor + done, size - done);
        }
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
    int result;
    kmx_buffer_init(&framed);
    if (kmx_frame_encode(type, payload, size, &framed) != KMX_OK) {
        kmx_buffer_free(&framed);
        return -1;
    }
    result = write_all(fd, framed.data, framed.size);
    kmx_buffer_free(&framed);
    return result;
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
scaled_coordinate(int value, int local_extent, int remote_extent) {
    long long numerator;
    if (local_extent <= 1 || remote_extent <= 1) return 0;
    if (value < 0) value = 0;
    if (value >= local_extent) value = local_extent - 1;
    numerator =
        (long long)value * (remote_extent - 1) + (local_extent - 1) / 2;
    return (int)(numerator / (local_extent - 1));
}

/*
 * Pixel panes request SGR-pixel mouse reports from Kilix. Their coordinates
 * describe the local terminal, while the XTest helper receives coordinates in
 * the remote framebuffer. Rewrite only those reports and leave CSI-u keys,
 * bracketed paste and arbitrary text byte-for-byte intact.
 *
 * `pending` retains a CSI split across read() calls. Ctrl-] is encoded as
 * CSI-u while the enhanced keyboard protocol is active, so recognise that
 * sequence here as the same local detach key used by ordinary panes.
 */
static int
pixel_input_transform(
    kmx_buffer *pending,
    const void *data,
    size_t size,
    int local_width,
    int local_height,
    int remote_width,
    int remote_height,
    kmx_buffer *out,
    bool *detach
) {
    size_t at = 0;
    if (kmx_buffer_append(pending, data, size) != KMX_OK) return -1;
    kmx_buffer_reset(out);
    while (at < pending->size) {
        size_t end;
        size_t length;
        unsigned char final;
        char sequence[128];

        if (pending->data[at] != '\033' ||
            at + 1 >= pending->size ||
            pending->data[at + 1] != '[') {
            if (pending->data[at] == '\033' && at + 1 == pending->size) break;
            if (kmx_buffer_append(out, pending->data + at, 1) != KMX_OK) {
                return -1;
            }
            at++;
            continue;
        }

        end = at + 2;
        while (end < pending->size &&
               !(pending->data[end] >= 0x40 && pending->data[end] <= 0x7e)) {
            end++;
        }
        if (end == pending->size) {
            if (pending->size - at < sizeof sequence) break;
            if (kmx_buffer_append(out, pending->data + at, 1) != KMX_OK) {
                return -1;
            }
            at++;
            continue;
        }

        length = end - at + 1;
        final = pending->data[end];
        if (length < sizeof sequence) {
            int code = 0;
            int modifiers = 1;
            memcpy(sequence, pending->data + at, length);
            sequence[length] = '\0';

            if (final == 'u' &&
                sscanf(sequence, "\033[%d;%d", &code, &modifiers) == 2 &&
                code == ']' && ((modifiers - 1) & 4)) {
                *detach = true;
                at = end + 1;
                continue;
            }

            if ((final == 'M' || final == 'm') &&
                length >= 4 && sequence[2] == '<') {
                unsigned button = 0;
                int x = 0;
                int y = 0;
                char parsed_final = '\0';
                int consumed = 0;
                if (sscanf(
                        sequence, "\033[<%u;%d;%d%c%n",
                        &button, &x, &y, &parsed_final, &consumed) == 4 &&
                    consumed == (int)length &&
                    (parsed_final == 'M' || parsed_final == 'm') &&
                    local_width > 0 && local_height > 0 &&
                    remote_width > 0 && remote_height > 0) {
                    char rewritten[128];
                    int rewritten_size;
                    x = scaled_coordinate(x, local_width, remote_width);
                    y = scaled_coordinate(y, local_height, remote_height);
                    rewritten_size = snprintf(
                        rewritten, sizeof rewritten, "\033[<%u;%d;%d%c",
                        button, x, y, parsed_final);
                    if (rewritten_size < 0 ||
                        (size_t)rewritten_size >= sizeof rewritten ||
                        kmx_buffer_append(
                            out, rewritten, (size_t)rewritten_size) != KMX_OK) {
                        return -1;
                    }
                    at = end + 1;
                    continue;
                }
            }
        }
        if (kmx_buffer_append(out, pending->data + at, length) != KMX_OK) {
            return -1;
        }
        at = end + 1;
    }
    if (at) {
        memmove(pending->data, pending->data + at, pending->size - at);
        pending->size -= at;
    }
    return 0;
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
    child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        output->disabled = true;
        return;
    }
    if (child == 0) {
        char rate[32];
        char channel_count[16];
        int null_fd = open("/dev/null", O_WRONLY);
        snprintf(rate, sizeof rate, "%u", sample_rate);
        snprintf(channel_count, sizeof channel_count, "%u", channels);
        setenv("KMX_AUDIO_RATE", rate, 1);
        setenv("KMX_AUDIO_CHANNELS", channel_count, 1);
        close(pipe_fds[1]);
        if (dup2(pipe_fds[0], STDIN_FILENO) < 0) _exit(127);
        close(pipe_fds[0]);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        if (command) {
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        } else if (command_exists("pacat")) {
            execlp(
                "pacat", "pacat", "--playback", "--raw",
                "--format=s16le", "--rate", rate, "--channels", channel_count,
                "--latency-msec=100", (char *)NULL);
        } else {
            execlp(
                "aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE",
                "-r", rate, "-c", channel_count, (char *)NULL);
        }
        _exit(127);
    }
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
        int attempt;
        pid_t reaped = 0;
        for (attempt = 0; attempt < 20 && reaped == 0; attempt++) {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
            reaped = waitpid(output->child, NULL, WNOHANG);
            if (reaped == 0) nanosleep(&pause, NULL);
        }
        if (reaped == 0) {
            kill(output->child, SIGTERM);
            (void)waitpid(output->child, NULL, 0);
        }
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
    kmx_tls_client *tls_client = NULL;
    kmx_tls_session *tls = NULL;
    int reconnect_seconds = 30;
    bool pane_ended = false;
    int run_seconds = 0;
    time_t started_at;
    bool sent_once = false;
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
        } else {
            fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict]"
                            " [--view] [--token TOKEN]\n"
                            "       [--tls-fingerprint HEX] [--reconnect N]"
                            "       [--dump] [--send TEXT] [--seconds N]\n"
                            "       [--audio-output COMMAND|--no-audio]"
                            " [--pixel-input]\n");
            return 2;
        }
        index++;
    }
    if (!socket_path) {
        fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict]"
                        " [--dump] [--send TEXT] [--seconds N]\n");
        return 2;
    }

    get_size(&rows, &cols);
    get_pixel_size(&local_pixel_width, &local_pixel_height);
    if (!kmx_endpoint_parse(socket_path, &endpoint)) {
        fprintf(stderr, "kmx-attach: cannot make sense of '%s'\n", socket_path);
        return 2;
    }
    fd = kmx_endpoint_connect(&endpoint);
    if (fd < 0) {
        fprintf(stderr, "kmx-attach: connect: %s\n", strerror(errno));
        return 1;
    }
    kmx_endpoint_tune(fd, &endpoint);
    if (fingerprint) {
        tls_client = kmx_tls_client_create(fingerprint);
        if (!tls_client) {
            fprintf(stderr, "kmx-attach: a fingerprint is %d hex characters\n",
                    KMX_TLS_FINGERPRINT_HEX);
            close(fd);
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
        active_tls = tls;
        if (!tls) {
            /* Either the handshake failed or the certificate is not the one
             * named.  Both mean: do not talk to this. */
            fprintf(stderr,
                "kmx-attach: the server did not present the expected "
                "certificate\n");
            close(fd);
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
        return 1;
    }
    kmx_framer_init(&framer);

    send_hello(fd, rows, cols, view_only, token);
    if (!view_only) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);

    if (!dump && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) have_termios = true;
    }
    if (pixel_input && !view_only && have_termios &&
        enable_pixel_input() == 0) {
        pixel_input_enabled = true;
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
        struct pollfd descriptors[2];
        int ready;
        bool redraw = false;

        if (resize_pending && !dump) {
            resize_pending = 0;
            get_size(&rows, &cols);
            get_pixel_size(&local_pixel_width, &local_pixel_height);
            if (!view_only) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);
            /* The screen is about to be described differently, so anything
             * predicted about the old one is void. */
            kmx_predictor_reset(predictor);
            kmx_render_invalidate(render);
        }

        descriptors[0].fd = fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = dump ? 0 : POLLIN;
        descriptors[1].revents = 0;
        ready = poll(descriptors, 2, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (run_seconds > 0 && time(NULL) - started_at >= run_seconds) break;
        if (send_text && !sent_once) {
            sent_once = true;
            /* Sent even as a viewer, deliberately: the point of the test is
             * that the server refuses it, not that the client withholds it. */
            if (send_message(fd, KMX_MSG_INPUT, send_text, strlen(send_text)) != 0) break;
            if (predict && kmx_predictor_type(predictor, send_text, strlen(send_text))) {
                redraw = true;
            }
        }

        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t count;
            if (tls) {
                count = kmx_tls_read(tls, buffer, sizeof buffer);
                if (count < 0 && errno == EAGAIN) continue;
            } else {
                count = read(fd, buffer, sizeof buffer);
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                  errno == EINTR)) {
                    continue;
                }
            }
            if (count <= 0) {
                int replacement;
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
                    active_tls = NULL;
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
                    active_tls = tls;
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
                kmx_predictor_reset(predictor);
                kmx_render_invalidate(render);
                layout.pane_count = 0;
                send_hello(fd, rows, cols, view_only, token);
                if (!view_only) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);
                continue;
            }
            if (kmx_framer_push(&framer, buffer, (size_t)count) != KMX_OK) break;
            while (true) {
                kmx_message_type type;
                const unsigned char *payload;
                size_t size;
                bool available = false;
                if (kmx_framer_next(
                        &framer, &available, &type, &payload, &size) != KMX_OK) {
                    stop_pending = 1;
                    break;
                }
                if (!available) break;
                if (type == KMX_MSG_LAYOUT) {
                    kmx_layout received;
                    if (kmx_layout_apply(&received, payload, size) == KMX_OK) {
                        size_t slot;
                        layout = received;
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
                } else if (type == KMX_MSG_AUDIO) {
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
                kmx_framer_consume(&framer);
            }
        }

        if (descriptors[1].revents & POLLIN) {
            ssize_t count = read(STDIN_FILENO, buffer, sizeof buffer);
            if (count > 0) {
                if (memchr(buffer, 0x1d, (size_t)count)) break; /* Ctrl-] */
                if (pixel_input) {
                    bool detach = false;
                    if (pixel_input_transform(
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
                    if (send_message(fd, KMX_MSG_INPUT, buffer, (size_t)count) != 0) break;
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

    if (pixel_input_enabled) disable_pixel_input();
    if (have_termios) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
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
    kmx_buffer_free(&pixel_input_output);
    kmx_buffer_free(&pixel_input_pending);
    kmx_image_cache_free(images);
    kmx_motion_sink_free(motion);
    kmx_audio_sink_free(audio);
    kmx_grid_free(&screen);
    kmx_predictor_free(predictor);
    kmx_render_free(render);
    audio_output_stop(&player);
    if (player.dropped) {
        fprintf(stderr, "kmx-attach: dropped %zu audio block(s) at playback\n",
                player.dropped);
    }
    kmx_tls_session_free(tls);
    kmx_tls_client_free(tls_client);
    if (fd >= 0) close(fd);
    return exit_code;
}
