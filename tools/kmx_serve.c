/* kmx-serve — run a command under a PTY and serve its screen over a socket.
 *
 * The first cut of the session server: one pane, one client at a time, no
 * layout plane and no graphics plane yet.  What it does have is the property
 * the whole design is for - what goes over the socket is the screen the client
 * should be showing, not the bytes the pane produced. */
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

/* Owner-only, in a directory the caller chose.  Matches the posture the rest
 * of this stack already uses for local sockets. */
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

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    int rows = 24;
    int cols = 80;
    int index = 1;
    int listener = -1;
    int client = -1;
    int master = -1;
    pid_t child;
    kmx_sync *sync = NULL;
    kmx_framer framer;
    unsigned char buffer[65536];
    int exit_code = 0;
    bool child_alive = true;

    while (index < argc && strcmp(argv[index], "--") != 0) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--cols") == 0 && index + 1 < argc) {
            cols = atoi(argv[++index]);
        } else {
            fprintf(stderr, "usage: kmx-serve --socket PATH [--rows N] [--cols N]"
                            " -- COMMAND [ARG...]\n");
            return 2;
        }
        index++;
    }
    if (!socket_path || index >= argc || strcmp(argv[index], "--") != 0 ||
        index + 1 >= argc || rows <= 0 || cols <= 0) {
        fprintf(stderr, "usage: kmx-serve --socket PATH [--rows N] [--cols N]"
                        " -- COMMAND [ARG...]\n");
        return 2;
    }
    index++;

    if (kmx_sync_create(&sync, rows, cols) != KMX_OK) {
        fprintf(stderr, "kmx-serve: out of memory\n");
        return 1;
    }
    kmx_framer_init(&framer);

    listener = create_listener(socket_path);
    if (listener < 0) {
        fprintf(stderr, "kmx-serve: listen: %s\n", strerror(errno));
        return 1;
    }

    {
        struct winsize size;
        memset(&size, 0, sizeof size);
        size.ws_row = (unsigned short)rows;
        size.ws_col = (unsigned short)cols;
        child = forkpty(&master, NULL, NULL, &size);
    }
    if (child < 0) {
        fprintf(stderr, "kmx-serve: forkpty: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        execvp(argv[index], &argv[index]);
        _exit(127);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);

    while (!stop_requested) {
        struct pollfd descriptors[3];
        kmx_buffer message;
        kmx_sync_info info;
        bool produced = false;
        int ready;

        descriptors[0].fd = listener;
        descriptors[0].events = client < 0 ? POLLIN : 0;
        descriptors[0].revents = 0;
        descriptors[1].fd = master;
        descriptors[1].events = child_alive ? POLLIN : 0;
        descriptors[1].revents = 0;
        descriptors[2].fd = client;
        descriptors[2].events = client >= 0 ? POLLIN : 0;
        descriptors[2].revents = 0;

        ready = poll(descriptors, 3, KMX_SEND_INTERVAL_MIN_MS);
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
                    /* A newly attached client holds nothing, so the next
                     * message must be a full screen.  Only the baseline is
                     * reset: the pane's history lives in the terminal model
                     * and recreating it would throw away everything printed
                     * before the client arrived. */
                    kmx_sync_reset_baseline(sync);
                }
            }
        }

        if (child_alive && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t count = read(master, buffer, sizeof buffer);
            if (count > 0) {
                kmx_sync_feed(sync, buffer, (size_t)count);
            } else if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
                child_alive = false;
            }
        }

        if (client >= 0 && (descriptors[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t count = read(client, buffer, sizeof buffer);
            if (count <= 0) {
                close(client);
                client = -1;
            } else if (kmx_framer_push(&framer, buffer, (size_t)count) != KMX_OK) {
                close(client);
                client = -1;
            } else {
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
                    switch (type) {
                        case KMX_MSG_INPUT:
                            if (child_alive) (void)write_all(master, payload, size);
                            break;
                        case KMX_MSG_RESIZE:
                            if (size == 4) {
                                struct winsize size_request;
                                int new_rows = (payload[0] << 8) | payload[1];
                                int new_cols = (payload[2] << 8) | payload[3];
                                if (new_rows > 0 && new_cols > 0 &&
                                    new_rows <= KMX_MAX_DIMENSION &&
                                    new_cols <= KMX_MAX_DIMENSION) {
                                    rows = new_rows;
                                    cols = new_cols;
                                    memset(&size_request, 0, sizeof size_request);
                                    size_request.ws_row = (unsigned short)rows;
                                    size_request.ws_col = (unsigned short)cols;
                                    ioctl(master, TIOCSWINSZ, &size_request);
                                    kmx_sync_resize(sync, rows, cols);
                                }
                            }
                            break;
                        case KMX_MSG_ACK:
                            if (size >= 1) {
                                uint64_t sequence = 0;
                                size_t position;
                                for (position = 0; position < size && position < 8;
                                     position++) {
                                    sequence = (sequence << 8) | payload[position];
                                }
                                kmx_sync_ack(sync, sequence);
                            }
                            break;
                        case KMX_MSG_HELLO:
                            break;
                        default:
                            break;
                    }
                    kmx_framer_consume(&framer);
                }
            }
        }

        kmx_buffer_init(&message);
        if (kmx_sync_poll(sync, now_millis(), &message, &produced, &info) == KMX_OK &&
            produced && client >= 0) {
            kmx_buffer framed;
            kmx_buffer_init(&framed);
            if (kmx_frame_encode(
                    KMX_MSG_CELLS, message.data, message.size, &framed) == KMX_OK) {
                if (write_all(client, framed.data, framed.size) != 0) {
                    close(client);
                    client = -1;
                }
            }
            kmx_buffer_free(&framed);
        }
        kmx_buffer_free(&message);

        if (!child_alive) {
            int status;
            if (waitpid(child, &status, WNOHANG) == child || errno == ECHILD) {
                if (client >= 0) {
                    kmx_buffer framed;
                    kmx_buffer_init(&framed);
                    if (kmx_frame_encode(KMX_MSG_EXIT, NULL, 0, &framed) == KMX_OK) {
                        (void)write_all(client, framed.data, framed.size);
                    }
                    kmx_buffer_free(&framed);
                }
                break;
            }
        }
    }

    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    if (master >= 0) close(master);
    unlink(socket_path);
    kmx_framer_free(&framer);
    kmx_sync_free(sync);
    return exit_code;
}
