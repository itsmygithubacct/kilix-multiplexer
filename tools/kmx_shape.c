/* A network you would not wish on anyone, on demand.
 *
 * Sits between a client and a server and applies delay, jitter, loss and a
 * bandwidth ceiling to the bytes going past.  This is how the claims about
 * behaviour on a bad link get tested; without it they are assertions.
 *
 * Shaping in userspace rather than with tc/netem is deliberate.  It needs no
 * privileges, it is deterministic and seedable where netem is stochastic and
 * kernel-dependent, and it therefore runs the same on a laptop and in CI.  The
 * cost is that it shapes a TCP stream rather than packets, so it models a slow
 * or distant link honestly and models packet loss only as the retransmission
 * delay the kernel would have suffered - which is the right abstraction for
 * asking "is this usable", and the wrong one for asking "how does QUIC differ
 * from TCP".  For the second question, reach for netem.
 *
 *   kmx-shape --listen PORT --to HOST:PORT
 *             [--delay MS] [--jitter MS] [--loss PERCENT]
 *             [--rate BYTES_PER_SECOND] [--seed N]
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* The queue only has to hold what is in flight: delay x rate.  At half a
 * second and 32 kB/s that is 16 kB, so this is already generous.  It is also
 * why these live in static storage rather than on the stack - two of these
 * structs are megabytes, and the first version of this tool overflowed an
 * 8 MB stack before it printed a single line. */
#define SHAPE_QUEUE 512
#define SHAPE_CHUNK 4096

typedef struct {
    uint64_t due;
    size_t size;
    unsigned char data[SHAPE_CHUNK];
} parcel;

/* One direction of the relay: bytes read here, released there once their time
 * has come and the rate allowance permits. */
typedef struct {
    int from;
    int to;
    parcel queue[SHAPE_QUEUE];
    size_t head;
    size_t count;
    uint64_t allowance_start;
    size_t allowance_used;
    size_t dropped;
    size_t delivered;
} direction;

static uint64_t rng = 0x243f6a8885a308d3ull;

static uint32_t
next_random(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static uint64_t
now_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
connect_to(const char *host, int port) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *candidate;
    char service[16];
    int fd = -1;
    snprintf(service, sizeof service, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &results) != 0) return -1;
    for (candidate = results; candidate; candidate = candidate->ai_next) {
        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    return fd;
}

