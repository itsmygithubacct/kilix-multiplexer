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
#include <termios.h>
#include <time.h>
#include <unistd.h>

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

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *send_text = NULL;
    bool predict = true;
    bool dump = false;
    bool view_only = false;
    const char *token = NULL;
    const char *fingerprint = NULL;
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
    kmx_framer framer;
    kmx_grid screen;
    unsigned char buffer[65536];
    size_t focus_hint = 0;
    int rows;
    int cols;
    int exit_code = 0;

    memset(receivers, 0, sizeof receivers);
    memset(panes, 0, sizeof panes);

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
        } else if (strcmp(argv[index], "--dump") == 0) {
            dump = true;
        } else if (strcmp(argv[index], "--send") == 0 && index + 1 < argc) {
            send_text = argv[++index];
        } else if (strcmp(argv[index], "--seconds") == 0 && index + 1 < argc) {
            run_seconds = atoi(argv[++index]);
        } else {
            fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict]"
                            " [--view] [--token TOKEN]\n"
                            "       [--tls-fingerprint HEX] [--reconnect N]"
                            "       [--dump] [--send TEXT] [--seconds N]\n");
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
                        }
                    }
                } else if (type == KMX_MSG_AUDIO) {
                    if (kmx_audio_sink_apply(audio, payload, size) == KMX_OK) {
                        size_t block = 0;
                        uint64_t when = 0;
                        blocks_seen++;
                        if (kmx_audio_sink_pcm(audio, &block, &when) && dump) {
                            printf("KMX_AUDIO %zu #%lu at=%llu gap=%llu\n",
                                   block, blocks_seen,
                                   (unsigned long long)when,
                                   (unsigned long long)kmx_audio_sink_gap_millis(audio));
                            fflush(stdout);
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
                if (memchr(buffer, 0x0f, (size_t)count) && layout.pane_count > 1) {
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
    }

    if (have_termios) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (!dump) (void)write_all(STDOUT_FILENO, "\033[0m\r\n", 6);
    {
        size_t slot;
        for (slot = 0; slot < KMX_MAX_PANES; slot++) {
            if (receivers[slot]) kmx_receiver_free(receivers[slot]);
            kmx_grid_free(&panes[slot]);
        }
    }
    kmx_framer_free(&framer);
    kmx_image_cache_free(images);
    kmx_motion_sink_free(motion);
    kmx_audio_sink_free(audio);
    kmx_grid_free(&screen);
    kmx_predictor_free(predictor);
    kmx_render_free(render);
    kmx_tls_session_free(tls);
    kmx_tls_client_free(tls_client);
    if (fd >= 0) close(fd);
    return exit_code;
}
