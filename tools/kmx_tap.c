#define _GNU_SOURCE

#include "kmx_tap.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define KMX_TAP_VERSION 1u
#define KMX_TAP_HEADER_SIZE 112u
#define KMX_TAP_FRAME_MAX (64u * 1024u * 1024u)

static uint32_t
read_u32(const unsigned char *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static uint64_t
read_u64(const unsigned char *data) {
    uint64_t value = 0;
    size_t index;
    for (index = 0; index < 8; index++) value = (value << 8) | data[index];
    return value;
}

static void
reset_message(kmx_tap *tap) {
    tap->header_used = 0;
    tap->frame_used = 0;
    tap->frame_size = 0;
    tap->width = 0;
    tap->height = 0;
    tap->columns = 0;
    tap->rows = 0;
    tap->scroll_x = 0;
    tap->scroll_y = 0;
    tap->offered_micros = 0;
}

static void
drop_source(kmx_tap *tap) {
    if (tap->source >= 0) close(tap->source);
    tap->source = -1;
    tap->have_frame = false;
    reset_message(tap);
}

static bool
peer_is_owner(int fd) {
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t size = sizeof credentials;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
        return false;
    }
    return credentials.uid == geteuid();
#else
    (void)fd;
    return true;
#endif
}

static bool
valid_header(kmx_tap *tap) {
    const unsigned char *header = tap->header;
    unsigned char expected_session[KMX_TAP_SESSION_MAX] = {0};
    uint32_t width;
    uint32_t height;
    uint32_t columns;
    uint32_t rows;
    uint64_t bytes;

    if (memcmp(header, "KFT1", 4) != 0 ||
        header[4] != 0 || header[5] != KMX_TAP_VERSION ||
        header[6] != 0 || header[7] != 0) {
        return false;
    }
    memcpy(expected_session, tap->session, strlen(tap->session));
    if (memcmp(header + 8, expected_session, sizeof expected_session) != 0) {
        return false;
    }

    width = read_u32(header + 72);
    height = read_u32(header + 76);
    columns = read_u32(header + 80);
    rows = read_u32(header + 84);
    bytes = read_u64(header + 104);
    if (!width || !height || width > 8192u || height > 8192u ||
        !columns || !rows || columns > 10000u || rows > 10000u ||
        (uint64_t)width * height * 3u != bytes ||
        bytes > KMX_TAP_FRAME_MAX || bytes > SIZE_MAX) {
        return false;
    }

    tap->width = (int)width;
    tap->height = (int)height;
    tap->columns = (int)columns;
    tap->rows = (int)rows;
    tap->scroll_x = (int32_t)read_u32(header + 88);
    tap->scroll_y = (int32_t)read_u32(header + 92);
    tap->offered_micros = read_u64(header + 96);
    tap->frame_size = (size_t)bytes;
    return true;
}

bool
kmx_tap_start(kmx_tap *tap, const char *path, const char *session) {
    struct sockaddr_un address;
    struct stat status;
    int flags;

    if (!tap || !path || path[0] != '/' || !session || !*session ||
        strlen(path) >= sizeof address.sun_path ||
        strlen(session) > KMX_TAP_SESSION_MAX) {
        errno = EINVAL;
        return false;
    }
    memset(tap, 0, sizeof *tap);
    tap->listener = -1;
    tap->source = -1;
    if (lstat(path, &status) == 0) {
        errno = EEXIST;
        return false;
    }
    if (errno != ENOENT) return false;

    tap->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (tap->listener < 0) return false;
    flags = fcntl(tap->listener, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(tap->listener, F_SETFL, flags | O_NONBLOCK) != 0) {
        kmx_tap_stop(tap);
        return false;
    }

    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(tap->listener, (struct sockaddr *)&address, sizeof address) != 0) {
        kmx_tap_stop(tap);
        return false;
    }
    tap->owns_path = true;
    if (chmod(path, 0600) != 0 || listen(tap->listener, 1) != 0) {
        kmx_tap_stop(tap);
        return false;
    }
    memcpy(tap->path, path, strlen(path) + 1);
    memcpy(tap->session, session, strlen(session) + 1);
    return true;
}