int
main(int argc, char **argv) {
    int listen_port = 0;
    char target_host[256] = "127.0.0.1";
    int target_port = 0;
    unsigned delay = 0;
    unsigned jitter = 0;
    unsigned loss = 0;
    size_t rate = 0;
    int index = 1;
    int listener;
    int client;
    int server;
    static direction forward;
    static direction backward;

    /* Answered before anything else is parsed, so it works regardless of
     * whether the rest of the command line is right. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("kmx-shape %s\n", KMX_VERSION);
        return 0;
    }

    while (index < argc) {
        if (!strcmp(argv[index], "--listen") && index + 1 < argc) {
            listen_port = atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--to") && index + 1 < argc) {
            char *colon = strrchr(argv[++index], ':');
            if (!colon) return 2;
            *colon = '\0';
            snprintf(target_host, sizeof target_host, "%s", argv[index]);
            target_port = atoi(colon + 1);
        } else if (!strcmp(argv[index], "--delay") && index + 1 < argc) {
            delay = (unsigned)atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--jitter") && index + 1 < argc) {
            jitter = (unsigned)atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--loss") && index + 1 < argc) {
            loss = (unsigned)atoi(argv[++index]);
        } else if (!strcmp(argv[index], "--rate") && index + 1 < argc) {
            rate = (size_t)strtoul(argv[++index], NULL, 10);
        } else if (!strcmp(argv[index], "--seed") && index + 1 < argc) {
            rng = strtoull(argv[++index], NULL, 10) | 1u;
        } else {
            fprintf(stderr,
                "usage: kmx-shape --listen PORT --to HOST:PORT [--delay MS]\n"
                "                 [--jitter MS] [--loss PERCENT] [--rate BPS]\n"
                "                 [--seed N]\n");
            return 2;
        }
        index++;
    }
    if (!listen_port || !target_port) return 2;

    signal(SIGPIPE, SIG_IGN);
    {
        struct sockaddr_in address;
        int reuse = 1;
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((unsigned short)listen_port);
        if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
            listen(listener, 1) != 0) {
            fprintf(stderr, "kmx-shape: listen: %s\n", strerror(errno));
            return 1;
        }
    }
    fprintf(stderr,
        "kmx-shape: :%d -> %s:%d  delay=%ums jitter=%ums loss=%u%% rate=%zu B/s\n",
        listen_port, target_host, target_port, delay, jitter, loss, rate);

    client = accept(listener, NULL, NULL);
    if (client < 0) return 1;
    close(listener);
    server = connect_to(target_host, target_port);
    if (server < 0) {
        fprintf(stderr, "kmx-shape: connect: %s\n", strerror(errno));
        return 1;
    }
    {
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
        fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
        fcntl(server, F_SETFL, fcntl(server, F_GETFL, 0) | O_NONBLOCK);
    }

    memset(&forward, 0, sizeof forward);
    memset(&backward, 0, sizeof backward);
    forward.from = client;
    forward.to = server;
    backward.from = server;
    backward.to = client;

    while (true) {
        struct pollfd descriptors[2];
        uint64_t now = now_millis();
        direction *sides[2] = {&forward, &backward};
        int side;
        bool finished = false;

        for (side = 0; side < 2; side++) {
            direction *way = sides[side];
            /* Read whatever has arrived, timestamp it for the future, and
             * decide now whether it will ever be delivered. */
            while (way->count < SHAPE_QUEUE) {
                parcel *slot = &way->queue[(way->head + way->count) % SHAPE_QUEUE];
                ssize_t count = recv(way->from, slot->data, SHAPE_CHUNK, MSG_DONTWAIT);
                if (count > 0) {
                    if (loss && (next_random() % 100u) < loss) {
                        /* Modelled as the delay a retransmission would have
                         * cost, not as a hole: this is a byte stream, and the
                         * kernel would not have handed the peer a gap. */
                        way->dropped++;
                        slot->due = now + delay * 2u + 50u;
                    } else {
                        uint64_t wobble = jitter ? (next_random() % (jitter * 2u + 1u)) : 0;
                        slot->due = now + delay + wobble;
                    }
                    slot->size = (size_t)count;
                    way->count++;
                    continue;
                }
                if (count == 0) finished = true;
                break;
            }

            /* Release what is due, within the rate allowance. */
            while (way->count) {
                parcel *slot = &way->queue[way->head];
                ssize_t written;
                if (slot->due > now) break;
                if (rate) {
                    if (now - way->allowance_start >= 1000) {
                        way->allowance_start = now;
                        way->allowance_used = 0;
                    }
                    if (way->allowance_used >= rate) break;
                }
                written = send(way->to, slot->data, slot->size,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
                if (written < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    finished = true;
                    break;
                }
                if ((size_t)written < slot->size) {
                    memmove(slot->data, slot->data + written, slot->size - (size_t)written);
                    slot->size -= (size_t)written;
                    way->allowance_used += (size_t)written;
                    break;
                }
                way->allowance_used += slot->size;
                way->delivered += slot->size;
                way->head = (way->head + 1) % SHAPE_QUEUE;
                way->count--;
            }
        }

        if (finished && !forward.count && !backward.count) break;

        descriptors[0].fd = client;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = server;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        /* A short tick, because delivery is time-driven as much as
         * event-driven: something may be due with nothing to read. */
        if (poll(descriptors, 2, 5) < 0 && errno != EINTR) break;
    }

    fprintf(stderr,
        "kmx-shape: delivered %zu/%zu bytes, delayed %zu parcels for loss\n",
        forward.delivered + backward.delivered,
        forward.delivered + backward.delivered,
        forward.dropped + backward.dropped);
    close(client);
    close(server);
    return 0;
}
