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
#include "kmx_tap.h"
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

/* How long a new connection has to complete its TLS handshake and send HELLO.
 * Generous for any real client on any real link, and finite, which is the
 * point. */
#define KMX_SETTLE_MS 10000u

typedef struct {
    kmx_term *term;
    int master;
    int input_fd;
    int observer_hold;
    pid_t child;
    pid_t input_child;
    bool alive;
    bool fixed_size;
    const char *command;
    /* Typed bytes the pty has not accepted yet.  A pty master holds only a few
     * kilobytes, so a program that stops reading its input fills it quickly -
     * and a blocking write there would stall this loop, freezing every pane and
     * every client on behalf of one wedged program.  So input is queued and
     * drained on POLLOUT, exactly like a client's outbound queue. */
    kmx_buffer input;
    size_t input_offset;
} pane;

/* Typing ahead of a program that is not reading is worth a little tolerance and
 * no more.  Past this the oldest keystrokes are already meaningless, so the new
 * ones are dropped rather than the buffer grown. */
#define KMX_PANE_INPUT_LIMIT (256u * 1024u)

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
    bool handshaking;
    bool wants_write;   /* the handshake is waiting for the socket, not the peer */
    /* When this connection must have finished introducing itself.  A peer that
     * connects and then goes quiet - mid-handshake or before HELLO - holds one
     * of a small number of slots, so silence is given a limit rather than
     * treated as patience.  Cleared once the peer has greeted. */
    uint64_t settle_by;
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

/* Drop what has already gone, so that a queue's limit bounds the buffer and not
 * merely its unsent tail.
 *
 * Without this, a peer that drains steadily but never quite empties the queue
 * keeps `size` growing for the life of the connection while the remainder -
 * the only thing the limit looks at - stays comfortably small.  The memory is
 * reclaimed at the halfway mark rather than on every write, which keeps the
 * copying amortised while still bounding the buffer at a small multiple of the
 * limit. */
static void
compact(kmx_buffer *buffer, size_t *offset) {
    if (!*offset) return;
    if (*offset < buffer->size / 2 && *offset < 64u * 1024u) return;
    memmove(buffer->data, buffer->data + *offset, buffer->size - *offset);
    buffer->size -= *offset;
    *offset = 0;
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
    compact(&item->out, &item->out_offset);
    if (item->out.size - item->out_offset + framed.size > KMX_CLIENT_QUEUE_LIMIT) {
        result = -1;
    } else if (kmx_buffer_append(&item->out, framed.data, framed.size) != KMX_OK) {
        result = -1;
    }
    kmx_buffer_free(&framed);
    return result;
}

/* Queue a message for a plane that is allowed to lose one.
 *
 * The motion and audio planes both drop rather than buffer when their rate
 * allowance is spent, and the socket has to agree with that: a frame or a block
 * that does not fit the backlog is discarded, not grounds for disconnecting.
 *
 * It was grounds for disconnecting, which is worse than it sounds.  A 1080p
 * frame of incompressible pixels is about 6 MB on the wire, over the 4 MiB
 * backlog, so the client was dropped mid-stream - and reconnected, and was
 * dropped again on the next such frame.  The one plane whose documented
 * contract is that it may drop was the one that could kill the connection. */
