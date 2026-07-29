/* Where a session listens, and how a client reaches it.
 *
 * Two forms: a Unix socket for the local case, and TCP for the remote one.
 * TCP binds loopback unless told otherwise, because the intended way to reach
 * a session across a network is an SSH tunnel to a loopback port - the same
 * posture the rest of this stack already takes, and the reason a bare
 * `--listen` cannot accidentally expose a shell to a network.
 *
 * Exposing a non-loopback address is possible but has to be asked for, and
 * says so when it happens. */
#define _POSIX_C_SOURCE 200809L

#include "kilix_mux.h"
#include "endpoint.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

bool
kmx_endpoint_parse(const char *text, kmx_endpoint *endpoint) {
    const char *colon;
    if (!text || !endpoint) return false;
    memset(endpoint, 0, sizeof *endpoint);
    colon = strrchr(text, ':');
    /* No colon at all means a filesystem path; a path is also the reading for
     * anything that starts with a slash or a dot, so a directory named with a
     * colon is not mistaken for a host. */
    if (!colon || text[0] == '/' || text[0] == '.') {
        endpoint->kind = KMX_ENDPOINT_UNIX;
        if (strlen(text) >= sizeof endpoint->path) return false;
        memcpy(endpoint->path, text, strlen(text));
        return true;
    }
    endpoint->kind = KMX_ENDPOINT_TCP;
    if ((size_t)(colon - text) >= sizeof endpoint->host) return false;
    memcpy(endpoint->host, text, (size_t)(colon - text));
    if (!endpoint->host[0]) {
        memcpy(endpoint->host, "127.0.0.1", sizeof "127.0.0.1");
    }
    endpoint->port = (int)strtol(colon + 1, NULL, 10);
    return endpoint->port > 0 && endpoint->port <= 65535;
}

bool
kmx_endpoint_is_loopback(const kmx_endpoint *endpoint) {
    struct in_addr address;
    if (!endpoint || endpoint->kind != KMX_ENDPOINT_TCP) return true;
    if (strcmp(endpoint->host, "localhost") == 0) return true;
    if (inet_pton(AF_INET, endpoint->host, &address) == 1) {
        return (ntohl(address.s_addr) >> 24) == 127;
    }
    return false;
}

/* The listeners are non-blocking.
 *
 * accept(2) is explicit that a POLLIN on a listening socket does not guarantee
 * a connection is still there to take - an asynchronous error can remove it in
 * between - and that accept() on a blocking listener then waits for the next
 * one to arrive.  In a single-threaded server with no other timeout that is a
 * stall of everything: the settle sweep, the pane reads, every attached
 * client, until some unrelated peer happens to connect.  `--lan` is exactly
 * where asynchronous errors are ordinary.
 *
 * SOCK_CLOEXEC in accept4 applies to the accepted socket, not to this one, so
 * it does not cover this. */
int
kmx_endpoint_listen(const kmx_endpoint *endpoint, bool allow_public) {
    int fd;
    if (!endpoint) return -1;

    if (endpoint->kind == KMX_ENDPOINT_UNIX) {
        struct sockaddr_un address;
        if (strlen(endpoint->path) >= sizeof address.sun_path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, endpoint->path, strlen(endpoint->path));
        unlink(endpoint->path);
        if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
            chmod(endpoint->path, 0600) != 0 ||
            listen(fd, 8) != 0) {
            int saved = errno;
            close(fd);
            unlink(endpoint->path);
            errno = saved;
            return -1;
        }
        return fd;
    }

    if (!kmx_endpoint_is_loopback(endpoint) && !allow_public) {
        /* Refused rather than quietly narrowed: a caller that asked for a
         * public address and silently got loopback would think it was
         * reachable when it was not. */
        errno = EPERM;
        return -1;
    }
    {
        struct sockaddr_in address;
        int reuse = 1;
        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_port = htons((unsigned short)endpoint->port);
        if (inet_pton(AF_INET, endpoint->host, &address.sin_addr) != 1) {
            errno = EINVAL;
            return -1;
        }
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
            listen(fd, 8) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    return fd;
}

int
kmx_endpoint_connect(const kmx_endpoint *endpoint) {
    int fd;
    if (!endpoint) return -1;
    if (endpoint->kind == KMX_ENDPOINT_UNIX) {
        struct sockaddr_un address;
        if (strlen(endpoint->path) >= sizeof address.sun_path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, endpoint->path, strlen(endpoint->path));
        if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        return fd;
    }
    {
        struct addrinfo hints;
        struct addrinfo *results = NULL;
        struct addrinfo *candidate;
        char service[16];
        snprintf(service, sizeof service, "%d", endpoint->port);
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(endpoint->host, service, &hints, &results) != 0) {
            errno = EHOSTUNREACH;
            return -1;
        }
        for (candidate = results; candidate; candidate = candidate->ai_next) {
            fd = socket(
                candidate->ai_family,
                candidate->ai_socktype | SOCK_CLOEXEC,
                candidate->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
                freeaddrinfo(results);
                return fd;
            }
            close(fd);
        }
        freeaddrinfo(results);
        errno = ECONNREFUSED;
    }
    return -1;
}

void
kmx_endpoint_tune(int fd, const kmx_endpoint *endpoint) {
    int flag = 1;
    if (fd < 0 || !endpoint || endpoint->kind != KMX_ENDPOINT_TCP) return;
    /* Nagle batches small writes, which is exactly wrong here: a cell diff is
     * small and its whole value is arriving promptly. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
}

void
kmx_endpoint_cleanup(const kmx_endpoint *endpoint) {
    if (endpoint && endpoint->kind == KMX_ENDPOINT_UNIX && endpoint->path[0]) {
        unlink(endpoint->path);
    }
}
