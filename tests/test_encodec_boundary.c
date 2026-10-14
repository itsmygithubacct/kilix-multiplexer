/* Real producer, rings and worker threads with a model-free native ABI double.
 * Including the translation unit permits deterministic accounting-lock tests. */
#include "../tools/kmx_encodec.c"
#include "encodec_double.h"

static unsigned checks;
#define CHECK(value, tag) do { checks++; if (!(value)) { fprintf(stderr, "%s line %d: %s\n", tag, __LINE__, #value); exit(1); } } while (0)
static void pause_ms(unsigned ms) { struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L}; nanosleep(&delay, NULL); }
static void await_output(kmx_encodec *codec) {
    struct pollfd event = {kmx_encodec_event_fd(codec), POLLIN, 0};
    CHECK(poll(&event, 1, 2000) == 1, "KILL-PUBLISH");
}
static kmx_encodec *opened(unsigned profile) {
    kmx_encodec *codec = kmx_encodec_open_profile(true, 6, 24000, 1, NULL, "double-only", 4, profile);
    CHECK(codec != NULL, "DOUBLE-OPEN"); return codec;
}
static void first_boundary(void) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *codec = opened(1);
    kmx_encodec_output output;
    kmx_encodec_boundary first, again;
    CHECK(!kmx_encodec_stream_boundary(codec, &first), "KILL-NO-EARLY-BOUNDARY");
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 40), "OFFER");
    for (unsigned i = 0; i < 2000 && !kmx_encodec_statistics(codec).input_drops; i++) pause_ms(1);
    CHECK(kmx_encodec_statistics(codec).input_drops == 1, "STARTUP-DROP");
    CHECK(!kmx_encodec_stream_boundary(codec, &first), "KILL-NO-EARLY-BOUNDARY");
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 1000), "OFFER");
    await_output(codec);
    /* No receive yet: publication itself owns the snapshot. */
    CHECK(kmx_encodec_stream_boundary(codec, &first), "KILL-PUBLISH");
    CHECK(first.before.input_drops == 1 && first.before.output_drops == 0 && first.before.discontinuities == 1,
          "KILL-EXACT-STARTUP");
    CHECK(first.before.calls == 1 && first.before.inference_ns > 0 && first.monotonic_ns > 0,
          "KILL-COHERENT-INFERENCE");
    CHECK(first.pts_ms == 1000 && first.epoch > 0 && first.flags == 13, "KILL-BOUNDARY-PACKET");
    pause_ms(280);
    CHECK(!kmx_encodec_receive(codec, &output), "STALE");
    CHECK(kmx_encodec_statistics(codec).output_drops - first.before.output_drops == 1, "KILL-POST-STALE");
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 2000), "OFFER");
    await_output(codec); CHECK(kmx_encodec_receive(codec, &output), "RECEIVE");
    CHECK(kmx_encodec_statistics(codec).discontinuities - first.before.discontinuities == 1, "KILL-POST-RESET");
    kmx_encodec_restart(codec); /* Encoder reconnect never erases its history. */
    CHECK(kmx_encodec_stream_boundary(codec, &again), "KILL-IMMUTABLE");
    CHECK(memcmp(&first, &again, sizeof first) == 0, "KILL-IMMUTABLE");
    kmx_encodec_stop(codec);
    kmx_encodec_stats final = kmx_encodec_statistics(codec);
    pause_ms(5); kmx_encodec_stop(codec);
    kmx_encodec_stats stable = kmx_encodec_statistics(codec);
    CHECK(memcmp(&final, &stable, sizeof final) == 0 && !codec->worker_running, "KILL-FINAL-QUIESCENT");
    kmx_encodec_close(codec);
}
static void profiles(void) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *a = opened(0), *b = opened(1);
    kmx_encodec_boundary ca, cb;
    kmx_encodec_output out;
    CHECK(kmx_encodec_offer_pcm(a, pcm, sizeof pcm, 0), "OFFER");
    CHECK(kmx_encodec_offer_pcm(b, pcm, sizeof pcm, 0), "OFFER");
    await_output(a); await_output(b);
    CHECK(kmx_encodec_stream_boundary(a, &ca) && kmx_encodec_stream_boundary(b, &cb), "KILL-PROFILES");
    CHECK(ca.flags == 5 && cb.flags == 13 && ca.before.discontinuities == 1 && cb.before.discontinuities == 1, "KILL-PROFILES");
    CHECK(kmx_encodec_receive(a, &out) && kmx_encodec_receive(b, &out), "RECEIVE");
    CHECK(kmx_encodec_offer_pcm(b, pcm, sizeof pcm, 40), "OFFER");
    await_output(b); CHECK(kmx_encodec_receive(b, &out), "RECEIVE");
    CHECK(kmx_encodec_statistics(b).discontinuities == cb.before.discontinuities, "KILL-HEALTHY-TWIN");
    CHECK(kmx_encodec_statistics(a).calls == ca.before.calls, "KILL-PROFILES");
    kmx_encodec_close(a); kmx_encodec_close(b);
}
static atomic_bool operation_done;
static void *add_drop(void *codec) { note_input_drop(codec); atomic_store(&operation_done, true); return NULL; }
static void *first_publish(void *opaque) {
    kmx_encodec *codec = opaque;
    kmx_encodec_output output = {.pts_ms = 0, .flags = 13, .epoch = 1, .created_ms = milliseconds()};
    (void)publish(codec, &output, generation_now(codec));
    atomic_store(&operation_done, true); return NULL;
}
static void serialized(void) {
    kmx_encodec *codec = opened(1);
    pthread_t thread;
    pthread_mutex_lock(&codec->statistics_lock);
    atomic_store(&operation_done, false);
    CHECK(pthread_create(&thread, NULL, add_drop, codec) == 0, "THREAD");
    pause_ms(20);
    CHECK(!atomic_load(&operation_done) && codec->statistics.input_drops == 0, "KILL-ACCOUNTING-LOCK");
    pthread_mutex_unlock(&codec->statistics_lock); pthread_join(thread, NULL);
    pthread_mutex_lock(&codec->statistics_lock);
    atomic_store(&operation_done, false);
    CHECK(pthread_create(&thread, NULL, first_publish, codec) == 0, "THREAD");
    pause_ms(20);
    CHECK(!atomic_load(&operation_done) && !codec->stream_started && atomic_load(&codec->out_head) == 0,
          "KILL-PUBLICATION-LOCK");
    pthread_mutex_unlock(&codec->statistics_lock); pthread_join(thread, NULL);
    kmx_encodec_boundary boundary;
    CHECK(kmx_encodec_stream_boundary(codec, &boundary) && boundary.before.input_drops == 1, "KILL-EXACT-STARTUP");
    kmx_encodec_close(codec);
}
static void *stop_worker(void *codec) { kmx_encodec_stop(codec); atomic_store(&operation_done, true); return NULL; }
static void in_flight_stop(void) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *codec = opened(1);
    pthread_t thread;
    kenc_double_arm();
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 0), "OFFER");
    for (unsigned i = 0; i < 2000 && !kenc_double_entered(); i++) pause_ms(1);
    CHECK(kenc_double_entered(), "DOUBLE-GATE");
    CHECK(pthread_mutex_trylock(&codec->statistics_lock) == 0, "KILL-NO-INFERENCE-LOCK");
    pthread_mutex_unlock(&codec->statistics_lock);
    atomic_store(&operation_done, false);
    CHECK(pthread_create(&thread, NULL, stop_worker, codec) == 0, "THREAD");
    pause_ms(20);
    CHECK(!atomic_load(&operation_done), "KILL-JOIN-INFLIGHT");
    kenc_double_release(); pthread_join(thread, NULL);
    CHECK(kmx_encodec_statistics(codec).calls == 1 && !codec->worker_running, "KILL-FINAL-QUIESCENT");
    kmx_encodec_close(codec);
}

