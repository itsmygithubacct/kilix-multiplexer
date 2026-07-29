/* kmx-serve — run one or more panes and serve the session over a socket.
 *
 * Each pane has its own PTY and its own synchroniser, and the server owns the
 * geometry, so every client agrees about the arrangement without negotiating.
 * What crosses the socket is the screen each pane should be showing plus a few
 * dozen bytes saying where the panes are - never a picture of the chrome
 * around them.
 *
 *   kmx-serve --socket PATH [--split horizontal|vertical]
 *             [--rows N] [--cols N]
 *             --pane 'COMMAND' [--pane 'COMMAND' ...]
 *   kmx-serve --socket PATH [...] -- COMMAND [ARG...]      (one pane)
 *
 * One client at a time; multi-client attach is not implemented yet. */
#define _GNU_SOURCE

#include "kilix_mux.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    kmx_sync *sync;
    int master;
    pid_t child;
    bool alive;
    const char *command;
} pane;

static volatile sig_atomic_t stop_requested;

static void
handle_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t
now_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
create_listener(const char *path) {
    struct sockaddr_un address;
    int fd;
    if (strlen(path) >= sizeof address.sun_path) {
        fprintf(stderr, "kmx-serve: socket path too long\n");
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path));
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        chmod(path, 0600) != 0 ||
        listen(fd, 4) != 0) {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool
peer_is_owner(int fd) {
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t size = sizeof credentials;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) return false;
    return credentials.uid == geteuid();
#else
    (void)fd;
    return true;
#endif
}

