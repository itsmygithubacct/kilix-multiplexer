#define _GNU_SOURCE

#include "kmx_tap.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "not ok %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++;                                                          \
    }                                                                        \
} while (0)

static int failures;

static void
put_u32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void
put_u64(unsigned char *out, uint64_t value) {
    int index;
    for (index = 7; index >= 0; index--) {
        out[index] = (unsigned char)value;
        value >>= 8;
    }
}

static void
make_header(
    unsigned char header[112],
    const char *session,
    uint32_t width,
    uint32_t height,
    uint64_t frame_size
) {
    size_t session_size = strlen(session);
    memset(header, 0, 112);
    memcpy(header, "KFT1", 4);
    header[5] = 1;
    if (session_size > 64) session_size = 64;
    memcpy(header + 8, session, session_size);
    put_u32(header + 72, width);
    put_u32(header + 76, height);
    put_u32(header + 80, 20);
    put_u32(header + 84, 8);
    put_u32(header + 88, (uint32_t)-3);
    put_u32(header + 92, 4);
    put_u64(header + 96, 123456789u);
    put_u64(header + 104, frame_size);
}

static int
connect_source(const char *path) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
write_all(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static void
test_frame_socket(void) {
    char directory[] = "/tmp/kmx-tap-test.XXXXXX";
    char path[108];
    char session[65];
    unsigned char header[112];
    unsigned char pixels[12];
    const unsigned char *received = NULL;
    struct stat info;
    kmx_tap tap;
    int producer;
    int width = 0;
    int height = 0;
    int columns = 0;
    int rows = 0;
    int scroll_x = 0;
    int scroll_y = 0;
    uint64_t offered = 0;
    size_t index;

    memset(session, 'a', 64);
    session[64] = '\0';
    for (index = 0; index < sizeof pixels; index++) {
        pixels[index] = (unsigned char)(index * 7u);
    }
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof path, "%s/frames.sock", directory) > 0);
    CHECK(kmx_tap_start(&tap, path, session));
    CHECK(lstat(path, &info) == 0);
    CHECK(S_ISSOCK(info.st_mode));
    CHECK((info.st_mode & 0777) == 0600);

    producer = connect_source(path);
    CHECK(producer >= 0);
    kmx_tap_poll(&tap, POLLIN, 0);
    CHECK(kmx_tap_source_fd(&tap) >= 0);

    make_header(header, session, 2, 2, sizeof pixels);
    CHECK(write_all(producer, header, 17) == 0);
    kmx_tap_poll(&tap, 0, POLLIN);
    CHECK(!kmx_tap_take(
        &tap, &received, NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    CHECK(write_all(producer, header + 17, sizeof header - 17) == 0);
    CHECK(write_all(producer, pixels, sizeof pixels) == 0);
    kmx_tap_poll(&tap, 0, POLLIN);
    CHECK(kmx_tap_take(
        &tap, &received, &width, &height, &columns, &rows,
        &scroll_x, &scroll_y, &offered));
    CHECK(width == 2 && height == 2);
    CHECK(columns == 20 && rows == 8);
    CHECK(scroll_x == -3 && scroll_y == 4);
    CHECK(offered == 123456789u);
    CHECK(received && memcmp(received, pixels, sizeof pixels) == 0);

    close(producer);
    kmx_tap_poll(&tap, 0, POLLHUP);
    CHECK(kmx_tap_source_fd(&tap) == -1);

    producer = connect_source(path);
    CHECK(producer >= 0);
    kmx_tap_poll(&tap, POLLIN, 0);
    make_header(header, "wrong-session", 2, 2, sizeof pixels);
    CHECK(write_all(producer, header, sizeof header) == 0);
    CHECK(write_all(producer, pixels, sizeof pixels) == 0);
    kmx_tap_poll(&tap, 0, POLLIN);
    CHECK(kmx_tap_source_fd(&tap) == -1);
    CHECK(!kmx_tap_take(
        &tap, &received, NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    close(producer);

    kmx_tap_stop(&tap);
    CHECK(access(path, F_OK) != 0 && errno == ENOENT);
    CHECK(rmdir(directory) == 0);
}

int
main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_frame_socket();
    if (failures) {
        fprintf(stderr, "%d frame tap check(s) failed\n", failures);
        return 1;
    }
    puts("all frame tap checks passed");
    return 0;
}
