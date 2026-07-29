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

#include <errno.h>
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

static int
write_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);
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
send_hello(int fd, int rows, int cols, bool view_only) {
    unsigned char payload[5];
    payload[0] = (unsigned char)(rows >> 8);
    payload[1] = (unsigned char)rows;
    payload[2] = (unsigned char)(cols >> 8);
    payload[3] = (unsigned char)cols;
    payload[4] = view_only ? 1u : 0u;
    return send_message(fd, KMX_MSG_HELLO, payload, sizeof payload);
}

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *send_text = NULL;
    bool predict = true;
    bool dump = false;
    bool view_only = false;
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
                            " [--view]\n       [--dump] [--send TEXT]"
                            " [--seconds N]\n");
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

    kmx_layout_init(&layout, rows, cols);
    if (kmx_render_create(&render) != KMX_OK ||
        kmx_predictor_create(&predictor) != KMX_OK ||
        kmx_image_cache_create(&images, 256, 32u * 1024u * 1024u) != KMX_OK ||
        kmx_grid_init(&screen, rows, cols) != KMX_OK) {
        fprintf(stderr, "kmx-attach: out of memory\n");
        return 1;
    }
    kmx_framer_init(&framer);

    send_hello(fd, rows, cols, view_only);
    if (!view_only) send_dimensions(fd, KMX_MSG_RESIZE, rows, cols);

    if (!dump && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) have_termios = true;
    }
    signal(SIGWINCH, handle_resize);
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);
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
            ssize_t count = read(fd, buffer, sizeof buffer);
            if (count <= 0) break;
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
                } else if (type == KMX_MSG_EXIT) {
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
    kmx_grid_free(&screen);
    kmx_predictor_free(predictor);
    kmx_render_free(render);
    close(fd);
    return exit_code;
}
