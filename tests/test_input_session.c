#include "kilix_mux_input_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char initial_epoch[KMX_INPUT_TOKEN_SIZE] = {1};
static const unsigned char first_epoch[KMX_INPUT_TOKEN_SIZE] = {2};
static const unsigned char second_epoch[KMX_INPUT_TOKEN_SIZE] = {3};
static const unsigned char third_epoch[KMX_INPUT_TOKEN_SIZE] = {4};

static void
require(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL  %s\n", message);
    exit(1);
}

static kmx_input_open
request_for(const unsigned char *epoch, unsigned char identity, uint64_t ack) {
    kmx_input_open request = {0};
    if (epoch) memcpy(request.epoch, epoch, sizeof request.epoch);
    request.client_id[15] = identity;
    request.last_ack = ack;
    return request;
}

static void
snapshot(kmx_input_session *saved, const kmx_input_session *session) {
    memcpy(saved, session, sizeof *saved);
}

static void
unchanged(const kmx_input_session *session, const kmx_input_session *saved) {
    require(memcmp(session, saved, sizeof *session) == 0, "session unchanged including padding");
}

static void
refuse(kmx_input_session *session, const kmx_input_open *request,
    uint64_t owner, uint64_t now, const unsigned char *epoch,
    bool allowed, bool other, kmx_input_status expected) {
    kmx_input_session saved;
    uint64_t fenced = UINT64_MAX;
    kmx_input_status status;
    snapshot(&saved, session);
    status = kmx_input_session_open(session, request, owner, now, epoch,
                                   allowed, other, &fenced);
    if (status != expected) {
        fprintf(stderr, "FAIL  OPEN refusal expected %u, got %u\n",
                (unsigned)expected, (unsigned)status);
        exit(1);
    }
    require(fenced == 0, "refused OPEN never fences a transport");
    unchanged(session, &saved);
}

static void
reject_sequence(kmx_input_session *session, uint64_t owner, uint64_t sequence) {
    kmx_input_session saved;
    bool duplicate = true;
    snapshot(&saved, session);
    require(kmx_input_session_classify(session, owner, sequence, &duplicate) == KMX_ERR_PROTOCOL,
            "classification rejects wrong owner, zero or gap");
    require(duplicate, "classification error leaves duplicate untouched");
    require(kmx_input_session_commit(session, owner, sequence) == KMX_ERR_PROTOCOL,
            "commit rejects wrong owner, zero or gap");
    unchanged(session, &saved);
}

