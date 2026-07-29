/* kmx-serve — run one or more panes and serve the session to several clients.
 *
 * Each pane has one terminal model; each client has its own synchronisation
 * state over it.  That split is the important one: what the pane shows is
 * shared, what a receiver has acknowledged is not, so two clients watching one
 * pane are each sent the difference between the screen and what *they* hold,
 * and neither pays for the other falling behind.
 *
 *   kmx-serve --socket PATH [--split horizontal|vertical]
 *             [--rows N] [--cols N]
 *             --pane 'COMMAND' [--pane 'COMMAND' ...]
 *   kmx-serve --socket PATH [...] -- COMMAND [ARG...]      (one pane)
 *
 * Clients attach for control or for viewing.  A viewer's input is dropped by
 * the server rather than hidden by the client, because a client choosing not
 * to send input is not a security property. */
#define _GNU_SOURCE

#include "kilix_mux.h"
#include "endpoint.h"
#include "kmx_pixel.h"
#include "kmx_tls.h"

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

#define KMX_MAX_CLIENTS 8

/* Accepts allowed per second before new connections are dropped.
 *
 * Eight client slots fill on a first-come basis, so anything that can connect
 * can also occupy them, and a peer that fails the token check can retry
 * without limit.  A bucket does not stop a determined local process - it
 * shares the user's privileges anyway - but it does stop a loop from turning
 * a wrong guess into an unbounded one, and it bounds what a reachable port
 * costs when someone finds it. */
#define KMX_ACCEPT_BURST 16
#define KMX_ACCEPT_PER_SECOND 8

/* HELLO role byte. */
#define KMX_ROLE_CONTROL 0u
#define KMX_ROLE_VIEW 1u

/* 128 bits, hex encoded.  Long enough that guessing is not a strategy and
 * short enough to paste. */
#define KMX_TOKEN_HEX 32

typedef struct {
    kmx_term *term;
    int master;
    pid_t child;
    bool alive;
    const char *command;
} pane;

/* KMX_CLIENT_QUEUE_LIMIT bounds what is held for one client.  A client that
 * stops reading must not be able to stall the panes or the other clients, so
 * its socket is non-blocking and its backlog is bounded; past the limit it is
 * disconnected rather than buffered.
 *
 * The cell plane needs no queue at all beyond one message.  Because every
 * message is a diff against what the client last acknowledged, a client that
 * is behind is better served by the NEXT diff than by the one it missed - so
 * a new cell message is only produced once the previous one has actually
 * gone. That is the same "never queue, always diff" property the
 * synchroniser has, applied to the socket. */
#define KMX_CLIENT_QUEUE_LIMIT (4u * 1024u * 1024u)

typedef struct {
    int fd;
    kmx_tls_session *tls;
    bool control;
    bool greeted;
    kmx_motion *motion;   /* per client: what IT has been shown */
    kmx_audio *audio;     /* per client: its own rate allowance */
    int rows;
    int cols;
    kmx_sync *sync[KMX_MAX_PANES];
    kmx_image_cache *holds;
    kmx_layout announced;
    kmx_framer framer;
    kmx_buffer out;
    size_t out_offset;
} client;

/* One place that knows whether this connection is wrapped, so every other
 * call site reads and writes without caring. */
static long
client_recv(client *item, void *data, size_t size) {
    if (item->tls) return kmx_tls_read(item->tls, data, size);
    return recv(item->fd, data, size, MSG_DONTWAIT);
}

