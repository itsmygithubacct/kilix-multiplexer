#ifndef KILIX_MUX_INPUT_SESSION_H
#define KILIX_MUX_INPUT_SESSION_H

#include "kilix_mux_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One acknowledged-input ledger and its exclusive controller lease. Owners
 * are caller-issued, nonzero transport identities: do not reuse an identity
 * while callbacks from its old transport can still arrive. This module owns
 * no input bytes, transport, randomness, or clock. */
typedef struct {
    unsigned char epoch[KMX_INPUT_TOKEN_SIZE];
    unsigned char client_id[KMX_INPUT_TOKEN_SIZE];
    uint64_t accepted;
    uint64_t expires;
    uint64_t owner; /* zero while disconnected */
    bool retained;
    uint32_t grace_ms;
} kmx_input_session;

/* Initial epoch must be nonzero. A zero grace is valid (immediate expiry).
 * Invalid arguments leave *session unchanged. */
kmx_result kmx_input_session_init(kmx_input_session *session,
    const unsigned char initial_epoch[KMX_INPUT_TOKEN_SIZE], uint32_t grace_ms);
void kmx_input_session_expire(kmx_input_session *session, uint64_t now);
/* Only the active owner starts grace. Stale/repeated disconnects do nothing.
 * The deadline saturates at UINT64_MAX rather than wrapping. */
void kmx_input_session_disconnect(kmx_input_session *session,
    uint64_t owner, uint64_t now);

/* OPEN validates a request and checks expiry at now. All refusals leave the
 * session unchanged; callers can use expire() to retire an idle ledger.
 * On READY a new ledger uses fresh_epoch, which must be nonzero and different
 * from the previous epoch. A retained resume keeps its epoch and counter;
 * fresh_epoch may be NULL when no allocation is needed. Other controllers
 * block only allocation. The required fenced_owner output defaults to zero
 * and receives the replaced transport identity on a successful takeover. */
kmx_input_status kmx_input_session_open(kmx_input_session *session,
    const kmx_input_open *request, uint64_t owner, uint64_t now,
    const unsigned char fresh_epoch[KMX_INPUT_TOKEN_SIZE],
    bool allowed, bool other_controller, uint64_t *fenced_owner);

/* Classify before admission to the caller's FIFO. Duplicates never append.
 * A next command is committed only after its entire payload is owned by that
 * FIFO; a blocked admission must not commit. These calls recheck ownership.
 * Classification leaves duplicate unchanged on error; both calls leave the
 * session unchanged on error. An ACK describes memory ownership, not PTY
 * delivery or application execution. */
kmx_result kmx_input_session_classify(const kmx_input_session *session,
    uint64_t owner, uint64_t sequence, bool *duplicate);
kmx_result kmx_input_session_commit(kmx_input_session *session,
    uint64_t owner, uint64_t sequence);
/* Release only when the active owner knows the exact accepted counter.
 * Diagnostics (epoch, client_id, accepted and expires) remain available. */
bool kmx_input_session_close(kmx_input_session *session,
    uint64_t owner, uint64_t last_ack);

#ifdef __cplusplus
}
#endif

#endif