static void timely_capture_during_reset(void) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *codec = opened(1);
    kmx_encodec_output output;
    uint64_t began;
    kenc_double_arm();
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 0), "RESET-OFFER");
    for (unsigned i = 0; i < 2000 && !kenc_double_entered(); i++) pause_ms(1);
    CHECK(kenc_double_entered(), "RESET-GATE");
    began = milliseconds();
    /* The retained native first reset took 166ms. Hold this ABI call for
     * 140ms while ordinary 40ms capture continues: every offered frame is
     * timely, but the old two-slot queue refused the third one at 120ms and
     * invalidated the entire epoch. The worker/rings/age checks are real. */
    for (unsigned i = 1; i <= 3; i++) {
        while (milliseconds() - began < i * 40u) pause_ms(1);
        CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, i * 40u), "RESET-TIMELY-CAPTURE");
    }
    while (milliseconds() - began < 140u) pause_ms(1);
    kenc_double_release();
    for (unsigned i = 0; i <= 3; i++) {
        if (!kmx_encodec_receive(codec, &output)) {
            await_output(codec);
            CHECK(kmx_encodec_receive(codec, &output), "RESET-TIMELY-OUTPUT");
        }
        CHECK(output.pts_ms == i * 40u && output.flags == (i ? 0u : 13u), "RESET-ORDER");
    }
    kmx_encodec_stats stats = kmx_encodec_statistics(codec);
    CHECK(stats.calls == 4 && stats.input_drops == 0 && stats.output_drops == 0 &&
          stats.discontinuities == 1, "RESET-NO-DROPS");
    kmx_encodec_close(codec);
}

static void queued_capture_at_counter_wrap(void) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *codec = opened(1);
    kmx_encodec_output output;
    /* Seed the empty ring while its worker is stopped, then exercise the
     * actual producer/consumer through counter wrap with a full input queue. */
    kmx_encodec_stop(codec);
    atomic_store(&codec->in_head, UINT64_MAX - 2u);
    atomic_store(&codec->in_tail, UINT64_MAX - 2u);
    atomic_store(&codec->stop, false);
    CHECK(pthread_create(&codec->thread, NULL, worker, codec) == 0, "WRAP-WORKER");
    codec->worker_running = true;
    kenc_double_arm();
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 0), "WRAP-FIRST-OFFER");
    for (unsigned i = 0; i < 2000 && !kenc_double_entered(); i++) pause_ms(1);
    CHECK(kenc_double_entered(), "WRAP-GATE");
    for (unsigned i = 1; i <= 6; i++)
        CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, i * 40u), "WRAP-OFFER");
    kenc_double_release();
    for (unsigned i = 0; i <= 6; i++) {
        if (!kmx_encodec_receive(codec, &output)) {
            await_output(codec);
            CHECK(kmx_encodec_receive(codec, &output), "WRAP-OUTPUT");
        }
        CHECK(output.pts_ms == i * 40u && output.flags == (i ? 0u : 13u), "WRAP-ORDER");
    }
    kmx_encodec_stats stats = kmx_encodec_statistics(codec);
    CHECK(stats.calls == 7 && stats.input_drops == 0 && stats.output_drops == 0,
          "WRAP-NO-DROPS");
    kmx_encodec_close(codec);
}

int main(void) {
    first_boundary(); profiles(); serialized(); in_flight_stop();
    timely_capture_during_reset(); queued_capture_at_counter_wrap();
    printf("publication boundary: %u checks PASS (native ABI double; no qualification)\n", checks);
    return 0;
}
