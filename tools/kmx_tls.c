/* Transport confidentiality, on the SSH model.
 *
 * No certificate authority: a personal session between two machines that
 * already know each other does not need one, and pretending otherwise means
 * either a CA nobody maintains or a client that accepts any certificate - the
 * second being worse than no TLS, because it looks like security.
 *
 * So the server mints a key and a self-signed certificate at startup, keeps
 * both in memory, and prints the certificate's fingerprint.  The client is
 * given that fingerprint and refuses anything else.  Trust is established once,
 * out of band, by the person who can read both screens. */
#define _GNU_SOURCE

#include "kmx_tls.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kmx_tls_server {
    SSL_CTX *context;
    EVP_PKEY *key;
    X509 *certificate;
};

struct kmx_tls_client {
    SSL_CTX *context;
    char fingerprint[KMX_TLS_FINGERPRINT_HEX + 1];
};

struct kmx_tls_session {
    SSL *ssl;
    bool wants_write;
};

static void
hex_digest(const unsigned char *digest, unsigned int length, char *out) {
    static const char hex[] = "0123456789abcdef";
    unsigned int index;
    for (index = 0; index < length; index++) {
        out[index * 2] = hex[digest[index] >> 4];
        out[index * 2 + 1] = hex[digest[index] & 15];
    }
    out[length * 2] = '\0';
}

static bool
certificate_fingerprint(X509 *certificate, char *out, size_t size) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (size <= KMX_TLS_FINGERPRINT_HEX) return false;
    if (!X509_digest(certificate, EVP_sha256(), digest, &length)) return false;
    /* The fingerprint comparison reads exactly KMX_TLS_FINGERPRINT_HEX
     * characters, so a digest of any other length would leave part of `out`
     * unwritten and the comparison would run over uninitialised bytes.  SHA-256
     * always gives 32, but that is a fact about the algorithm rather than
     * something this function was checking. */
    if ((size_t)length * 2u != KMX_TLS_FINGERPRINT_HEX) return false;
    hex_digest(digest, length, out);
    return true;
}

/* A certificate with no name in it, because there is no name to verify: the
 * fingerprint is the identity. */
static bool
make_self_signed(EVP_PKEY **key_out, X509 **certificate_out) {
    EVP_PKEY *key = NULL;
    X509 *certificate = NULL;
    X509_NAME *name = NULL;

    key = EVP_RSA_gen(2048);
    if (!key) return false;
    certificate = X509_new();
    if (!certificate) {
        EVP_PKEY_free(key);
        return false;
    }
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
    /* A day: this exists for one session, not for a fleet. */
    X509_gmtime_adj(X509_getm_notAfter(certificate), 60 * 60 * 24);
    X509_set_pubkey(certificate, key);
    name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, (const unsigned char *)"kilix-multiplexer",
        -1, -1, 0);
    X509_set_issuer_name(certificate, name);
    if (!X509_sign(certificate, key, EVP_sha256())) {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return false;
    }
    *key_out = key;
    *certificate_out = certificate;
    return true;
}

kmx_tls_server *
kmx_tls_server_create(char *fingerprint, size_t size) {
    kmx_tls_server *server = calloc(1, sizeof *server);
    if (!server) return NULL;
    if (!make_self_signed(&server->key, &server->certificate)) {
        free(server);
        return NULL;
    }
    server->context = SSL_CTX_new(TLS_server_method());
    if (!server->context) {
        kmx_tls_server_free(server);
        return NULL;
    }
    /* Nothing before 1.2, and 1.3 in practice with any current library. */
    SSL_CTX_set_min_proto_version(server->context, TLS1_2_VERSION);
    /* The outbound queue is a growable buffer: it is compacted when its sent
     * prefix gets large, and appending to it can realloc.  So the pointer handed
     * to SSL_write can legitimately move between a WANT_WRITE and the retry -
     * and OpenSSL treats that as a caller bug (SSL_R_BAD_WRITE_RETRY) and kills
     * the session, rather than as the ordinary thing it is here.
     *
     * Measured before this was set: a TLS client that stopped reading died at
     * about 320 kB of backlog, against a 4 MiB limit, with the server silently
     * losing the connection instead of dropping frames.  Over plain TCP the same
     * client survived and the drop counters engaged as designed.
     *
     * The alternative is to promise OpenSSL a stable address by never compacting
     * or reallocating while a write is pending, which means a second buffering
     * discipline for the TLS case alone.  This flag is what the mode exists
     * for. */
    SSL_CTX_set_mode(server->context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_use_certificate(server->context, server->certificate) != 1 ||
        SSL_CTX_use_PrivateKey(server->context, server->key) != 1 ||
        !certificate_fingerprint(server->certificate, fingerprint, size)) {
        kmx_tls_server_free(server);
        return NULL;
    }
    return server;
}

void
kmx_tls_server_free(kmx_tls_server *server) {
    if (!server) return;
    if (server->context) SSL_CTX_free(server->context);
    if (server->certificate) X509_free(server->certificate);
    if (server->key) EVP_PKEY_free(server->key);
    free(server);
}

static kmx_tls_session *
wrap(SSL *ssl) {
    kmx_tls_session *session = calloc(1, sizeof *session);
    if (!session) {
        SSL_free(ssl);
        return NULL;
    }
    session->ssl = ssl;
    return session;
}

kmx_tls_session *
kmx_tls_server_begin(kmx_tls_server *server, int fd) {
    SSL *ssl;
    if (!server) return NULL;
    ssl = SSL_new(server->context);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    return wrap(ssl);
}