static void
client_queue_droppable(
    client *item,
    kmx_message_type type,
    const void *payload,
    size_t size,
    size_t *dropped
) {
    if (client_queue(item, type, payload, size) != 0 && dropped) (*dropped)++;
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

/* Push queued keystrokes at the pty, without blocking on it. */
static int
pane_input_flush(pane *item) {
    if (item->input_fd < 0) return -1;
    while (item->input_offset < item->input.size) {
        ssize_t count = write(
            item->input_fd,
            item->input.data + item->input_offset,
            item->input.size - item->input_offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (count == 0) return 0;
        item->input_offset += (size_t)count;
    }
    item->input_offset = 0;
    item->input.size = 0;
    return 0;
}

static void
pane_input_queue(pane *item, const void *data, size_t size) {
    if (item->input_fd < 0) return;
    compact(&item->input, &item->input_offset);
    /* The limit bounds what is ALREADY waiting, not what is arriving.
     *
     * Testing the sum instead meant a single message at or above the limit
     * failed unconditionally - even against an empty queue and an idle pane,
     * where nothing was under pressure at all.  Measured: one 300 KiB message
     * delivered zero bytes to the pane, while the same 300 KiB split into 4 KiB
     * frames delivered all of it.  A large paste is an ordinary thing to do and
     * the framer already bounds one message at KMX_MESSAGE_MAX, so a message
     * that arrives when there is room is taken whole however big it is; what
     * the limit refuses is piling more on top of a backlog that is already too
     * deep.  The buffer is therefore bounded by the limit plus one message. */
    if (item->input.size - item->input_offset > KMX_PANE_INPUT_LIMIT) return;
    if (kmx_buffer_append(&item->input, data, size) != KMX_OK) return;
    (void)pane_input_flush(item);
}

static bool
pane_input_pending(const pane *item) {
    return item->input_fd >= 0 && item->input_offset < item->input.size;
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

/*
 * Shared-memory and file graphics name resources on the host.  Replaying that
 * reference on another machine cannot work (and the local frontend may already
 * have unlinked it).  A presenter tap supplies the pixels for those panes, so
 * retain inline graphics while suppressing only host-local transfers.
 */
static bool
graphics_is_host_local(const unsigned char *data, size_t size) {
    const unsigned char *end;
    size_t header_size;
    if (!data || !size) return false;
    end = memchr(data, ';', size);
    header_size = end ? (size_t)(end - data) : size;
    return memmem(data, header_size, "t=s", 3) != NULL ||
           memmem(data, header_size, "t=f", 3) != NULL ||
           memmem(data, header_size, "t=t", 3) != NULL;
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
make_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Keep the broker library's blocking frame reader outside the scheduler.  Its
 * public `observe` CLI is a protocol-v2 observer: stdout is only pane output,
 * stdin is never forwarded, and the observer never claims the control slot.
 * The scheduler reads the pipe non-blocking just as it reads a pty.
 */
static pid_t
start_broker_observer(
    const char *executable,
    const char *runtime_dir,
    const char *session_id,
    int *output_fd,
    int *hold_fd
) {
    int pipe_fds[2];
    int idle_fds[2];
    pid_t child;
    if (!executable || !runtime_dir || !session_id || !output_fd || !hold_fd ||
        pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return -1;
    }
    if (pipe2(idle_fds, O_CLOEXEC) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(idle_fds[0]);
        close(idle_fds[1]);
        return -1;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        close(idle_fds[1]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(idle_fds[0], STDIN_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        close(idle_fds[0]);
        if (strchr(executable, '/')) {
            execl(
                executable, executable,
                "--runtime-dir", runtime_dir,
                "observe", session_id, (char *)NULL);
        } else {
            execlp(
                executable, executable,
                "--runtime-dir", runtime_dir,
                "observe", session_id, (char *)NULL);
        }
        _exit(127);
    }
    close(pipe_fds[1]);
    close(idle_fds[0]);
    *output_fd = pipe_fds[0];
    *hold_fd = idle_fds[1];
    make_non_blocking(*output_fd);
    return child;
}

/*
 * Live input is deliberately a different descriptor from broker observation.
 * Kilix supplies a pane-scoped helper here; without one a live pane is
 * view-only.  This preserves the broker's assertion that an observer can
 * never inject bytes.
 */
static pid_t
start_input_helper(const char *command, int *input_fd) {
    int pipe_fds[2];
    pid_t child;
    if (!command || !input_fd || pipe2(pipe_fds, O_CLOEXEC) != 0) return -1;
    child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        close(pipe_fds[1]);
        if (dup2(pipe_fds[0], STDIN_FILENO) < 0) _exit(127);
        close(pipe_fds[0]);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[0]);
    *input_fd = pipe_fds[1];
    make_non_blocking(*input_fd);
    return child;
}

static void
stop_helper(pid_t *child) {
    if (!child || *child <= 0) return;
    if (waitpid(*child, NULL, WNOHANG) == 0) {
        kill(*child, SIGTERM);
        (void)waitpid(*child, NULL, 0);
    }
    *child = -1;
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
          "       kmx-serve --socket PATH --broker-session ID\n"
          "                 --broker-runtime DIR [--broker-executable PATH]\n"
          "                 [--input-command COMMAND] [--title TITLE]\n"
          "                 [--tap-socket PATH [--tap-session ID]]\n"
          "\n"
          "  --socket accepts a path or HOST:PORT.  TCP binds loopback unless\n"
          "  --lan is given; reach a session across a network over SSH.\n"
          "  --pixel-pane runs an X client on a private display and streams its\n"
          "  frames; it never touches the display the operator is using.\n"
          "  --broker-session observes a live protocol-v2 broker session.  The\n"
          "  observer stays read-only; input uses the separate helper command.\n"
          "  --tap-socket receives presenter RGB frames for the named session.\n"
          "  --audio-source runs a command that writes raw 16-bit PCM to stdout.\n"
          "  --lan requires a token; one is generated and printed if not given.\n"
          "  A reachable bind encrypts by default and prints a fingerprint the\n"
        "  client must be given.  That identity persists under XDG_STATE_HOME so\n"
        "  the fingerprint stays the same across restarts; --tls-ephemeral mints\n"
        "  a fresh one per start and never writes it to disk.\n"
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
    bool tls_ephemeral = false;
    kmx_tls_server *tls = NULL;
    char fingerprint[KMX_TLS_FINGERPRINT_HEX + 1];
    const char *commands[KMX_MAX_PANES];
    const char *broker_session = NULL;
    const char *broker_runtime = NULL;
    const char *broker_executable = NULL;
    const char *input_command = NULL;
    const char *live_title = NULL;
    const char *tap_path = NULL;
    const char *tap_session = NULL;
    bool tap_session_given = false;
    kmx_tap tap;
    bool tap_running = false;
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
    /* Counted and reported: a session displacing peers constantly is being
     * leaned on, and that should be visible rather than silent. */
    unsigned long displaced_pending = 0;
    /* Reported at shutdown rather than silently swallowed: a plane that drops
     * is behaving as designed, but a plane that drops everything is a
     * misconfiguration, and the difference should be visible. */
    size_t frames_dropped = 0;
    size_t blocks_dropped = 0;
    size_t count;
    size_t slot;
    size_t which;

    memset(panes, 0, sizeof panes);
    memset(clients, 0, sizeof clients);
    memset(&tap, 0, sizeof tap);
    tap.listener = -1;
    tap.source = -1;
    for (which = 0; which < KMX_MAX_PANES; which++) {
        panes[which].master = -1;
        panes[which].input_fd = -1;
        panes[which].observer_hold = -1;
        panes[which].child = -1;
        panes[which].input_child = -1;
    }
    for (which = 0; which < KMX_MAX_CLIENTS; which++) clients[which].fd = -1;

    /* Answered before anything else is parsed, so it works regardless of
     * whether the rest of the command line is right. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("kmx-serve %s\n", KMX_VERSION);
        return 0;
    }

    while (index < argc && strcmp(argv[index], "--") != 0) {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--rows") == 0 && index + 1 < argc) {
            rows = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--cols") == 0 && index + 1 < argc) {
            cols = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--listen") == 0 && index + 1 < argc) {
            socket_path = argv[++index];
        } else if (strcmp(argv[index], "--broker-session") == 0 && index + 1 < argc) {
            broker_session = argv[++index];
        } else if (strcmp(argv[index], "--broker-runtime") == 0 && index + 1 < argc) {
            broker_runtime = argv[++index];
        } else if (strcmp(argv[index], "--broker-executable") == 0 &&
                   index + 1 < argc) {
            broker_executable = argv[++index];
        } else if (strcmp(argv[index], "--input-command") == 0 &&
                   index + 1 < argc) {
            input_command = argv[++index];
        } else if (strcmp(argv[index], "--title") == 0 && index + 1 < argc) {
            live_title = argv[++index];
        } else if (strcmp(argv[index], "--tap-socket") == 0 && index + 1 < argc) {
            tap_path = argv[++index];
        } else if (strcmp(argv[index], "--tap-session") == 0 && index + 1 < argc) {
            tap_session = argv[++index];
            tap_session_given = true;
        } else if (strcmp(argv[index], "--pixel-pane") == 0 && index + 1 < argc) {
            pixel_command = argv[++index];
        } else if (strcmp(argv[index], "--pixel-size") == 0 && index + 1 < argc) {
            if (sscanf(argv[++index], "%dx%d", &pixel_width, &pixel_height) != 2 ||
                pixel_width < 1 || pixel_height < 1 ||
                pixel_width > 8192 || pixel_height > 8192 ||
                (long)pixel_width * pixel_height * 3L > 64L * 1024L * 1024L) {
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
        } else if (strcmp(argv[index], "--tls-ephemeral") == 0) {
            tls_ephemeral = true;
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
    if (!broker_runtime) broker_runtime = getenv("KITTY_PTY_BROKER_RUNTIME");
    if (!broker_executable) {
        broker_executable = getenv("KITTY_PTY_BROKER_EXECUTABLE");
        if (!broker_executable || !*broker_executable) {
            broker_executable = "kitty-pty-broker";
        }
    }
    if (!tap_session) tap_session = broker_session;
    if (!socket_path || rows <= 0 || cols <= 0 ||
        (command_count == 0 && !single && !pixel_command && !broker_session) ||
        (broker_session && (!broker_runtime || !*broker_runtime)) ||
        (broker_session &&
         (command_count != 0 || single || pixel_command)) ||
        (input_command && !broker_session) ||
        (tap_path && (!tap_session || !*tap_session)) ||
        (tap_session_given && !tap_path)) {
        usage();
        return 2;
    }
    /* A pixel pane is the whole session for now: mixing pixel and text panes
     * in one layout is a later refinement, not part of proving the plane. */
    count = pixel_command ? 0 : (broker_session ? 1 : (single ? 1 : command_count));

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
    /* Every TCP bind needs a token, loopback included.
     *
     * An earlier version tied this to reachability, on the reasoning that
     * loopback reaches nobody new.  That was wrong: SO_PEERCRED exists only on
     * a Unix socket, so a loopback TCP port has no peer check at all and any
     * local user could attach as a control client.  Reachability decides
     * whether the traffic needs encrypting; it does not decide whether the
     * peer needs identifying. */
    if (endpoint.kind == KMX_ENDPOINT_TCP && !require_token) {
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
        tls = kmx_tls_server_create(fingerprint, sizeof fingerprint, tls_ephemeral);
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
        /* Only when it really is in the clear.  This warning used to print on
         * every reachable bind, including the ones that had just printed a
         * certificate fingerprint two lines above - so the common case told the
         * operator their encrypted session was readable by anyone on the
         * segment.  A warning that is wrong where it is loudest teaches people
         * to ignore the ones that are right. */
        if (endpoint.kind == KMX_ENDPOINT_TCP &&
            !kmx_endpoint_is_loopback(&endpoint) && !use_tls) {
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
        item->command = broker_session
            ? (live_title ? live_title : "live pane")
            : (single ? single[0] : commands[slot]);
        snprintf(layout.panes[slot].title, KMX_TITLE_MAX, "%zu: %s",
                 slot + 1, item->command);
        if (kmx_term_create(&item->term, info->rows, info->cols) != KMX_OK) {
            fprintf(stderr, "kmx-serve: out of memory\n");
            return 1;
        }
        if (broker_session) {
            item->child = start_broker_observer(
                broker_executable, broker_runtime, broker_session,
                &item->master, &item->observer_hold);
            if (item->child < 0) {
                fprintf(stderr, "kmx-serve: could not start broker observer: %s\n",
                        strerror(errno));
                return 1;
            }
            item->fixed_size = true;
            item->alive = true;
            if (input_command) {
                item->input_child = start_input_helper(input_command, &item->input_fd);
                if (item->input_child < 0) {
                    fprintf(stderr, "kmx-serve: could not start input helper: %s\n",
                            strerror(errno));
                    return 1;
                }
            } else {
                fprintf(stderr,
                    "kmx-serve: live pane is view-only (no input helper)\n");
            }
            fprintf(stderr, "kmx-serve: observing broker session %s\n",
                    broker_session);
            continue;
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
        /* Non-blocking from here: every write to this descriptor goes through
         * pane_input_queue, which never waits on it. */
        make_non_blocking(item->master);
        item->input_fd = item->master;
        item->alive = true;
    }

    if (tap_path) {
        if (!kmx_tap_start(&tap, tap_path, tap_session)) {
            fprintf(stderr, "kmx-serve: could not start frame tap: %s\n",
                    strerror(errno));
            return 1;
        }
        tap_running = true;
        fprintf(stderr, "kmx-serve: frame tap %s\n", tap_path);
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
        /* +4 for audio, the private-display frame pipe, and both ends of the
         * presenter tap. */
        struct pollfd descriptors[1 + KMX_MAX_CLIENTS + KMX_MAX_PANES + 4];
        nfds_t descriptor_count = 0;
        size_t client_at[KMX_MAX_CLIENTS];
        size_t pane_at[KMX_MAX_PANES];
        nfds_t client_first;
        nfds_t pane_first;
        nfds_t pane_descriptors;
        nfds_t tap_listener_index = (nfds_t)-1;
        nfds_t tap_source_index = (nfds_t)-1;
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
                (short)(POLLIN |
                        ((clients[which].handshaking
                              ? clients[which].wants_write
                              : !client_idle(&clients[which]))
                             ? POLLOUT : 0));
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        pane_first = descriptor_count;
        for (slot = 0; slot < count; slot++) {
            if (!panes[slot].alive) continue;
            any_alive = true;
            pane_at[descriptor_count - pane_first] = slot;
            descriptors[descriptor_count].fd = panes[slot].master;
            descriptors[descriptor_count].events =
                (short)(POLLIN |
                        (pane_input_pending(&panes[slot]) &&
                         panes[slot].input_fd == panes[slot].master
                             ? POLLOUT : 0));
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

        if (tap_running) {
            tap_listener_index = descriptor_count;
            descriptors[descriptor_count].fd = kmx_tap_listener_fd(&tap);
            descriptors[descriptor_count].events = POLLIN;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
            if (kmx_tap_source_fd(&tap) >= 0) {
                tap_source_index = descriptor_count;
                descriptors[descriptor_count].fd = kmx_tap_source_fd(&tap);
                descriptors[descriptor_count].events = POLLIN;
                descriptors[descriptor_count].revents = 0;
                descriptor_count++;
            }
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
                /* No free slot: take the oldest one still not authenticated.
                 *
                 * Without this, eight peers that connect and say nothing hold
                 * every slot for KMX_SETTLE_MS and can simply reconnect, which
                 * kept a session unattachable indefinitely at no cost to
                 * whoever could reach the port.  The settle sweep bounded how
                 * long each squatter lasted; it did nothing about the next one.
                 *
                 * A peer that has greeted is never displaced - that is a real
                 * client and losing it would be the denial of service moved
                 * rather than removed.  Among the un-greeted, the oldest goes:
                 * a legitimate client greets in milliseconds, so the connection
                 * being evicted is the one that has already had its whole
                 * chance and not taken it. */
                if (free_slot == KMX_MAX_CLIENTS) {
                    uint64_t oldest = 0;
                    size_t victim = KMX_MAX_CLIENTS;
                    for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                        if (clients[which].fd < 0) continue;
                        if (clients[which].greeted) continue;
                        /* settle_by is armed at accept and cleared on HELLO, so
                         * the largest one is the connection that arrived first. */
                        if (victim == KMX_MAX_CLIENTS ||
                            clients[which].settle_by < oldest) {
                            oldest = clients[which].settle_by;
                            victim = which;
                        }
                    }
                    if (victim != KMX_MAX_CLIENTS) {
                        client_release(&clients[victim], count);
                        displaced_pending++;
                        free_slot = victim;
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
                        item->tls = kmx_tls_server_begin(tls, accepted);
                        if (!item->tls) {
                            close(accepted);
                            item->fd = -1;
                            continue;
                        }
                        item->handshaking = true;
                    }
                    item->fd = accepted;
                    item->control = true;
                    item->settle_by = now_millis() + KMX_SETTLE_MS;
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
                    if (ok && (pixel_running || tap_running)) {
                        /* Per client, like the cell baseline: what one client
                         * has been shown says nothing about another. */
                        ok = kmx_motion_create(&item->motion, pixel_budget) == KMX_OK;
                    }
                    if (!ok) client_release(item, count);
                }
            }
        }

        /* Swept over every slot rather than only the readable ones: a peer that
         * has gone quiet produces no events, which is precisely the case this
         * is here to catch. */
        {
            uint64_t moment = now_millis();
            for (slot = 0; slot < KMX_MAX_CLIENTS; slot++) {
                if (clients[slot].fd < 0 || !clients[slot].settle_by) continue;
                if (moment > clients[slot].settle_by) client_release(&clients[slot], count);
            }
        }

        for (which = 0; which + client_first < pane_first; which++) {
            size_t id = client_at[which];
            client *item = &clients[id];
            short revents = descriptors[client_first + which].revents;
            if (item->fd < 0 || descriptors[client_first + which].fd != item->fd) continue;
            if (item->handshaking) {
                /* One step per readiness, never a loop: this is a stranger's
                 * message being parsed, and it gets a slice of the loop rather
                 * than the loop. */
                if (!(revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) continue;
                switch (kmx_tls_server_step(item->tls)) {
                case KMX_TLS_DONE:
                    item->handshaking = false;
                    item->wants_write = false;
                    break;
                case KMX_TLS_WANT_READ:
                    item->wants_write = false;
                    continue;
                case KMX_TLS_WANT_WRITE:
                    item->wants_write = true;
                    continue;
                default:
                    client_release(item, count);
                    continue;
                }
            }
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
                    item->settle_by = 0;
                    /* A peer's dimensions drive layout arithmetic and pane
                     * sizing, so they are bounded here rather than trusted to
                     * be sane. */
                    item->rows = (payload[0] << 8) | payload[1];
                    item->cols = (payload[2] << 8) | payload[3];
                    if (item->rows < 1 || item->rows > KMX_MAX_DIMENSION ||
                        item->cols < 1 || item->cols > KMX_MAX_DIMENSION) {
                        client_release(item, count);
                        break;
                    }
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
                    if (item->control && focused < count && panes[focused].alive &&
                        panes[focused].input_fd >= 0) {
                        pane_input_queue(&panes[focused], payload, size);
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
                    int wanted_r = (payload[0] << 8) | payload[1];
                    int wanted_c = (payload[2] << 8) | payload[3];
                    if (wanted_r < 1 || wanted_r > KMX_MAX_DIMENSION ||
                        wanted_c < 1 || wanted_c > KMX_MAX_DIMENSION) {
                        client_release(item, count);
                        break;
                    }
                    item->rows = wanted_r;
                    item->cols = wanted_c;
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
            if (!broker_session && wanted_rows > 0 && wanted_cols > 0 &&
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
            short revents = descriptors[pane_first + which].revents;
            if (revents & POLLOUT) (void)pane_input_flush(&panes[id]);
            if (!(revents & (POLLIN | POLLHUP | POLLERR))) continue;
            {
                ssize_t received = read(panes[id].master, buffer, sizeof buffer);
                if (received > 0) {
                    /* Fed once, however many clients are watching. */
                    kmx_term_feed(panes[id].term, buffer, (size_t)received);
                } else if (received == 0 || (errno != EINTR && errno != EAGAIN)) {
                    panes[id].alive = false;
                    /* Reaped here rather than only once every pane has gone.
                     * A session whose first pane exits early otherwise carries
                     * that pane's zombie for the rest of its life, which in a
                     * long-lived multi-pane session is exactly the shape that
                     * accumulates. */
                    if (panes[id].child > 0) {
                        int status;
                        if (waitpid(panes[id].child, &status, WNOHANG) ==
                            panes[id].child) {
                            panes[id].child = -1;
                        }
                    }
                }
            }
        }
        for (slot = 0; slot < count; slot++) {
            if (!pane_input_pending(&panes[slot]) ||
                panes[slot].input_fd == panes[slot].master) {
                continue;
            }
            if (pane_input_flush(&panes[slot]) != 0) {
                close(panes[slot].input_fd);
                panes[slot].input_fd = -1;
                stop_helper(&panes[slot].input_child);
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
                    if (item->handshaking) continue;
                    if (require_token && !item->greeted) continue;
                    kmx_buffer_init(&message);
                    if (kmx_audio_offer(
                            item->audio, audio_block, audio_block_bytes,
                            audio_clock, &message, &produced, NULL) == KMX_OK &&
                        produced) {
                        client_queue_droppable(
                            item, KMX_MSG_AUDIO,
                            message.data, message.size, &blocks_dropped);
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
                    if (item->handshaking) continue;
                    if (require_token && !item->greeted) continue;
                    kmx_buffer_init(&message);
                    if (kmx_buffer_append(
                            &message, &(unsigned char){0}, 1) == KMX_OK &&
                        kmx_motion_offer(
                            item->motion, pixel.frame, pixel.width, pixel.height,
                            now_millis(), &message, &produced, NULL) == KMX_OK &&
                        produced) {
                        client_queue_droppable(
                            item, KMX_MSG_FRAME,
                            message.data, message.size, &frames_dropped);
                    }
                    kmx_buffer_free(&message);
                }
            }
            if (closed) break;
        }

        if (tap_running) {
            const unsigned char *frame = NULL;
            int frame_width = 0;
            int frame_height = 0;
            uint64_t offered_micros = 0;
            short listener_events = tap_listener_index == (nfds_t)-1
                ? 0 : descriptors[tap_listener_index].revents;
            short source_events = tap_source_index == (nfds_t)-1
                ? 0 : descriptors[tap_source_index].revents;
            kmx_tap_poll(&tap, listener_events, source_events);
            if (kmx_tap_take(
                    &tap, &frame, &frame_width, &frame_height,
                    NULL, NULL, NULL, NULL, &offered_micros)) {
                uint64_t frame_clock =
                    offered_micros ? offered_micros / 1000u : now_millis();
                if (getenv("KMX_DEBUG_FRAMES")) {
                    fprintf(stderr, "kmx-serve: tapped frame\n");
                    fflush(stderr);
                }
                for (which = 0; which < KMX_MAX_CLIENTS; which++) {
                    client *item = &clients[which];
                    kmx_buffer message;
                    bool produced = false;
                    if (item->fd < 0 || !item->motion) continue;
                    if (item->handshaking) continue;
                    if (require_token && !item->greeted) continue;
                    kmx_buffer_init(&message);
                    if (kmx_buffer_append(
                            &message, &(unsigned char){0}, 1) == KMX_OK &&
                        kmx_motion_offer(
                            item->motion, frame, frame_width, frame_height,
                            frame_clock, &message, &produced, NULL) == KMX_OK &&
                        produced) {
                        client_queue_droppable(
                            item, KMX_MSG_FRAME,
                            message.data, message.size, &frames_dropped);
                    }
                    kmx_buffer_free(&message);
                }
            }
        }

        for (which = 0; which < KMX_MAX_CLIENTS; which++) {
            client *item = &clients[which];
            if (item->fd < 0) continue;
            /* Nothing goes out before a valid greeting.
             *
             * The inbound side already refused everything from an un-greeted
             * peer, and that was mistaken for the whole check: an independent
             * review pointed out the server was still SENDING - layout, cell
             * updates, images - to a peer that had presented no token at all.
             * Refusing what a peer may say is not the same as refusing what it
             * may hear, and for a terminal the second is the one that
             * matters. */
            if (item->handshaking) continue;
            if (require_token && !item->greeted) continue;

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
                    if (tap_running &&
                        graphics_is_host_local(
                            graphic->payload.data, graphic->payload.size)) {
                        continue;
                    }
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
        if (panes[slot].input_fd >= 0 &&
            panes[slot].input_fd != panes[slot].master) {
            close(panes[slot].input_fd);
        }
        stop_helper(&panes[slot].input_child);
        if (panes[slot].observer_hold >= 0) close(panes[slot].observer_hold);
        if (panes[slot].master >= 0) close(panes[slot].master);
        if (broker_session) stop_helper(&panes[slot].child);
        kmx_buffer_free(&panes[slot].input);
        kmx_term_free(panes[slot].term);
    }
    if (pixel_running) kmx_pixel_stop(&pixel);
    if (tap_running) kmx_tap_stop(&tap);
    if (audio_fd >= 0) close(audio_fd);
    if (audio_child > 0) {
        kill(audio_child, SIGTERM);
        waitpid(audio_child, NULL, 0);
    }
    free(audio_block);
    if (frames_dropped || blocks_dropped) {
        fprintf(stderr,
            "kmx-serve: dropped %zu video frame(s) and %zu audio block(s) that "
            "did not fit a client's backlog\n", frames_dropped, blocks_dropped);
    }
    if (displaced_pending) {
        fprintf(stderr,
            "kmx-serve: displaced %lu connection(s) that never authenticated\n",
            displaced_pending);
    }
    if (accepts_refused) {
        fprintf(stderr, "kmx-serve: refused %lu connection(s) to the rate limit\n",
                accepts_refused);
    }
    if (listener >= 0) close(listener);
    kmx_tls_server_free(tls);
    kmx_endpoint_cleanup(&endpoint);
    return 0;
}
