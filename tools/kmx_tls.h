#ifndef KILIX_MUX_TLS_H
#define KILIX_MUX_TLS_H

/* Transport confidentiality.
 *
 * A token says who may attach.  It says nothing about who may watch, and on a
 * reachable network that is the larger of the two problems: a terminal carries
 * whatever the person typed into it.
 *
 * There is no certificate authority here and there should not be one for a
 * personal session.  The server generates a key and a self-signed certificate
 * when it starts and prints the certificate's fingerprint; the client is given
 * that fingerprint and refuses anything else.  That is SSH's model, and it is
 * the right one for two machines that already know each other. */

#include <stdbool.h>
#include <stddef.h>

typedef struct kmx_tls_server kmx_tls_server;
typedef struct kmx_tls_client kmx_tls_client;
typedef struct kmx_tls_session kmx_tls_session;

/* SHA-256 of the certificate, hex, as printed and compared. */
#define KMX_TLS_FINGERPRINT_HEX 64

/* Generate a key and a self-signed certificate, both held in memory and never
 * written to disk. `fingerprint` receives the hex digest. */
kmx_tls_server *kmx_tls_server_create(char *fingerprint, size_t size);
void kmx_tls_server_free(kmx_tls_server *server);

/* How a handshake in progress wants to be driven. */
typedef enum {
    KMX_TLS_DONE,
    KMX_TLS_WANT_READ,
    KMX_TLS_WANT_WRITE,
    KMX_TLS_FAILED
} kmx_tls_progress;

/* Wrap an accepted socket without performing the handshake.
 *
 * Split in two because a handshake done inline is a handshake a peer controls
 * the duration of: a single-threaded server that runs SSL_accept to completion
 * inside its accept path stops serving everyone else until some stranger
 * finishes talking, or does not.  So the socket stays non-blocking and the
 * caller drives kmx_tls_server_step from its poll loop, under whatever deadline
 * it considers reasonable. */
kmx_tls_session *kmx_tls_server_begin(kmx_tls_server *server, int fd);
kmx_tls_progress kmx_tls_server_step(kmx_tls_session *session);

/* `fingerprint` is required: a client that would take any certificate has no
 * more assurance than no TLS at all, and a comforting one. */
kmx_tls_client *kmx_tls_client_create(const char *fingerprint);
void kmx_tls_client_free(kmx_tls_client *client);
kmx_tls_session *kmx_tls_client_connect(kmx_tls_client *client, int fd);

/* Read and write with the same shape as recv/send: -1 with EAGAIN when the
 * caller should wait, 0 at end of stream. */
long kmx_tls_read(kmx_tls_session *session, void *data, size_t size);
long kmx_tls_write(kmx_tls_session *session, const void *data, size_t size);
/* True when the last operation needs the socket writable before it can make
 * progress, which a poll loop has to know about. */
bool kmx_tls_wants_write(const kmx_tls_session *session);
void kmx_tls_session_free(kmx_tls_session *session);

#endif