kmx_tls_progress
kmx_tls_server_step(kmx_tls_session *session) {
    int rc;
    if (!session) return KMX_TLS_FAILED;
    rc = SSL_accept(session->ssl);
    if (rc == 1) return KMX_TLS_DONE;
    switch (SSL_get_error(session->ssl, rc)) {
    case SSL_ERROR_WANT_READ:
        return KMX_TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
        return KMX_TLS_WANT_WRITE;
    default:
        return KMX_TLS_FAILED;
    }
}

kmx_tls_client *
kmx_tls_client_create(const char *fingerprint) {
    kmx_tls_client *client;
    /* Refusing to run without one is deliberate.  A client that would take any
     * certificate has no more assurance than no TLS at all, and a comforting
     * one, which is worse. */
    if (!fingerprint || strlen(fingerprint) != KMX_TLS_FINGERPRINT_HEX) return NULL;
    client = calloc(1, sizeof *client);
    if (!client) return NULL;
    memcpy(client->fingerprint, fingerprint, KMX_TLS_FINGERPRINT_HEX);
    client->context = SSL_CTX_new(TLS_client_method());
    if (!client->context) {
        free(client);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(client->context, TLS1_2_VERSION);
    /* Same reason as the server side.  The outbound queue is a growable buffer: it is compacted when its sent
     * prefix gets large, and appending to it can realloc.  So the pointer handed
     * to SSL_write can legitimately move between a WANT_WRITE and the retry -
     * and OpenSSL treats that as a caller bug (SSL_R_BAD_WRITE_RETRY) and kills
     * the session, rather than as the ordinary thing it is here.
     *
     * Measured before this was set: a TLS client that stopped reading died at
     * about 320 kB of backlog, against a 4 MiB limit, with the server silently
     * losing the connection instead of dropping frames.  Over plain TCP the same
     * client survived and the drop counters engaged as designed.
     *
     * The alternative is to promise OpenSSL a stable address by never compacting
     * or reallocating while a write is pending, which means a second buffering
     * discipline for the TLS case alone.  This flag is what the mode exists
     * for. */
    SSL_CTX_set_mode(client->context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    /* Chain verification is off because there is no chain; the fingerprint
     * below is the whole check, and it is stricter than a CA would be. */
    SSL_CTX_set_verify(client->context, SSL_VERIFY_NONE, NULL);
    return client;
}

void
kmx_tls_client_free(kmx_tls_client *client) {
    if (!client) return;
    if (client->context) SSL_CTX_free(client->context);
    free(client);
}

kmx_tls_session *
kmx_tls_client_connect(kmx_tls_client *client, int fd) {
    SSL *ssl;
    X509 *presented;
    char seen[KMX_TLS_FINGERPRINT_HEX + 1];
    bool ok;

    if (!client) return NULL;
    ssl = SSL_new(client->context);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    presented = SSL_get1_peer_certificate(ssl);
    if (!presented) {
        SSL_free(ssl);
        return NULL;
    }
    ok = certificate_fingerprint(presented, seen, sizeof seen);
    X509_free(presented);
    if (!ok) {
        SSL_free(ssl);
        return NULL;
    }
    /* Constant-time, for the same reason the token check is: a comparison that
     * stops at the first difference leaks how much of it matched. */
    {
        unsigned char difference = 0;
        size_t index;
        for (index = 0; index < KMX_TLS_FINGERPRINT_HEX; index++) {
            difference |= (unsigned char)(seen[index] ^ client->fingerprint[index]);
        }
        if (difference != 0) {
            SSL_free(ssl);
            return NULL;
        }
    }
    return wrap(ssl);
}

long
kmx_tls_read(kmx_tls_session *session, void *data, size_t size) {
    int count;
    if (!session) return -1;
    session->wants_write = false;
    count = SSL_read(session->ssl, data, (int)size);
    if (count > 0) return count;
    switch (SSL_get_error(session->ssl, count)) {
        case SSL_ERROR_WANT_READ:
            errno = EAGAIN;
            return -1;
        case SSL_ERROR_WANT_WRITE:
            /* A read that needs the socket writable: renegotiation, or a
             * handshake message.  The poll loop has to be told. */
            session->wants_write = true;
            errno = EAGAIN;
            return -1;
        case SSL_ERROR_ZERO_RETURN:
            return 0;
        default:
            /* Set explicitly.  SSL_ERROR_SSL and ZERO_RETURN do not touch
             * errno, and this layer plants EAGAIN on the same connection on
             * every ordinary would-block - so a caller reading a stale errno
             * after a hard failure reliably sees EAGAIN and waits forever for a
             * session that can never make progress. */
            errno = EIO;
            return 0;
    }
}

long
kmx_tls_write(kmx_tls_session *session, const void *data, size_t size) {
    int count;
    if (!session) return -1;
    session->wants_write = false;
    count = SSL_write(session->ssl, data, (int)size);
    if (count > 0) return count;
    switch (SSL_get_error(session->ssl, count)) {
        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_WANT_READ:
            session->wants_write = true;
            errno = EAGAIN;
            return -1;
        default:
            /* See kmx_tls_read: never leave errno holding this connection's own
             * stale EAGAIN on a fatal error. */
            errno = EIO;
            return -1;
    }
}

bool
kmx_tls_wants_write(const kmx_tls_session *session) {
    return session ? session->wants_write : false;
}

void
kmx_tls_session_free(kmx_tls_session *session) {
    if (!session) return;
    if (session->ssl) {
        SSL_shutdown(session->ssl);
        SSL_free(session->ssl);
    }
    free(session);
}