static long
client_send(client *item, const void *data, size_t size) {
    if (item->tls) return kmx_tls_write(item->tls, data, size);
    return send(item->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* Queue a message.  Returns -1 when the client has fallen too far behind to
 * be worth keeping. */
static int
client_queue(client *item, kmx_message_type type, const void *payload, size_t size) {
    kmx_buffer framed;
    int result = 0;
    kmx_buffer_init(&framed);
    if (kmx_frame_encode(type, payload, size, &framed) != KMX_OK) {
        kmx_buffer_free(&framed);
        return -1;
    }
    if (item->out.size - item->out_offset + framed.size > KMX_CLIENT_QUEUE_LIMIT) {
        result = -1;
    } else if (kmx_buffer_append(&item->out, framed.data, framed.size) != KMX_OK) {
        result = -1;
    }
    kmx_buffer_free(&framed);
    return result;
}

/* Push what will go without blocking.  Anything left waits for POLLOUT. */
static int
client_flush(client *item) {
    while (item->out_offset < item->out.size) {
        long count = client_send(
            item,
            item->out.data + item->out_offset,
            item->out.size - item->out_offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (count == 0) return -1;
        item->out_offset += (size_t)count;
    }
    item->out_offset = 0;
    item->out.size = 0;
    return 0;
}

static bool
client_idle(const client *item) {
    return item->out.size == item->out_offset;
}

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

/* Compared in constant time: a token check that returns early on the first
 * wrong character tells an attacker how much of it was right. */
static bool
token_matches(const char *expected, const char *offered, size_t offered_size) {
    size_t index;
    unsigned char difference = 0;
    size_t expected_size = strlen(expected);
    if (offered_size != expected_size) return false;
    for (index = 0; index < expected_size; index++) {
        difference |= (unsigned char)(expected[index] ^ offered[index]);
    }
    return difference == 0;
}

static bool
mint_token(char *out, size_t size) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[KMX_TOKEN_HEX / 2];
    size_t index;
    int fd;
    if (size <= KMX_TOKEN_HEX) return false;
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    {
        size_t got = 0;
        while (got < sizeof raw) {
            ssize_t count = read(fd, raw + got, sizeof raw - got);
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                close(fd);
                return false;
            }
            got += (size_t)count;
        }
    }
    close(fd);
    for (index = 0; index < sizeof raw; index++) {
        out[index * 2] = hex[raw[index] >> 4];
        out[index * 2 + 1] = hex[raw[index] & 15];
    }
    out[KMX_TOKEN_HEX] = '\0';
    return true;
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

static void
client_release(client *item, size_t pane_count) {
    size_t slot;
    kmx_tls_session_free(item->tls);
    item->tls = NULL;
    if (item->fd >= 0) close(item->fd);
    for (slot = 0; slot < pane_count; slot++) {
        kmx_sync_free(item->sync[slot]);
    }
    kmx_image_cache_free(item->holds);
    kmx_motion_free(item->motion);
    kmx_audio_free(item->audio);
    kmx_framer_free(&item->framer);
    kmx_buffer_free(&item->out);
    memset(item, 0, sizeof *item);
    item->fd = -1;
}

static void
usage(void) {
    fputs("usage: kmx-serve --socket PATH [--split horizontal|vertical]\n"
          "                 [--rows N] [--cols N]\n"
          "                 --pane 'COMMAND' [--pane 'COMMAND' ...]\n"
          "       kmx-serve --socket PATH [...] -- COMMAND [ARG...]\n"
          "\n"
          "       kmx-serve --socket PATH --pixel-pane 'COMMAND'\n"
          "                 [--pixel-size WxH] [--pixel-fps N] [--pixel-budget BPS]\n"
          "\n"
          "  --socket accepts a path or HOST:PORT.  TCP binds loopback unless\n"
          "  --lan is given; reach a session across a network over SSH.\n"
          "  --pixel-pane runs an X client on a private display and streams its\n"
          "  frames; it never touches the display the operator is using.\n"
          "  --audio-source runs a command that writes raw 16-bit PCM to stdout.\n"
          "  --lan requires a token; one is generated and printed if not given.\n"
          "  A reachable bind encrypts by default and prints a fingerprint the\n"
          "  client must be given; --no-tls turns that off, which is only\n"
          "  reasonable inside a tunnel.\n",
          stderr);
}

int
main(int argc, char **argv) {
    const char *socket_path = NULL;
    kmx_endpoint endpoint;
    bool allow_public = false;
    char token[KMX_TOKEN_HEX + 1];
    bool require_token = false;
    bool use_tls = false;
    bool refuse_tls = false;
    kmx_tls_server *tls = NULL;
    char fingerprint[KMX_TLS_FINGERPRINT_HEX + 1];
    const char *commands[KMX_MAX_PANES];
    const char *pixel_command = NULL;
    kmx_pixel_pane pixel;
    bool pixel_running = false;
    int pixel_width = 640;
    int pixel_height = 480;
    int pixel_fps = 10;
    uint32_t pixel_budget = 0;
    const char *audio_command = NULL;
    int audio_rate = 48000;
    int audio_channels = 2;
    uint32_t audio_budget = 0;
    int audio_fd = -1;
    pid_t audio_child = -1;
    unsigned char *audio_block = NULL;
    size_t audio_block_bytes = 0;
    size_t audio_filled = 0;
    uint64_t audio_clock = 0;
    char *const *single = NULL;
    size_t command_count = 0;
    bool vertical = false;
    int rows = 24;
    int cols = 80;
    int index = 1;
    int listener = -1;
    pane panes[KMX_MAX_PANES];
    client clients[KMX_MAX_CLIENTS];
    kmx_layout layout;
    unsigned char buffer[65536];
    size_t focused = 0;
    unsigned accept_allowance = KMX_ACCEPT_BURST;
    uint64_t accept_refilled = 0;
    unsigned long accepts_refused = 0;
    size_t count;
    size_t slot;
    size_t which;

    memset(panes, 0, sizeof panes);
    memset(clients, 0, sizeof clients);
    for (which = 0; which < KMX_MAX_CLIENTS; which++) clients[which].fd = -1;

    while (index < argc && strcmp(argv[index], "--") != 0) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--cols") == 0 && index + 1 < argc) {
            cols = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--listen") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--pixel-pane") == 0 && index + 1 < argc) {
            pixel_command = argv[++index];
        } else if (strcmp(argv[index], "--pixel-size") == 0 && index + 1 < argc) {
            if (sscanf(argv[++index], "%dx%d", &pixel_width, &pixel_height) != 2) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[index], "--pixel-fps") == 0 && index + 1 < argc) {
            pixel_fps = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--pixel-budget") == 0 && index + 1 < argc) {
            pixel_budget = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--audio-source") == 0 && index + 1 < argc) {
            audio_command = argv[++index];
        } else if (strcmp(argv[index], "--audio-rate") == 0 && index + 1 < argc) {
            audio_rate = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--audio-channels") == 0 && index + 1 < argc) {
            audio_channels = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--audio-budget") == 0 && index + 1 < argc) {
            audio_budget = (uint32_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--lan") == 0) {
            allow_public = true;
        } else if (strcmp(argv[index], "--tls") == 0) {
            use_tls = true;
        } else if (strcmp(argv[index], "--no-tls") == 0) {
            refuse_tls = true;
        } else if (strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            const char *given = argv[++index];
            if (strlen(given) < 16 || strlen(given) > KMX_TOKEN_HEX) {
                fprintf(stderr, "kmx-serve: a token must be 16 to %d characters\n",
                        KMX_TOKEN_HEX);
                return 2;
            }
            snprintf(token, sizeof token, "%s", given);
            require_token = true;
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
        (command_count == 0 && !single && !pixel_command)) {
        usage();
        return 2;
    }
    /* A pixel pane is the whole session for now: mixing pixel and text panes
     * in one layout is a later refinement, not part of proving the plane. */
    count = pixel_command ? 0 : (single ? 1 : command_count);

    kmx_layout_init(&layout, rows, cols);
    if (count && kmx_layout_arrange(&layout, count, vertical) != KMX_OK) {
        fprintf(stderr, "kmx-serve: %d by %d is too small for %zu panes\n",
                rows, cols, count);
        return 1;
    }

    if (!kmx_endpoint_parse(socket_path, &endpoint)) {
        fprintf(stderr, "kmx-serve: cannot make sense of '%s'\n", socket_path);
        return 2;
    }
    /* A token is mandatory once the socket is genuinely reachable from a
     * network, and there is no way to turn it off: the alternative is handing
     * a shell to whoever can route to the port.
     *
     * The requirement follows the address, not the flag.  --lan on a loopback
     * address reaches nobody new, and demanding a token there would be a
     * requirement the operator never asked for. */
    if (endpoint.kind == KMX_ENDPOINT_TCP &&
        !kmx_endpoint_is_loopback(&endpoint) && !require_token) {
        if (!mint_token(token, sizeof token)) {
            fprintf(stderr, "kmx-serve: could not generate a token\n");
            return 1;
        }
        require_token = true;
    }
    listener = kmx_endpoint_listen(&endpoint, allow_public);
    if (listener < 0) {
        if (errno == EPERM) {
            fprintf(stderr,
                "kmx-serve: %s is not a loopback address.  Reach a session "
                "across a network through an SSH tunnel, or pass --lan to "
                "bind it anyway.\n", endpoint.host);
        } else {
            fprintf(stderr, "kmx-serve: listen: %s\n", strerror(errno));
        }
        return 1;
    }
    /* A reachable bind encrypts by default.  The old behaviour - cleartext
     * unless asked otherwise - made the safe choice the one you had to
     * remember, which is the wrong way round for something carrying a
     * terminal.  Turning it off is still possible and now has to be said out
     * loud, because over an SSH tunnel it is a reasonable thing to want. */
    if (endpoint.kind == KMX_ENDPOINT_TCP &&
        !kmx_endpoint_is_loopback(&endpoint) && !refuse_tls) {
        use_tls = true;
    }
    if (use_tls && refuse_tls) {
        fprintf(stderr, "kmx-serve: --tls and --no-tls disagree\n");
        return 2;
    }

    if (use_tls) {
        if (endpoint.kind != KMX_ENDPOINT_TCP) {
            fprintf(stderr, "kmx-serve: --tls applies to a TCP socket\n");
            return 2;
        }
        tls = kmx_tls_server_create(fingerprint, sizeof fingerprint);
        if (!tls) {
            fprintf(stderr, "kmx-serve: could not set up TLS\n");
            return 1;
        }
    }

    /* Printed whenever a token is required, however it came to be required:
     * a token the operator is never told is a session nobody can attach to. */
    if (require_token) {
        fprintf(stderr,
            "kmx-serve: listening on %s:%d\n"
            "  Attach with:  kmx-attach --socket %s:%d --token %s%s%s\n",
            endpoint.host, endpoint.port, endpoint.host, endpoint.port, token,
            use_tls ? " \\\n                  --tls-fingerprint " : "",
            use_tls ? fingerprint : "");
        if (endpoint.kind == KMX_ENDPOINT_TCP &&
            !kmx_endpoint_is_loopback(&endpoint)) {
            fprintf(stderr,
                "  Reachable from the network, and carried in the clear:\n"
                "  anyone who can watch the segment can read this session.\n"
                "  Use an SSH tunnel to a loopback port if that matters.\n");
        }
    }

    for (slot = 0; slot < count; slot++) {
        const kmx_pane_info *info = &layout.panes[slot];
        struct winsize size;
        pane *item = &panes[slot];
        item->command = single ? single[0] : commands[slot];
        snprintf(layout.panes[slot].title, KMX_TITLE_MAX, "%zu: %s",
                 slot + 1, item->command);
        if (kmx_term_create(&item->term, info->rows, info->cols) != KMX_OK) {
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

    if (pixel_command) {
        if (!kmx_pixel_start(
                &pixel, pixel_command, pixel_width, pixel_height, pixel_fps)) {
            fprintf(stderr, "kmx-serve: could not start a private display\n");
            return 1;
        }
        pixel_running = true;
        fprintf(stderr, "kmx-serve: pixel pane on display :%d, %dx%d\n",
                pixel.display, pixel_width, pixel_height);
    }

    if (audio_command) {
        int pipe_fds[2];
        if (audio_rate <= 0 || audio_rate > 384000 ||
            audio_channels <= 0 || audio_channels > 8 || pipe(pipe_fds) != 0) {
            fprintf(stderr, "kmx-serve: bad audio source\n");
            return 1;
        }
        audio_child = fork();
        if (audio_child < 0) {
            fprintf(stderr, "kmx-serve: fork: %s\n", strerror(errno));
            return 1;
        }
        if (audio_child == 0) {
            int null_fd = open("/dev/null", O_RDWR);
            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDOUT_FILENO);
            close(pipe_fds[1]);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > STDERR_FILENO) close(null_fd);
            }
            execl("/bin/sh", "sh", "-c", audio_command, (char *)NULL);
            _exit(127);
        }
        close(pipe_fds[1]);
        audio_fd = pipe_fds[0];
        {
            int flags = fcntl(audio_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(audio_fd, F_SETFL, flags | O_NONBLOCK);
        }
        /* Twenty milliseconds a block: small enough that a drop is a blink
         * rather than a stutter, large enough not to be all framing. */
        audio_block_bytes =
            (size_t)audio_rate * (size_t)audio_channels * 2u / 50u;
        audio_block = malloc(audio_block_bytes);
        if (!audio_block) {
            fprintf(stderr, "kmx-serve: out of memory\n");
            return 1;
        }
        fprintf(stderr, "kmx-serve: audio source, %d Hz, %d channel(s)\n",
                audio_rate, audio_channels);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);

    while (!stop_requested) {
        struct pollfd descriptors[1 + KMX_MAX_CLIENTS + KMX_MAX_PANES];
        nfds_t descriptor_count = 0;
        size_t client_at[KMX_MAX_CLIENTS];
        size_t pane_at[KMX_MAX_PANES];
        nfds_t client_first;
        nfds_t pane_first;
        nfds_t pane_descriptors;
        bool any_alive = false;
        int ready;

        descriptors[descriptor_count].fd = listener;
        descriptors[descriptor_count].events = POLLIN;
        descriptors[descriptor_count].revents = 0;
        descriptor_count++;
        client_first = descriptor_count;
        for (which = 0; which < KMX_MAX_CLIENTS; which++) {
            if (clients[which].fd < 0) continue;
            client_at[descriptor_count - client_first] = which;
            descriptors[descriptor_count].fd = clients[which].fd;
            descriptors[descriptor_count].events =
                (short)(POLLIN | (client_idle(&clients[which]) ? 0 : POLLOUT));
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        pane_first = descriptor_count;
        for (slot = 0; slot < count; slot++) {
            if (!panes[slot].alive) continue;
            any_alive = true;
            pane_at[descriptor_count - pane_first] = slot;
            descriptors[descriptor_count].fd = panes[slot].master;
            descriptors[descriptor_count].events = POLLIN;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }

        /* Captured before anything else is appended.  Deriving this from
         * descriptor_count later would sweep whatever came after the panes -
         * the frame pipe - as though it were a pane, index an uninitialised
         * slot, and write through it. */
        pane_descriptors = descriptor_count - pane_first;

        if (audio_fd >= 0) {
            descriptors[descriptor_count].fd = audio_fd;
            descriptors[descriptor_count].events = POLLIN;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }

        if (pixel_running) {
            descriptors[descriptor_count].fd = pixel.frames;
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
                /* Refill first, then spend.  A dropped connection is closed
                 * without a word: telling a flood apart from a mistake is not
                 * this layer's job, and answering costs more than ignoring. */
                uint64_t moment = now_millis();
                uint64_t since = moment - accept_refilled;
                if (since >= 1000) {
                    uint64_t earned = (since / 1000u) * KMX_ACCEPT_PER_SECOND;
                    accept_allowance = accept_allowance + earned > KMX_ACCEPT_BURST
                        ? KMX_ACCEPT_BURST : accept_allowance + earned;
                    accept_refilled = moment;
                }
                if (!accept_allowance) {
                    close(accepted);
                    accepted = -1;
                    accepts_refused++;
                } else {
                    accept_allowance--;
                }
            }
            if (accepted >= 0) {
                size_t free_slot = KMX_MAX_CLIENTS;
                for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                    if (clients[which].fd < 0) {
                        free_slot = which;
                        break;
                    }
                }
                kmx_endpoint_tune(accepted, &endpoint);
                /* Peer credentials exist only on a Unix socket; over TCP the
                 * transport is expected to be an SSH tunnel, which is what
                 * establishes who is on the other end. */
                if ((endpoint.kind == KMX_ENDPOINT_UNIX && !peer_is_owner(accepted)) ||
                    free_slot == KMX_MAX_CLIENTS) {
                    close(accepted);
                } else {
                    client *item = &clients[free_slot];
                    bool ok = true;
                    int flags = fcntl(accepted, F_GETFL, 0);
                    if (flags < 0 || fcntl(accepted, F_SETFL, flags | O_NONBLOCK) != 0) {
                        close(accepted);
                        continue;
                    }
                    memset(item, 0, sizeof *item);
                    if (tls) {
                        /* The handshake is blocking, so it is given a deadline:
                         * a peer that opens a connection and then says nothing
                         * must not be able to hold up the session. */
                        struct timeval limit = {.tv_sec = 5, .tv_usec = 0};
                        int blocking = fcntl(accepted, F_GETFL, 0);
                        if (blocking >= 0) {
                            fcntl(accepted, F_SETFL, blocking & ~O_NONBLOCK);
                        }
                        setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
                                   &limit, sizeof limit);
                        setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
                                   &limit, sizeof limit);
                        item->tls = kmx_tls_server_accept(tls, accepted);
                        if (!item->tls) {
                            close(accepted);
                            memset(item, 0, sizeof *item);
                            item->fd = -1;
                            continue;
                        }
                    }
                    item->fd = accepted;
                    item->control = true;
                    item->rows = rows;
                    item->cols = cols;
                    kmx_framer_init(&item->framer);
                    for (slot = 0; slot < count && ok; slot++) {
                        ok = kmx_sync_create_over(
                            &item->sync[slot], panes[slot].term) == KMX_OK;
                    }
                    if (ok) {
                        ok = kmx_image_cache_create(
                            &item->holds, 256, 32u * 1024u * 1024u) == KMX_OK;
                    }
                    if (ok && audio_command) {
                        ok = kmx_audio_create(
                            &item->audio, (uint32_t)audio_rate,
                            (uint8_t)audio_channels, audio_budget) == KMX_OK;
                    }
                    if (ok && pixel_running) {
                        /* Per client, like the cell baseline: what one client
                         * has been shown says nothing about another. */
                        ok = kmx_motion_create(&item->motion, pixel_budget) == KMX_OK;
                    }
                    if (!ok) client_release(item, count);
                }
            }
        }

        for (which = 0; which + client_first < pane_first; which++) {
            size_t id = client_at[which];
            client *item = &clients[id];
            short revents = descriptors[client_first + which].revents;
            if (item->fd < 0 || descriptors[client_first + which].fd != item->fd) continue;
            if (revents & POLLOUT) {
                if (client_flush(item) != 0) {
                    client_release(item, count);
                    continue;
                }
            }
            if (!(revents & (POLLIN | POLLHUP | POLLERR))) continue;
            {
                long received = client_recv(item, buffer, sizeof buffer);
                if (received == -1 && errno == EAGAIN) continue;
                if (received <= 0 ||
                    kmx_framer_push(&item->framer, buffer, (size_t)received) != KMX_OK) {
                    client_release(item, count);
                    continue;
                }
            }
            while (item->fd >= 0) {
                kmx_message_type type;
                const unsigned char *payload;
                size_t size;
                bool available = false;
                if (kmx_framer_next(
                        &item->framer, &available, &type, &payload, &size) != KMX_OK) {
                    client_release(item, count);
                    break;
                }
                if (!available) break;
                if (type == KMX_MSG_HELLO && size >= 4) {
                    if (require_token &&
                        !(size > 5 &&
                          token_matches(
                              token, (const char *)payload + 5, size - 5))) {
                        /* No error frame: a peer that cannot present the
                         * token learns only that the connection closed. */
                        client_release(item, count);
                        break;
                    }
                    item->greeted = true;
                    item->rows = (payload[0] << 8) | payload[1];
                    item->cols = (payload[2] << 8) | payload[3];
                    /* A client that does not say is a control client, so an
                     * older client keeps working. */
                    if (size >= 5) item->control = payload[4] == KMX_ROLE_CONTROL;
                } else if (require_token && !item->greeted) {
                    /* Nothing is accepted before a valid greeting. */
                    client_release(item, count);
                    break;
                } else if (type == KMX_MSG_INPUT) {
                    /* Enforced here, not in the client: a viewer that chose to
                     * send input still cannot reach the pane. */
                    if (item->control && focused < count && panes[focused].alive) {
                        (void)write_all(panes[focused].master, payload, size);
                    }
                } else if (type == KMX_MSG_FOCUS && size >= 1 && item->control) {
                    size_t wanted = payload[0];
                    if (wanted < count) {
                        focused = wanted;
                        for (slot = 0; slot < count; slot++) {
                            layout.panes[slot].focused = slot == focused;
                        }
                        layout.generation++;
                    }
                } else if (type == KMX_MSG_RESIZE && size == 4 && item->control) {
                    item->rows = (payload[0] << 8) | payload[1];
                    item->cols = (payload[2] << 8) | payload[3];
                } else if (type == KMX_MSG_ACK && size >= 9) {
                    uint64_t sequence = 0;
                    size_t position;
                    size_t pane_id = payload[0];
                    for (position = 1; position < 9; position++) {
                        sequence = (sequence << 8) | payload[position];
                    }
                    /* With the arrival time, so the round trip is
                     * measurable and the send interval can follow it. */
                    if (pane_id < count) {
                        kmx_sync_ack_at(item->sync[pane_id], sequence, now_millis());
                    }
                }
                kmx_framer_consume(&item->framer);
            }
        }

        /* The session is as large as the smallest control client can show, the
         * rule tmux settled on: anything larger and someone is looking at a
         * screen with content off the edge. */
        {
            int wanted_rows = 0;
            int wanted_cols = 0;
            for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                if (clients[which].fd < 0 || !clients[which].control) continue;
                if (clients[which].rows <= 0 || clients[which].cols <= 0) continue;
                if (!wanted_rows || clients[which].rows < wanted_rows) {
                    wanted_rows = clients[which].rows;
                }
                if (!wanted_cols || clients[which].cols < wanted_cols) {
                    wanted_cols = clients[which].cols;
                }
            }
            if (wanted_rows > 0 && wanted_cols > 0 &&
                (wanted_rows != rows || wanted_cols != cols)) {
                kmx_layout candidate = layout;
                candidate.rows = wanted_rows;
                candidate.cols = wanted_cols;
                if (kmx_layout_arrange(&candidate, count, vertical) == KMX_OK) {
                    rows = wanted_rows;
                    cols = wanted_cols;
                    layout = candidate;
                    for (slot = 0; slot < count; slot++) {
                        struct winsize size_request;
                        layout.panes[slot].focused = slot == focused;
                        memset(&size_request, 0, sizeof size_request);
                        size_request.ws_row = (unsigned short)layout.panes[slot].rows;
                        size_request.ws_col = (unsigned short)layout.panes[slot].cols;
                        ioctl(panes[slot].master, TIOCSWINSZ, &size_request);
                        kmx_term_resize(
                            panes[slot].term,
                            layout.panes[slot].rows, layout.panes[slot].cols);
                    }
                }
            }
        }

        for (which = 0; which < pane_descriptors; which++) {
            size_t id = pane_at[which];
            if (!(descriptors[pane_first + which].revents &
                  (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            {
                ssize_t received = read(panes[id].master, buffer, sizeof buffer);
                if (received > 0) {
                    /* Fed once, however many clients are watching. */
                    kmx_term_feed(panes[id].term, buffer, (size_t)received);
                } else if (received == 0 || (errno != EINTR && errno != EAGAIN)) {
                    panes[id].alive = false;
                }
            }
        }

        while (audio_fd >= 0) {
            /* Deliberately not named `count`: that is the pane count in this
             * scope, and shadowing it here would hand the wrong value to
             * client_release. */
            ssize_t received = read(
                audio_fd, audio_block + audio_filled,
                audio_block_bytes - audio_filled);
            if (received > 0) {
                audio_filled += (size_t)received;
                if (audio_filled < audio_block_bytes) continue;
                audio_filled = 0;
                for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                    client *item = &clients[which];
                    kmx_buffer message;
                    bool produced = false;
                    if (item->fd < 0 || !item->audio) continue;
                    kmx_buffer_init(&message);
                    if (kmx_audio_offer(
                            item->audio, audio_block, audio_block_bytes,
                            audio_clock, &message, &produced, NULL) == KMX_OK &&
                        produced) {
                        if (client_queue(
                                item, KMX_MSG_AUDIO,
                                message.data, message.size) != 0) {
                            kmx_buffer_free(&message);
                            client_release(item, count);
                            continue;
                        }
                    }
                    kmx_buffer_free(&message);
                }
                /* The clock advances by the block's own duration, so a
                 * receiver can tell a gap from a late delivery. */
                audio_clock += 20;
                continue;
            }
            if (received < 0 && errno == EINTR) continue;
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close(audio_fd);
            audio_fd = -1;
            break;
        }

        if (pixel_running) {
            bool closed = false;
            while (kmx_pixel_poll(&pixel, &closed)) {
                if (getenv("KMX_DEBUG_FRAMES")) {
                    fprintf(stderr, "kmx-serve: frame\n");
                    fflush(stderr);
                }
                for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                    client *item = &clients[which];
                    kmx_buffer message;
                    bool produced = false;
                    if (item->fd < 0 || !item->motion) continue;
                    kmx_buffer_init(&message);
                    if (kmx_buffer_append(
                            &message, &(unsigned char){0}, 1) == KMX_OK &&
                        kmx_motion_offer(
                            item->motion, pixel.frame, pixel.width, pixel.height,
                            now_millis(), &message, &produced, NULL) == KMX_OK &&
                        produced) {
                        if (client_queue(
                                item, KMX_MSG_FRAME,
                                message.data, message.size) != 0) {
                            kmx_buffer_free(&message);
                            client_release(item, count);
                            continue;
                        }
                    }
                    kmx_buffer_free(&message);
                }
            }
            if (closed) break;
        }

        for (which = 0; which < KMX_MAX_CLIENTS; which++) {
            client *item = &clients[which];
            if (item->fd < 0) continue;

            if (count && !kmx_layout_equal(&layout, &item->announced)) {
                kmx_buffer wire;
                kmx_buffer_init(&wire);
                if (kmx_layout_encode(&layout, &wire) == KMX_OK) {
                    if (client_queue(
                            item, KMX_MSG_LAYOUT, wire.data, wire.size) != 0) {
                        kmx_buffer_free(&wire);
                        client_release(item, count);
                        continue;
                    }
                    item->announced = layout;
                }
                kmx_buffer_free(&wire);
            }

            for (slot = 0; slot < count && item->fd >= 0; slot++) {
                kmx_buffer message;
                kmx_sync_info info;
                bool produced = false;
                /* Nothing new until the last one is away: a client that is
                 * behind wants the next diff, not a queue of stale ones. */
                if (!client_idle(item)) break;
                kmx_buffer_init(&message);
                if (kmx_buffer_append(
                        &message, &(unsigned char){(unsigned char)slot}, 1) == KMX_OK &&
                    kmx_sync_poll(
                        item->sync[slot], now_millis(),
                        &message, &produced, &info) == KMX_OK && produced) {
                    if (client_queue(
                            item, KMX_MSG_CELLS, message.data, message.size) != 0) {
                        kmx_buffer_free(&message);
                        client_release(item, count);
                        break;
                    }
                }
                kmx_buffer_free(&message);
            }

            for (slot = 0; slot < count && item->fd >= 0; slot++) {
                size_t events = kmx_term_graphics_count(panes[slot].term);
                size_t event;
                for (event = 0; event < events && item->fd >= 0; event++) {
                    const kmx_graphics_event *graphic =
                        kmx_term_graphics_at(panes[slot].term, event);
                    kmx_image_key key;
                    kmx_buffer wire;
                    bool known;
                    if (!graphic || !graphic->payload.size) continue;
                    key = kmx_image_key_of(
                        graphic->payload.data, graphic->payload.size);
                    known = kmx_image_cache_has(item->holds, &key);
                    kmx_buffer_init(&wire);
                    if (kmx_image_encode(
                            (uint32_t)slot, &key,
                            known ? NULL : (const void *)graphic->payload.data,
                            known ? 0 : graphic->payload.size, &wire) == KMX_OK) {
                        if (client_queue(
                                item, KMX_MSG_IMAGE, wire.data, wire.size) != 0) {
                            kmx_buffer_free(&wire);
                            client_release(item, count);
                            break;
                        }
                        if (!known) {
                            (void)kmx_image_cache_put(
                                item->holds, &key,
                                graphic->payload.data, graphic->payload.size);
                        }
                    }
                    kmx_buffer_free(&wire);
                }
            }
        }

        for (which = 0; which < KMX_MAX_CLIENTS; which++) {
            if (clients[which].fd < 0 || client_idle(&clients[which])) continue;
            if (client_flush(&clients[which]) != 0) client_release(&clients[which], count);
        }

        /* Cleared only once every client has had its look, since what one
         * client already holds says nothing about what another holds - and
         * only if there was a client at all.  With nobody attached the events
         * stay queued, so a client that arrives shortly after a pane drew
         * something still receives it.  The queue is bounded, so an unattached
         * pane drawing forever drops the oldest rather than growing. */
        {
            bool anyone_attached = false;
            for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                if (clients[which].fd >= 0) {
                    anyone_attached = true;
                    break;
                }
            }
            if (anyone_attached) {
                for (slot = 0; slot < count; slot++) {
                    if (kmx_term_graphics_count(panes[slot].term)) {
                        kmx_term_graphics_clear(panes[slot].term);
                    }
                }
            }
        }

        if (!any_alive && !pixel_running) {
            for (slot = 0; slot < count; slot++) {
                int status;
                if (panes[slot].child > 0) waitpid(panes[slot].child, &status, WNOHANG);
            }
            for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                if (clients[which].fd < 0) continue;
                (void)client_queue(&clients[which], KMX_MSG_EXIT, NULL, 0);
                (void)client_flush(&clients[which]);
            }
            break;
        }
    }

    for (which = 0; which < KMX_MAX_CLIENTS; which++) {
        if (clients[which].fd >= 0) client_release(&clients[which], count);
    }
    for (slot = 0; slot < count; slot++) {
        if (panes[slot].master >= 0) close(panes[slot].master);
        kmx_term_free(panes[slot].term);
    }
    if (pixel_running) kmx_pixel_stop(&pixel);
    if (audio_fd >= 0) close(audio_fd);
    if (audio_child > 0) {
        kill(audio_child, SIGTERM);
        waitpid(audio_child, NULL, 0);
    }
    free(audio_block);
    if (accepts_refused) {
        fprintf(stderr, "kmx-serve: refused %lu connection(s) to the rate limit\n",
                accepts_refused);
    }
    if (listener >= 0) close(listener);
    kmx_tls_server_free(tls);
    kmx_endpoint_cleanup(&endpoint);
    return 0;
}
