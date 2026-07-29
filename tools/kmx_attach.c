/* kmx-attach — render a remote pane locally and type into it.
 *
 * Received frames describe the screen, so the client draws the difference
 * against what it already shows.  Typed characters are echoed immediately
 * where they are expected to land and withdrawn when the server's own frame
 * disagrees, because on a slow link the alternative is typing that feels
 * broken.
 *
 *   kmx-attach --socket PATH [--no-predict] [--dump]
 *
 * Ctrl-] detaches.  --dump renders to stdout without taking over the terminal,
 * which is what makes the client testable without a TTY. */
#define _GNU_SOURCE

#include "kilix_mux.h"

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

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *send_text = NULL;
    bool predict = true;
    bool dump = false;
    int run_seconds = 0;
    time_t started_at;
    bool sent_once = false;
    int index = 1;
    int fd;
    struct sockaddr_un address;
    struct termios saved;
    struct termios raw;
    bool have_termios = false;
    kmx_receiver *receiver = NULL;
    kmx_render *render = NULL;
    kmx_predictor *predictor = NULL;
    kmx_framer framer;
    kmx_grid display;
    unsigned char buffer[65536];
    int rows;
    int cols;
    int exit_code = 0;

    while (index < argc) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--no-predict") == 0) {
            predict = false;
        } else if (strcmp(argv[index], "--dump") == 0) {
            dump = true;
        } else if (strcmp(argv[index], "--send") == 0 && index + 1 < argc) {
            send_text = argv[++index];
        } else if (strcmp(argv[index], "--seconds") == 0 && index + 1 < argc) {
            run_seconds = atoi(argv[++index]);
        } else {
            fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict] [--dump]\n");
            return 2;
        }
        index++;
    }
    if (!socket_path) {
        fprintf(stderr, "usage: kmx-attach --socket PATH [--no-predict] [--dump]\n");
        return 2;
    }

    get_size(&rows, &cols);
    if (strlen(socket_path) >= sizeof address.sun_path) {
        fprintf(stderr, "kmx-attach: socket path too long\n");
        return 1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "kmx-attach: socket: %s\n", strerror(errno));
        return 1;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path));
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "kmx-attach: connect: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (kmx_receiver_create(&receiver, rows, cols) != KMX_OK ||
        kmx_render_create(&render) != KMX_OK ||
        kmx_predictor_create(&predictor) != KMX_OK ||
        kmx_grid_init(&display, rows, cols) != KMX_OK) {
        fprintf(stderr, "kmx-attach: out of memory\n");
        return 1;
    }
    kmx_framer_init(&framer);

    {
        unsigned char hello[4];
        hello[0] = (unsigned char)(rows >> 8);
        hello[1] = (unsigned char)rows;
        hello[2] = (unsigned char)(cols >> 8);
        hello[3] = (unsigned char)cols;
        send_message(fd, KMX_MSG_HELLO, hello, sizeof hello);
        send_message(fd, KMX_MSG_RESIZE, hello, sizeof hello);
    }

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
            unsigned char payload[4];
            resize_pending = 0;
            get_size(&rows, &cols);
            payload[0] = (unsigned char)(rows >> 8);
            payload[1] = (unsigned char)rows;
            payload[2] = (unsigned char)(cols >> 8);
            payload[3] = (unsigned char)cols;
            send_message(fd, KMX_MSG_RESIZE, payload, sizeof payload);
            /* The screen is about to be described differently; anything
             * predicted about the old one is void. */
            kmx_predictor_reset(predictor);
            kmx_render_invalidate(render);
        }

        descriptors[0].fd = fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        /* In dump mode the local terminal is not the point, so stdin is left
         * alone entirely: this exists to be driven by a test, not a person. */
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
            if (send_message(
                    fd, KMX_MSG_INPUT, send_text, strlen(send_text)) != 0) {
                break;
            }
            if (predict &&
                kmx_predictor_type(predictor, send_text, strlen(send_text))) {
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
                if (type == KMX_MSG_CELLS) {
                    uint64_t sequence = 0;
                    if (kmx_receiver_apply(receiver, payload, size, &sequence) == KMX_OK) {
                        unsigned char ack[8];
                        size_t position;
                        for (position = 0; position < 8; position++) {
                            ack[position] =
                                (unsigned char)(sequence >> (8 * (7 - position)));
                        }
                        send_message(fd, KMX_MSG_ACK, ack, sizeof ack);
                        if (predict) {
                            kmx_predictor_reconcile(
                                predictor, kmx_receiver_grid(receiver));
                        }
                        redraw = true;
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
                if (send_message(fd, KMX_MSG_INPUT, buffer, (size_t)count) != 0) break;
                if (predict &&
                    kmx_predictor_type(predictor, buffer, (size_t)count)) {
                    redraw = true;
                }
            } else if (count == 0) {
                if (dump) break;
            }
        }

        if (redraw) {
            kmx_buffer painted;
            if (kmx_grid_copy(&display, kmx_receiver_grid(receiver)) != KMX_OK) break;
            if (predict) kmx_predictor_overlay(predictor, &display);
            kmx_buffer_init(&painted);
            if (kmx_render_frame(render, &display, &painted) == KMX_OK) {
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
    kmx_framer_free(&framer);
    kmx_grid_free(&display);
    kmx_predictor_free(predictor);
    kmx_render_free(render);
    kmx_receiver_free(receiver);
    close(fd);
    return exit_code;
}
