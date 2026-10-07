#ifndef KMX_RANDOM_H
#define KMX_RANDOM_H

#include <errno.h>
#include <stddef.h>
#include <sys/random.h>

/* Identities must not fall back to timestamps or process IDs. */
static inline int
kmx_random_bytes(unsigned char *bytes, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = getrandom(bytes + done, size - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

#endif
