#ifndef KILIX_MUX_ENDPOINT_H
#define KILIX_MUX_ENDPOINT_H

/* Internal to the tools: where a session listens and how a client reaches it.
 * Not part of the installed API, because the library does not own I/O policy. */

#include <stdbool.h>

typedef enum {
    KMX_ENDPOINT_UNIX = 0,
    KMX_ENDPOINT_TCP = 1
} kmx_endpoint_kind;

typedef struct {
    int kind;
    char path[108];
    char host[256];
    int port;
} kmx_endpoint;

/* "/tmp/s.sock" or "./s.sock" is a path; "host:port" or ":port" is TCP. */
bool kmx_endpoint_parse(const char *text, kmx_endpoint *endpoint);
bool kmx_endpoint_is_loopback(const kmx_endpoint *endpoint);

/* A non-loopback bind is refused unless explicitly allowed, and refused
 * rather than narrowed, so a caller cannot believe it is reachable when it is
 * not. */
int kmx_endpoint_listen(const kmx_endpoint *endpoint, bool allow_public);
int kmx_endpoint_connect(const kmx_endpoint *endpoint);
void kmx_endpoint_tune(int fd, const kmx_endpoint *endpoint);
void kmx_endpoint_cleanup(const kmx_endpoint *endpoint);

#endif