static void
test_validation_and_open(void) {
    kmx_input_session session, saved;
    kmx_input_open request = request_for(NULL, 7, 0);
    const unsigned char zero[KMX_INPUT_TOKEN_SIZE] = {0};
    uint64_t fenced = UINT64_MAX;
    memset(&session, 0xa5, sizeof session);
    snapshot(&saved, &session);
    require(kmx_input_session_init(&session, NULL, 60000) == KMX_ERR_INVALID &&
            kmx_input_session_init(&session, zero, 60000) == KMX_ERR_INVALID &&
            kmx_input_session_init(NULL, initial_epoch, 60000) == KMX_ERR_INVALID,
            "init rejects missing or zero epoch");
    unchanged(&session, &saved);
    require(kmx_input_session_init(&session, initial_epoch, 60000) == KMX_OK,
            "initialize disconnected ledger");
    require(!session.retained && !session.owner && !session.accepted && !session.expires &&
            session.grace_ms == 60000 &&
            memcmp(session.epoch, initial_epoch, sizeof session.epoch) == 0,
            "initial fields and epoch");
    refuse(&session, &request, 0, 100, first_epoch, true, false, KMX_INPUT_DENIED);
    refuse(&session, NULL, 1, 100, first_epoch, true, false, KMX_INPUT_DENIED);
    refuse(&session, &request, 1, 100, first_epoch, false, false, KMX_INPUT_DENIED);
    refuse(&session, &request, 1, 100, first_epoch, true, true, KMX_INPUT_BUSY);
    refuse(&session, &request, 1, 100, NULL, true, false, KMX_INPUT_DENIED);
    refuse(&session, &request, 1, 100, zero, true, false, KMX_INPUT_DENIED);
    refuse(&session, &request, 1, 100, initial_epoch, true, false, KMX_INPUT_DENIED);
    request.client_id[15] = 0;
    refuse(&session, &request, 1, 100, first_epoch, true, false, KMX_INPUT_DENIED);
    request.client_id[15] = 7;
    request.last_ack = 1;
    refuse(&session, &request, 1, 100, first_epoch, true, false, KMX_INPUT_DENIED);
    request = request_for(second_epoch, 7, 0);
    refuse(&session, &request, 1, 100, first_epoch, true, false, KMX_INPUT_EPOCH);
    request = request_for(initial_epoch, 7, 0);
    refuse(&session, &request, 1, 100, first_epoch, true, false, KMX_INPUT_EXPIRED);
    request = request_for(NULL, 7, 0);
    snapshot(&saved, &session);
    require(kmx_input_session_open(&session, &request, 1, 100, first_epoch,
                true, false, NULL) == KMX_INPUT_DENIED, "fencing output required");
    unchanged(&session, &saved);
    require(kmx_input_session_open(NULL, &request, 1, 100, first_epoch,
                true, false, &fenced) == KMX_INPUT_DENIED && !fenced, "NULL session refused");
    require(kmx_input_session_open(&session, &request, 1, 100, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY && !fenced,
            "fresh allocation succeeds");
    require(session.owner == 1 && session.retained && !session.accepted && !session.expires &&
            memcmp(session.epoch, first_epoch, sizeof session.epoch) == 0 &&
            memcmp(session.client_id, request.client_id, sizeof session.client_id) == 0,
            "new allocation rotates epoch and assigns identity");
    /* A lost initial STATE can be recovered with zero epoch before any input
     * was accepted, without generating another ledger or counter reset. */
    require(kmx_input_session_open(&session, &request, 2, 110, NULL,
                true, true, &fenced) == KMX_INPUT_READY && fenced == 1,
            "zero epoch same key rebinds an empty retained ledger");
    require(memcmp(session.epoch, first_epoch, sizeof session.epoch) == 0,
            "empty rebind retains epoch");
    request.client_id[15] = 8;
    refuse(&session, &request, 3, 120, second_epoch, true, false, KMX_INPUT_BUSY);
    memcpy(request.epoch, first_epoch, sizeof request.epoch);
    refuse(&session, &request, 3, 120, second_epoch, true, false, KMX_INPUT_EXPIRED);
    request.client_id[15] = 7;
    request.last_ack = 1;
    refuse(&session, &request, 3, 120, second_epoch, true, false, KMX_INPUT_DENIED);
    require(kmx_input_session_commit(&session, 2, 1) == KMX_OK, "first committed command");
    request = request_for(NULL, 7, 0);
    refuse(&session, &request, 3, 120, second_epoch, true, false, KMX_INPUT_DENIED);
    request = request_for(first_epoch, 7, 1);
    require(kmx_input_session_open(&session, &request, 2, 130, second_epoch,
                true, true, &fenced) == KMX_INPUT_READY && !fenced,
            "active same owner resume does not fence itself");
    require(session.accepted == 1 && memcmp(session.epoch, first_epoch, sizeof session.epoch) == 0,
            "retained resume ignores distinct fresh candidate");
    bool duplicate = false;
    snapshot(&saved, &session);
    require(kmx_input_session_classify(NULL, 2, 1, &duplicate) == KMX_ERR_INVALID &&
            kmx_input_session_classify(&session, 2, 1, NULL) == KMX_ERR_INVALID &&
            kmx_input_session_commit(NULL, 2, 1) == KMX_ERR_INVALID,
            "NULL classification and commit arguments rejected");
    require(!duplicate, "NULL classification leaves output untouched");
    unchanged(&session, &saved);
}

static void
test_lost_ack_fencing_and_admission(void) {
    kmx_input_session session, saved;
    kmx_input_open request = request_for(NULL, 9, 0);
    uint64_t fenced;
    bool duplicate = true;
    require(kmx_input_session_init(&session, initial_epoch, 60000) == KMX_OK, "init lost ACK test");
    require(kmx_input_session_open(&session, &request, 10, 100, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY, "open lost ACK test");
    snapshot(&saved, &session);
    require(kmx_input_session_classify(&session, 10, 1, &duplicate) == KMX_OK && !duplicate,
            "first input classifies as new");
    /* A failed or blocked FIFO append has no state transition to undo. */
    unchanged(&session, &saved);
    require(!session.accepted, "blocked admission cannot advance accepted");
    require(kmx_input_session_commit(&session, 10, 1) == KMX_OK, "commit after complete FIFO append");
    kmx_input_session_disconnect(&session, 10, 200);
    require(session.retained && !session.owner && session.expires == 60200 && session.accepted == 1,
            "disconnect retains counter for exact grace");
    snapshot(&saved, &session);
    kmx_input_session_disconnect(&session, 10, 10000);
    kmx_input_session_disconnect(&session, 99, 10000);
    unchanged(&session, &saved);
    request = request_for(first_epoch, 9, 0); /* ACK1 was lost. */
    require(kmx_input_session_open(&session, &request, 11, 201, NULL,
                true, true, &fenced) == KMX_INPUT_READY && !fenced,
            "resume with lost ACK despite foreign legacy controller");
    require(session.accepted == 1 && !session.expires, "resume keeps accepted and clears deadline");
    require(kmx_input_session_classify(&session, 11, 1, &duplicate) == KMX_OK && duplicate,
            "lost ACK retry is a duplicate");
    snapshot(&saved, &session);
    require(kmx_input_session_commit(&session, 11, 1) == KMX_ERR_PROTOCOL,
            "duplicate cannot be committed twice");
    unchanged(&session, &saved);
    reject_sequence(&session, 11, 0);
    reject_sequence(&session, 11, 3);
    reject_sequence(&session, 0, 2);
    reject_sequence(&session, 10, 2);
    require(kmx_input_session_classify(&session, 11, 2, &duplicate) == KMX_OK && !duplicate,
            "next command can await FIFO admission");
    require(kmx_input_session_open(&session, &request, 12, 202, NULL,
                true, true, &fenced) == KMX_INPUT_READY && fenced == 11,
            "takeover returns previous transport for fencing");
    snapshot(&saved, &session);
    kmx_input_session_disconnect(&session, 11, 203);
    unchanged(&session, &saved);
    reject_sequence(&session, 11, 2);
    require(kmx_input_session_commit(&session, 12, 2) == KMX_OK,
            "new owner can commit after stale owner was fenced");
    snapshot(&saved, &session);
    require(!kmx_input_session_close(&session, 11, 2) &&
            !kmx_input_session_close(&session, 12, 1) &&
            !kmx_input_session_close(&session, 0, 2), "wrong owner or ACK cannot release lease");
    unchanged(&session, &saved);
    require(kmx_input_session_close(&session, 12, 2), "matching ACK releases active lease");
    require(!session.retained && !session.owner && session.accepted == 2 &&
            memcmp(session.epoch, first_epoch, sizeof session.epoch) == 0,
            "release preserves diagnostics");
    snapshot(&saved, &session);
    require(!kmx_input_session_close(&session, 12, 2), "repeated close fails");
    kmx_input_session_disconnect(&session, 12, 204);
    unchanged(&session, &saved);
    reject_sequence(&session, 12, 3);
}

static void
test_expiry_and_retired_identity(void) {
    kmx_input_session session, saved;
    kmx_input_open fresh = request_for(NULL, 13, 0), resume;
    uint64_t fenced;
    require(kmx_input_session_init(&session, initial_epoch, 60) == KMX_OK, "init expiry test");
    require(kmx_input_session_open(&session, &fresh, 20, 100, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY, "allocate expiry test");
    kmx_input_session_expire(&session, UINT64_MAX);
    require(session.retained, "connected ledger never expires");
    require(kmx_input_session_commit(&session, 20, 1) == KMX_OK, "commit before expiry");
    kmx_input_session_disconnect(&session, 20, 100);
    kmx_input_session_expire(&session, 159);
    require(session.retained && session.expires == 160, "retain just before expiry");
    resume = request_for(first_epoch, 13, 0);
    refuse(&session, &resume, 21, 160, second_epoch, true, false, KMX_INPUT_EXPIRED);
    /* Invalid candidates must not mutate even when logical expiry is due. */
    refuse(&session, &fresh, 21, 160, first_epoch, true, false, KMX_INPUT_DENIED);
    snapshot(&saved, &session);
    kmx_input_session_expire(&session, 160);
    require(!session.retained && session.accepted == saved.accepted &&
            session.expires == saved.expires && !session.owner,
            "deadline equality expires retention only");
    refuse(&session, &resume, 21, 160, second_epoch, true, false, KMX_INPUT_EXPIRED);
    require(kmx_input_session_open(&session, &fresh, 21, 161, second_epoch,
                true, false, &fenced) == KMX_INPUT_READY && !fenced && !session.accepted,
            "fresh identity after expiry allocates a rotated epoch");
    refuse(&session, &resume, 22, 162, first_epoch, true, false, KMX_INPUT_EPOCH);
    require(kmx_input_session_commit(&session, 21, 1) == KMX_OK, "new ledger accepts new sequence1");
    require(kmx_input_session_close(&session, 21, 1), "close new ledger");
    resume = request_for(second_epoch, 13, 0);
    refuse(&session, &resume, 22, 162, first_epoch, true, false, KMX_INPUT_EXPIRED);
    require(kmx_input_session_open(&session, &fresh, 22, 163, third_epoch,
                true, false, &fenced) == KMX_INPUT_READY,
            "same client ID after close still rotates previous epoch");
    refuse(&session, &resume, 23, 164, second_epoch, true, false, KMX_INPUT_EPOCH);
    /* Zero-grace sessions and saturating arithmetic are legitimate bounds. */
    require(kmx_input_session_init(&session, initial_epoch, 0) == KMX_OK, "zero grace valid");
    require(kmx_input_session_open(&session, &fresh, 24, 200, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY, "open zero grace");
    kmx_input_session_disconnect(&session, 24, 200);
    kmx_input_session_expire(&session, 199);
    require(session.retained, "zero grace preserves state before disconnect time");
    kmx_input_session_expire(&session, 200);
    require(!session.retained, "zero grace expires at disconnect time");
    require(kmx_input_session_init(&session, initial_epoch, UINT32_MAX) == KMX_OK,
            "maximum grace valid");
    require(kmx_input_session_open(&session, &fresh, 25, 200, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY, "open saturation test");
    kmx_input_session_disconnect(&session, 25, UINT64_MAX - 1);
    require(session.expires == UINT64_MAX, "grace saturates without wrap");
    kmx_input_session_expire(&session, UINT64_MAX - 1);
    require(session.retained, "saturated deadline retains before maximum");
    kmx_input_session_expire(&session, UINT64_MAX);
    require(!session.retained, "saturated deadline expires at maximum");
    kmx_input_session_expire(NULL, 0);
    kmx_input_session_disconnect(NULL, 1, 0);
    require(!kmx_input_session_close(NULL, 1, 0), "NULL lifecycle calls safe");
}

static void
test_terminal_sequence(void) {
    kmx_input_session session, saved;
    kmx_input_open request = request_for(NULL, 17, 0);
    uint64_t fenced;
    bool duplicate;
    require(kmx_input_session_init(&session, initial_epoch, 60) == KMX_OK, "init terminal counter");
    require(kmx_input_session_open(&session, &request, 30, 100, first_epoch,
                true, false, &fenced) == KMX_INPUT_READY, "open terminal counter");
    session.accepted = UINT64_MAX - 1; /* Public diagnostic state seeds boundary. */
    require(kmx_input_session_classify(&session, 30, UINT64_MAX, &duplicate) == KMX_OK && !duplicate,
            "last representable sequence is new");
    require(kmx_input_session_commit(&session, 30, UINT64_MAX) == KMX_OK &&
            session.accepted == UINT64_MAX, "commit final sequence without wrap");
    require(kmx_input_session_classify(&session, 30, UINT64_MAX, &duplicate) == KMX_OK && duplicate &&
            kmx_input_session_classify(&session, 30, 1, &duplicate) == KMX_OK && duplicate,
            "all nonzero sequences are duplicates at terminal counter");
    snapshot(&saved, &session);
    require(kmx_input_session_commit(&session, 30, UINT64_MAX) == KMX_ERR_PROTOCOL &&
            kmx_input_session_commit(&session, 30, 1) == KMX_ERR_PROTOCOL,
            "terminal counter cannot advance or commit duplicates");
    unchanged(&session, &saved);
    reject_sequence(&session, 30, 0);
    request = request_for(first_epoch, 17, UINT64_MAX);
    require(kmx_input_session_open(&session, &request, 31, 101, NULL,
                true, true, &fenced) == KMX_INPUT_READY && fenced == 30 &&
            session.accepted == UINT64_MAX, "terminal ledger resumes without reset");
    require(kmx_input_session_close(&session, 31, UINT64_MAX), "terminal ledger closes exactly");
}

static uint32_t
random_u32(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

/* The reference is a conceptual FIFO of accepted command IDs, plus a client
 * journal of generated IDs and its last delivered ACK. It never computes an
 * expected counter by reading the implementation's counter. Each ledger has
 * its own command list; retirement abandons its unresolved client journal. */
static void
test_randomized_schedules(void) {
    size_t total_admitted = 0, retries = 0, blocked = 0, takeovers = 0, retirements = 0;
    for (uint32_t seed = 1; seed <= 100; seed++) {
        kmx_input_session session, saved;
        uint64_t commands[2048], accepted_commands[2048];
        size_t generated = 0, count = 0;
        uint64_t ack = 0, now = 100, next_owner = 1, owner = 1, deadline = 0;
        uint64_t next_command = 0, epoch_generation = 0, fenced;
        uint32_t random = seed * UINT32_C(0x71b4e9d3);
        unsigned char epoch[KMX_INPUT_TOKEN_SIZE] = {2};
        kmx_input_open request = request_for(NULL, 19, 0);
        bool retained = true;
        require(kmx_input_session_init(&session, initial_epoch, 60) == KMX_OK &&
                kmx_input_session_open(&session, &request, owner, now, epoch,
                    true, false, &fenced) == KMX_INPUT_READY, "random schedule initial allocation");
        for (size_t step = 0; step < 2000; step++) {
            unsigned action = random_u32(&random) % 10;
            bool duplicate;
            if (action == 0) {
                require(generated < 2048, "generated command list bound");
                commands[generated++] = ++next_command;
            } else if (action == 1 && owner && ack < generated) {
                uint64_t sequence = ack + 1;
                require(kmx_input_session_classify(&session, owner, sequence, &duplicate) == KMX_OK,
                        "oldest journal command classifies");
                require(duplicate == (sequence <= count), "classification matches conceptual FIFO");
                if (duplicate) {
                    retries++;
                } else if (random_u32(&random) % 4 == 0) {
                    blocked++;
                    snapshot(&saved, &session);
                    require(session.accepted == count, "blocked FIFO admission keeps conceptual count");
                    unchanged(&session, &saved);
                } else {
                    require(sequence == count + 1, "conceptual FIFO has no gaps");
                    accepted_commands[count++] = commands[sequence - 1];
                    require(kmx_input_session_commit(&session, owner, sequence) == KMX_OK,
                            "commit command owned by conceptual FIFO");
                    total_admitted++;
                }
                if (random_u32(&random) % 3 == 0) ack = count; /* Otherwise lose ACK. */
            } else if (action == 2 && owner) {
                kmx_input_session_disconnect(&session, owner, now);
                owner = 0;
                deadline = now + 60;
            } else if (action == 3 && retained && (!owner ? now < deadline : true)) {
                uint64_t previous = owner;
                request = request_for(epoch, 19, ack);
                owner = ++next_owner;
                require(kmx_input_session_open(&session, &request, owner, now, NULL,
                            true, true, &fenced) == KMX_INPUT_READY && fenced == previous,
                        "random resume/takeover returns conceptual owner");
                if (previous) {
                    takeovers++;
                    snapshot(&saved, &session);
                    kmx_input_session_disconnect(&session, previous, now + 1);
                    unchanged(&session, &saved);
                    reject_sequence(&session, previous, count + 1);
                }
            } else if (action == 4) {
                now += random_u32(&random) % 31;
                kmx_input_session_expire(&session, now);
                if (!owner && retained && now >= deadline) retained = false;
            } else if (action == 5 && owner) {
                reject_sequence(&session, owner, count + 2);
            } else if (action == 6 && owner && ack == count) {
                require(kmx_input_session_close(&session, owner, ack), "random explicit release");
                owner = 0;
                retained = false;
            } else if (action == 7 && !retained) {
                kmx_input_open stale = request_for(epoch, 19, ack);
                refuse(&session, &stale, next_owner + 1, now, initial_epoch,
                        true, false, KMX_INPUT_EXPIRED);
                /* Rotate with a globally unique candidate for this schedule. */
                epoch_generation++;
                for (size_t i = 0; i < 8; i++) {
                    epoch[8 + i] = (unsigned char)(epoch_generation >> (56 - i * 8));
                }
                request = request_for(NULL, 19, 0);
                owner = ++next_owner;
                require(kmx_input_session_open(&session, &request, owner, now, epoch,
                            true, false, &fenced) == KMX_INPUT_READY && !fenced,
                        "random fresh allocation after retirement");
                refuse(&session, &stale, next_owner + 1, now, NULL,
                        true, false, KMX_INPUT_EPOCH);
                generated = count = 0;
                ack = 0;
                retained = true;
                retirements++;
            } else if (action == 8) {
                snapshot(&saved, &session);
                kmx_input_session_disconnect(&session, next_owner + 99, now);
                unchanged(&session, &saved);
            } else if (action == 9 && owner) {
                snapshot(&saved, &session);
                require(!kmx_input_session_close(&session, owner, count + 1),
                        "random unacknowledged release fails");
                unchanged(&session, &saved);
            }
            require(session.accepted == count && session.owner == owner && session.retained == retained,
                    "ledger follows conceptual FIFO and lifecycle");
            for (size_t i = 0; i < count; i++) {
                require(accepted_commands[i] == commands[i] &&
                        (!i || accepted_commands[i - 1] < accepted_commands[i]),
                        "accepted commands are exact ordered unique source prefix");
            }
        }
    }
    require(total_admitted > 1000 && retries > 1000 && blocked > 100 &&
            takeovers > 1000 && retirements > 1000, "random schedules cover every important transition");
}

int
main(void) {
    test_validation_and_open();
    test_lost_ack_fencing_and_admission();
    test_expiry_and_retired_identity();
    test_terminal_sequence();
    test_randomized_schedules();
    puts("input sessions: lease/dedup boundaries and 100 randomized 2000-step schedules passed");
    return 0;
}