static int
send_framed(int fd, kmx_message_type type, const void *payload, size_t size) {
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
usage(void) {
    fputs("usage: kmx-serve --socket PATH [--split horizontal|vertical]\n"
          "                 [--rows N] [--cols N]\n"
          "                 --pane 'COMMAND' [--pane 'COMMAND' ...]\n"
          "       kmx-serve --socket PATH [...] -- COMMAND [ARG...]\n", stderr);
}

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *commands[KMX_MAX_PANES];
    char *const *single = NULL;
    size_t command_count = 0;
    bool vertical = false;
    int rows = 24;
    int cols = 80;
    int index = 1;
    int listener = -1;
    int client = -1;
    pane panes[KMX_MAX_PANES];
    kmx_layout layout;
    kmx_layout announced;
    kmx_framer framer;
    unsigned char buffer[65536];
    size_t focused = 0;
    size_t count;
    size_t slot;

    memset(panes, 0, sizeof panes);
    while (index < argc && strcmp(argv[index], "--") != 0) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--cols") == 0 && index + 1 < argc) {
            cols = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--split") == 0 && index + 1 < argc) {
            vertical = strcmp(argv[++index], "vertical") == 0;
        } else if (strcmp(argv[index], "--pane") == 0 && index + 1 < argc) {
            if (command_count >= KMX_MAX_PANES) {
                fprintf(stderr, "kmx-serve: too many panes\n");
                return 2;
            }
            commands[command_count++] = argv[++index];
        } else {
            usage();
            return 2;
        }
        index++;
    }
    if (index < argc && strcmp(argv[index], "--") == 0 && index + 1 < argc) {
        single = &argv[index + 1];
    }
    if (!socket_path || rows <= 0 || cols <= 0 ||
        (command_count == 0 && !single)) {
        usage();
        return 2;
    }
    count = single ? 1 : command_count;

    kmx_layout_init(&layout, rows, cols);
    if (kmx_layout_arrange(&layout, count, vertical) != KMX_OK) {
        fprintf(stderr, "kmx-serve: %d by %d is too small for %zu panes\n",
                rows, cols, count);
        return 1;
    }
    memset(&announced, 0, sizeof announced);
    kmx_framer_init(&framer);

    listener = create_listener(socket_path);
    if (listener < 0) {
        fprintf(stderr, "kmx-serve: listen: %s\n", strerror(errno));
        return 1;
    }

    for (slot = 0; slot < count; slot++) {
        const kmx_pane_info *info = &layout.panes[slot];
        struct winsize size;
        pane *item = &panes[slot];
        item->command = single ? single[0] : commands[slot];
        snprintf(
            layout.panes[slot].title, KMX_TITLE_MAX, "%zu: %s",
            slot + 1, item->command);
        if (kmx_sync_create(&item->sync, info->rows, info->cols) != KMX_OK) {
            fprintf(stderr, "kmx-serve: out of memory\n");
            return 1;
        }
        memset(&size, 0, sizeof size);
        size.ws_row = (unsigned short)info->rows;
        size.ws_col = (unsigned short)info->cols;
        item->child = forkpty(&item->master, NULL, NULL, &size);
        if (item->child < 0) {
            fprintf(stderr, "kmx-serve: forkpty: %s\n", strerror(errno));
            return 1;
        }
        if (item->child == 0) {
            setenv("TERM", "xterm-256color", 1);
            if (single) {
                execvp(single[0], single);
            } else {
                execl("/bin/sh", "sh", "-c", item->command, (char *)NULL);
            }
            _exit(127);
        }
        item->alive = true;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);

    while (!stop_requested) {
        struct pollfd descriptors[2 + KMX_MAX_PANES];
        nfds_t descriptor_count = 0;
        size_t pane_index[KMX_MAX_PANES];
        bool any_alive = false;
        int ready;

        descriptors[descriptor_count].fd = listener;
        descriptors[descriptor_count].events = client < 0 ? POLLIN : 0;
        descriptors[descriptor_count].revents = 0;
        descriptor_count++;
        descriptors[descriptor_count].fd = client;
        descriptors[descriptor_count].events = client >= 0 ? POLLIN : 0;
        descriptors[descriptor_count].revents = 0;
        descriptor_count++;
        for (slot = 0; slot < count; slot++) {
            if (!panes[slot].alive) continue;
            any_alive = true;
            pane_index[descriptor_count - 2] = slot;
            descriptors[descriptor_count].fd = panes[slot].master;
            descriptors[descriptor_count].events = POLLIN;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }

        ready = poll(descriptors, descriptor_count, KMX_SEND_INTERVAL_MIN_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (descriptors[0].revents & POLLIN) {
            int accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (accepted >= 0) {
                if (!peer_is_owner(accepted)) {
                    close(accepted);
                } else {
                    client = accepted;
                    kmx_framer_free(&framer);
                    kmx_framer_init(&framer);
                    /* A new client holds nothing.  Only the baselines reset:
                     * each pane's history lives in its terminal model and must
                     * survive a client coming and going. */
                    for (slot = 0; slot < count; slot++) {
                        kmx_sync_reset_baseline(panes[slot].sync);
                    }
                    memset(&announced, 0, sizeof announced);
                }
            }
        }

        if (client >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t received = read(client, buffer, sizeof buffer);
            if (received <= 0 ||
                kmx_framer_push(&framer, buffer, (size_t)received) != KMX_OK) {
                close(client);
                client = -1;
            }
            while (client >= 0) {
                kmx_message_type type;
                const unsigned char *payload;
                size_t size;
                bool available = false;
                if (kmx_framer_next(
                        &framer, &available, &type, &payload, &size) != KMX_OK) {
                    close(client);
                    client = -1;
                    break;
                }
                if (!available) break;
                if (type == KMX_MSG_INPUT) {
                    if (focused < count && panes[focused].alive) {
                        (void)write_all(panes[focused].master, payload, size);
                    }
                } else if (type == KMX_MSG_FOCUS && size >= 1) {
                    size_t wanted = payload[0];
                    if (wanted < count) {
                        focused = wanted;
                        for (slot = 0; slot < count; slot++) {
                            layout.panes[slot].focused = slot == focused;
                        }
                        layout.generation++;
                    }
                } else if (type == KMX_MSG_RESIZE && size == 4) {
                    int new_rows = (payload[0] << 8) | payload[1];
                    int new_cols = (payload[2] << 8) | payload[3];
                    if (new_rows > 0 && new_cols > 0 &&
                        new_rows <= KMX_MAX_DIMENSION &&
                        new_cols <= KMX_MAX_DIMENSION) {
                        kmx_layout candidate = layout;
                        candidate.rows = new_rows;
                        candidate.cols = new_cols;
                        if (kmx_layout_arrange(&candidate, count, vertical) == KMX_OK) {
                            rows = new_rows;
                            cols = new_cols;
                            layout = candidate;
                            for (slot = 0; slot < count; slot++) {
                                struct winsize size_request;
                                layout.panes[slot].focused = slot == focused;
                                memset(&size_request, 0, sizeof size_request);
                                size_request.ws_row =
                                    (unsigned short)layout.panes[slot].rows;
                                size_request.ws_col =
                                    (unsigned short)layout.panes[slot].cols;
                                ioctl(panes[slot].master, TIOCSWINSZ, &size_request);
                                kmx_sync_resize(
                                    panes[slot].sync,
                                    layout.panes[slot].rows,
                                    layout.panes[slot].cols);
                            }
                        }
                    }
                } else if (type == KMX_MSG_ACK && size >= 9) {
                    uint64_t sequence = 0;
                    size_t position;
                    size_t which = payload[0];
                    for (position = 1; position < 9; position++) {
                        sequence = (sequence << 8) | payload[position];
                    }
                    if (which < count) kmx_sync_ack(panes[which].sync, sequence);
                }
                kmx_framer_consume(&framer);
            }
        }

        for (slot = 0; slot + 2 < (size_t)descriptor_count; slot++) {
            size_t which = pane_index[slot];
            if (!(descriptors[slot + 2].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            {
                ssize_t received = read(panes[which].master, buffer, sizeof buffer);
                if (received > 0) {
                    kmx_sync_feed(panes[which].sync, buffer, (size_t)received);
                } else if (received == 0 || (errno != EINTR && errno != EAGAIN)) {
                    panes[which].alive = false;
                }
            }
        }

        if (client >= 0 && !kmx_layout_equal(&layout, &announced)) {
            kmx_buffer wire;
            kmx_buffer_init(&wire);
            if (kmx_layout_encode(&layout, &wire) == KMX_OK) {
                if (send_framed(client, KMX_MSG_LAYOUT, wire.data, wire.size) != 0) {
                    close(client);
                    client = -1;
                } else {
                    announced = layout;
                }
            }
            kmx_buffer_free(&wire);
        }

        for (slot = 0; slot < count && client >= 0; slot++) {
            kmx_buffer message;
            kmx_sync_info info;
            bool produced = false;
            kmx_buffer_init(&message);
            /* The pane index precedes its cell message, so the client can
             * route it without inspecting the contents. */
            if (kmx_buffer_append(&message, &(unsigned char){(unsigned char)slot}, 1)
                    == KMX_OK &&
                kmx_sync_poll(
                    panes[slot].sync, now_millis(), &message, &produced, &info) == KMX_OK &&
                produced) {
                if (send_framed(
                        client, KMX_MSG_CELLS, message.data, message.size) != 0) {
                    close(client);
                    client = -1;
                }
            }
            kmx_buffer_free(&message);
        }

        if (!any_alive) {
            for (slot = 0; slot < count; slot++) {
                int status;
                if (panes[slot].child > 0) waitpid(panes[slot].child, &status, WNOHANG);
            }
            if (client >= 0) (void)send_framed(client, KMX_MSG_EXIT, NULL, 0);
            break;
        }
    }

    for (slot = 0; slot < count; slot++) {
        if (panes[slot].master >= 0) close(panes[slot].master);
        kmx_sync_free(panes[slot].sync);
    }
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    unlink(socket_path);
    kmx_framer_free(&framer);
    return 0;
}
