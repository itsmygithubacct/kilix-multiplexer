#include "kilix_mux_input_session.h"

#include <string.h>

static bool
token_nonzero(const unsigned char token[KMX_INPUT_TOKEN_SIZE]) {
    unsigned char combined = 0;
    for (size_t i = 0; i < KMX_INPUT_TOKEN_SIZE; i++) combined |= token[i];
    return combined != 0;
}

kmx_result
kmx_input_session_init(kmx_input_session *session,
    const unsigned char initial_epoch[KMX_INPUT_TOKEN_SIZE], uint32_t grace_ms) {
    kmx_input_session value = {0};
    if (!session || !initial_epoch || !token_nonzero(initial_epoch)) return KMX_ERR_INVALID;
    memcpy(value.epoch, initial_epoch, sizeof value.epoch);
    value.grace_ms = grace_ms;
    *session = value;
    return KMX_OK;
}

void
kmx_input_session_expire(kmx_input_session *session, uint64_t now) {
    if (session && session->retained && !session->owner && now >= session->expires) {
        session->retained = false;
    }
}

void
kmx_input_session_disconnect(kmx_input_session *session, uint64_t owner, uint64_t now) {
    if (!session || !owner || session->owner != owner) return;
    session->owner = 0;
    session->expires = now > UINT64_MAX - session->grace_ms
        ? UINT64_MAX : now + session->grace_ms;
}

kmx_input_status
kmx_input_session_open(kmx_input_session *session,
    const kmx_input_open *request, uint64_t owner, uint64_t now,
    const unsigned char fresh_epoch[KMX_INPUT_TOKEN_SIZE],
    bool allowed, bool other_controller, uint64_t *fenced_owner) {
    kmx_input_session next;
    bool fresh;
    if (fenced_owner) *fenced_owner = 0;
    if (!session || !request || !owner || !fenced_owner ||
        !token_nonzero(session->epoch) || !token_nonzero(request->client_id)) {
        return KMX_INPUT_DENIED;
    }
    next = *session;
    kmx_input_session_expire(&next, now);
    fresh = !token_nonzero(request->epoch);
    if (!allowed) return KMX_INPUT_DENIED;
    if (!fresh && memcmp(request->epoch, next.epoch, sizeof next.epoch)) return KMX_INPUT_EPOCH;
    if (fresh && request->last_ack) return KMX_INPUT_DENIED;
    if (!fresh && (!next.retained ||
        memcmp(request->client_id, next.client_id, sizeof next.client_id))) {
        return KMX_INPUT_EXPIRED;
    }
    if (next.retained && memcmp(request->client_id, next.client_id, sizeof next.client_id)) {
        return KMX_INPUT_BUSY;
    }
    if (next.retained && (request->last_ack > next.accepted || (fresh && next.accepted))) {
        return KMX_INPUT_DENIED;
    }
    if (!next.retained) {
        if (other_controller) return KMX_INPUT_BUSY;
        if (!fresh_epoch || !token_nonzero(fresh_epoch) ||
            !memcmp(fresh_epoch, next.epoch, sizeof next.epoch)) return KMX_INPUT_DENIED;
        memcpy(next.epoch, fresh_epoch, sizeof next.epoch);
        memcpy(next.client_id, request->client_id, sizeof next.client_id);
        next.accepted = 0;
        next.retained = true;
    }
    if (next.owner && next.owner != owner) *fenced_owner = next.owner;
    next.owner = owner;
    next.expires = 0;
    *session = next;
    return KMX_INPUT_READY;
}

kmx_result
kmx_input_session_classify(const kmx_input_session *session,
    uint64_t owner, uint64_t sequence, bool *duplicate) {
    if (!session || !duplicate) return KMX_ERR_INVALID;
    if (!owner || !sequence || !session->retained || session->owner != owner) {
        return KMX_ERR_PROTOCOL;
    }
    if (sequence <= session->accepted) {
        *duplicate = true;
        return KMX_OK;
    }
    if (session->accepted == UINT64_MAX || sequence != session->accepted + 1) {
        return KMX_ERR_PROTOCOL;
    }
    *duplicate = false;
    return KMX_OK;
}

kmx_result
kmx_input_session_commit(kmx_input_session *session, uint64_t owner, uint64_t sequence) {
    bool duplicate;
    kmx_result result = kmx_input_session_classify(session, owner, sequence, &duplicate);
    if (result != KMX_OK) return result;
    if (duplicate) return KMX_ERR_PROTOCOL;
    session->accepted = sequence;
    return KMX_OK;
}

bool
kmx_input_session_close(kmx_input_session *session, uint64_t owner, uint64_t last_ack) {
    if (!session || !owner || !session->retained || session->owner != owner ||
        last_ack != session->accepted) return false;
    session->owner = 0;
    session->retained = false;
    return true;
}
