/* Push keystrokes at a session faster than the pane will take them.
 *
 * Exists because the interesting case is hard to reach by hand: a pane in
 * canonical mode echoes its input straight back, and the server drains that
 * echo, so the pty never actually fills.  Put the pane in raw mode with echo
 * off and nothing drains it - the tty input queue fills after about a hundred
 * kilobytes, and a server that writes to the master with a blocking write stops
 * there, serving nobody, until the program that is not reading decides to read.
 *
 *   flood-input SOCKET MEGABYTES
 *   flood-input SOCKET probe
 *
 * The flood exits 0 having sent what it could; the point of the test is what
 * the server does next, not what this manages to send.  `probe` is the question
 * that follows: attach, greet, and exit 0 only if the server answers - which is
 * how a stuck loop is told from a busy one.
 */
#define _GNU_SOURCE

#include "kilix_mux.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int
connect_to(const char *path) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Blocking, but under a send timeout: once the server's own input queue is full
 * it stops reading, and this is expected to stall rather than to fail. */
static int
send_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count = send(fd, cursor + done, size - done, MSG_NOSIGNAL);
        if (count <= 0) {
            if (count < 0 && errno == EINTR) continue;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

int
main(int argc, char **argv) {
    struct timeval limit = {.tv_sec = 5, .tv_usec = 0};
    unsigned char payload[5];
    unsigned char typing[16384];
    kmx_buffer hello;
    kmx_buffer block;
    long megabytes;
    long sent = 0;
    bool probe;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "usage: flood-input SOCKET MEGABYTES|probe\n");
        return 2;
    }
    probe = strcmp(argv[2], "probe") == 0;
    megabytes = probe ? 0 : atol(argv[2]);
    fd = connect_to(argv[1]);
    if (fd < 0) {
        fprintf(stderr, "flood-input: cannot connect to %s\n", argv[1]);
        return 1;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);

    payload[0] = 0; payload[1] = 24;      /* rows */
    payload[2] = 0; payload[3] = 80;      /* cols */
    payload[4] = 0;                       /* control role */
    kmx_buffer_init(&hello);
    if (kmx_frame_encode(KMX_MSG_HELLO, payload, sizeof payload, &hello) != KMX_OK ||
        send_all(fd, hello.data, hello.size) != 0) {
        kmx_buffer_free(&hello);
        close(fd);
        return 1;
    }
    kmx_buffer_free(&hello);

    if (probe) {
        unsigned char reply[4096];
        ssize_t got;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        got = recv(fd, reply, sizeof reply, 0);
        close(fd);
        if (got <= 0) {
            fprintf(stderr, "flood-input: no answer from the server\n");
            return 1;
        }
        printf("flood-input: server answered with %ld bytes\n", (long)got);
        return 0;
    }

    memset(typing, 'x', sizeof typing);
    kmx_buffer_init(&block);
    if (kmx_frame_encode(KMX_MSG_INPUT, typing, sizeof typing, &block) != KMX_OK) {
        close(fd);
        return 1;
    }
    while (sent < megabytes * 1024L * 1024L) {
        if (send_all(fd, block.data, block.size) != 0) break;
        sent += (long)sizeof typing;
    }
    kmx_buffer_free(&block);
    close(fd);
    printf("flood-input: sent %ld bytes\n", sent);
    return 0;
}