void
kmx_tap_stop(kmx_tap *tap) {
    if (!tap) return;
    if (tap->source >= 0) close(tap->source);
    if (tap->listener >= 0) close(tap->listener);
    if (tap->owns_path && tap->path[0]) unlink(tap->path);
    free(tap->frame);
    memset(tap, 0, sizeof *tap);
    tap->listener = -1;
    tap->source = -1;
}

int
kmx_tap_listener_fd(const kmx_tap *tap) {
    return tap ? tap->listener : -1;
}

int
kmx_tap_source_fd(const kmx_tap *tap) {
    return tap ? tap->source : -1;
}

static void
accept_source(kmx_tap *tap) {
    int accepted;
    int flags;
    if (!tap || tap->listener < 0) return;
    accepted = accept4(tap->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (accepted < 0) return;
    if (tap->source >= 0 || !peer_is_owner(accepted)) {
        close(accepted);
        return;
    }
    flags = fcntl(accepted, F_GETFL, 0);
    if (flags < 0 || fcntl(accepted, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(accepted);
        return;
    }
    tap->source = accepted;
    reset_message(tap);
}

static void
read_source(kmx_tap *tap) {
    while (tap->source >= 0 && !tap->have_frame) {
        unsigned char *target;
        size_t remaining;
        ssize_t count;

        if (tap->header_used < KMX_TAP_HEADER_SIZE) {
            target = tap->header + tap->header_used;
            remaining = KMX_TAP_HEADER_SIZE - tap->header_used;
        } else {
            if (!tap->frame_size && !valid_header(tap)) {
                drop_source(tap);
                return;
            }
            if (tap->frame_size > tap->frame_capacity) {
                unsigned char *replacement = realloc(tap->frame, tap->frame_size);
                if (!replacement) {
                    drop_source(tap);
                    return;
                }
                tap->frame = replacement;
                tap->frame_capacity = tap->frame_size;
            }
            target = tap->frame + tap->frame_used;
            remaining = tap->frame_size - tap->frame_used;
        }

        count = recv(tap->source, target, remaining, MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            drop_source(tap);
            return;
        }
        if (count == 0) {
            drop_source(tap);
            return;
        }
        if (tap->header_used < KMX_TAP_HEADER_SIZE) {
            tap->header_used += (size_t)count;
            if (tap->header_used == KMX_TAP_HEADER_SIZE && !valid_header(tap)) {
                drop_source(tap);
                return;
            }
        } else {
            tap->frame_used += (size_t)count;
            if (tap->frame_used == tap->frame_size) tap->have_frame = true;
        }
    }
}

void
kmx_tap_poll(kmx_tap *tap, short listener_events, short source_events) {
    if (!tap) return;
    if (listener_events & POLLIN) accept_source(tap);
    if (source_events & (POLLIN | POLLHUP | POLLERR)) read_source(tap);
}

bool
kmx_tap_take(
    kmx_tap *tap,
    const unsigned char **rgb,
    int *width,
    int *height,
    int *columns,
    int *rows,
    int *scroll_x,
    int *scroll_y,
    uint64_t *offered_micros
) {
    if (!tap || !tap->have_frame || !rgb) return false;
    *rgb = tap->frame;
    if (width) *width = tap->width;
    if (height) *height = tap->height;
    if (columns) *columns = tap->columns;
    if (rows) *rows = tap->rows;
    if (scroll_x) *scroll_x = tap->scroll_x;
    if (scroll_y) *scroll_y = tap->scroll_y;
    if (offered_micros) *offered_micros = tap->offered_micros;
    tap->have_frame = false;
    reset_message(tap);
    return true;
}
