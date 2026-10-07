/* Test-only OpenSSL fault injection. The real client completes a pinned TLS
 * handshake; its first application write then waits for READ, transitions to
 * WRITE after the peer speaks, and resumes on the real encrypted transport. */
#define _POSIX_C_SOURCE 200809L
#include <openssl/ssl.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "TLS_WAIT_TEST check failed: %s\n", #condition); abort(); \
} } while (0)

int __real_SSL_write(SSL *ssl, const void *data, int size);
int __real_SSL_get_error(const SSL *ssl, int result);

static unsigned phase;
static unsigned read_retries;
static unsigned write_retries;
static int injected_error;
static const SSL *injected_session;
static uint64_t started;
static unsigned char *original;
static int original_size;

static uint64_t millis(void) {
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void report(void) {
    fprintf(stderr, "TLS_WAIT_TEST phase=%u read_retries=%u write_retries=%u\n",
            phase, read_retries, write_retries);
    free(original);
}

int __wrap_SSL_write(SSL *ssl, const void *data, int size) {
    if (!original) {
        CHECK(size > 0);
        original = malloc((size_t)size);
        CHECK(original);
        original_size = size;
        memcpy(original, data, (size_t)size);
        started = millis();
        CHECK(atexit(report) == 0);
    }
    if (phase < 2) {
        /* Appending ACKs, resize, and input must not alter the TLS retry. */
        CHECK(size == original_size);
        CHECK(memcmp(data, original, (size_t)size) == 0);
    }
    if (phase == 0) {
        unsigned char byte;
        long available = recv(SSL_get_fd(ssl), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        injected_session = ssl;
        if (millis() - started >= 200u && available > 0) {
            phase = 1;
            write_retries++;
            injected_error = SSL_ERROR_WANT_WRITE;
            fprintf(stderr, "TLS_WAIT_TEST direction=WRITE\n");
        } else {
            read_retries++;
            injected_error = SSL_ERROR_WANT_READ;
            if (read_retries == 1) fprintf(stderr, "TLS_WAIT_TEST direction=READ\n");
            if (read_retries == 10) fprintf(stderr, "TLS_WAIT_TEST excessive_read_retries\n");
        }
        errno = EAGAIN;
        return -1;
    }
    if (phase == 1) {
        phase = 2;
        fprintf(stderr, "TLS_WAIT_TEST resumed\n");
    }
    return __real_SSL_write(ssl, data, size);
}

int __wrap_SSL_get_error(const SSL *ssl, int result) {
    if (injected_error && ssl == injected_session && result == -1) {
        int error = injected_error;
        injected_error = 0;
        injected_session = NULL;
        return error;
    }
    return __real_SSL_get_error(ssl, result);
}
