#define _POSIX_C_SOURCE 200809L
#include "kmx_encodec.h"
#include <stdio.h>
#include <string.h>
#ifdef KMX_HAVE_ENCODEC
#include <kilix_encodec.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr, "age check line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
static atomic_bool armed, entered, released;

static uint64_t now(void) {
    struct timespec value;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}
static void until(uint64_t target) {
    while (now() < target) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
}
static void pause_ms(unsigned ms) { until(now() + (uint64_t)ms * 1000000u); }

/* Only this test call site adds a scheduling gate; the actual native encoder
 * runs after release. Its result and the candidate worker are not replaced. */
kenc_result __real_kenc_encoder_push_s16(kenc_encoder *, const int16_t *, size_t,
    uint64_t, uint8_t *, size_t, size_t *);
kenc_result __wrap_kenc_encoder_push_s16(kenc_encoder *encoder, const int16_t *pcm,
    size_t samples, uint64_t pts, uint8_t *packet, size_t capacity, size_t *written) {
    if (atomic_exchange(&armed, false)) {
        atomic_store(&entered, true);
        while (!atomic_load(&released)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    return __real_kenc_encoder_push_s16(encoder, pcm, samples, pts, packet, capacity, written);
}

static void output_ready(kmx_encodec *codec) {
    struct pollfd event = {kmx_encodec_event_fd(codec), POLLIN, 0};
    CHECK(poll(&event, 1, 1000) == 1 && (event.revents & POLLIN));
}
static void output(kmx_encodec *codec, uint64_t pts, unsigned flags) {
    kmx_encodec_output packet;
    output_ready(codec);
    CHECK(kmx_encodec_receive(codec, &packet));
    CHECK(packet.pts_ms == pts && packet.flags == flags);
}

static void total_age(const char *assets, int rate) {
    unsigned char pcm[3840] = {0};
    size_t bytes = (size_t)(rate / 25) * 2;
    kmx_encodec *codec = kmx_encodec_open(true, 6, rate, 1, NULL, assets);
    kmx_encodec_output packet;
    uint64_t began, published, received;
    bool accepted;
    CHECK(codec != NULL);
    atomic_store(&entered, false); atomic_store(&released, false); atomic_store(&armed, true);
    began = now();
    CHECK(kmx_encodec_offer_pcm(codec, pcm, bytes, 0));
    if (rate != 24000) {
        /* The first conversion leaves a partial 960-sample accumulator. */
        pause_ms(80);
        CHECK(kmx_encodec_offer_pcm(codec, pcm, bytes, 40));
    }
    while (!atomic_load(&entered)) { CHECK(now() - began < 1000000000u); pause_ms(1); }
    pause_ms(rate == 24000 ? 130 : 90);
    atomic_store(&released, true);
    output_ready(codec); published = now();
    /* Establish two individually timely phases whose total is stale. */
    CHECK(published - began < 250000000u);
    until(began + 280000000u); received = now();
    CHECK(received - published < 250000000u);
    accepted = kmx_encodec_receive(codec, &packet);
    printf("age rate=%d acquire_to_publish_ns=%llu queue_ns=%llu total_ns=%llu accepted=%d\n", rate,
        (unsigned long long)(published - began), (unsigned long long)(received - published),
        (unsigned long long)(received - began), accepted);
    CHECK(!accepted);
    CHECK(kmx_encodec_statistics(codec).output_drops >= 1);
    CHECK(kmx_encodec_offer_pcm(codec, pcm, bytes, 1000));
    if (rate != 24000) CHECK(kmx_encodec_offer_pcm(codec, pcm, bytes, 1040));
    output(codec, 1000, 5);
    CHECK(kmx_encodec_offer_pcm(codec, pcm, bytes, rate == 24000 ? 1040 : 1080));
    output(codec, 1040, 0);
    kmx_encodec_close(codec);
}

static void invalidate_next_epoch(const char *assets, bool in_flight) {
    unsigned char pcm[1920] = {0};
    kmx_encodec *codec = kmx_encodec_open(true, 6, 24000, 1, NULL, assets);
    kmx_encodec_output packet;
    uint64_t began, second;
    unsigned i;
    bool accepted;
    CHECK(codec != NULL);
    for (i = 0; i < 24; i++) {
        CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, i * 40u));
        output(codec, i * 40u, i == 0 ? 5 : 0);
    }
    began = now();
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 960));
    output_ready(codec);
    pause_ms(180);
    second = now();
    if (in_flight) {
        atomic_store(&entered, false); atomic_store(&released, false); atomic_store(&armed, true);
    }
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 1000));
    while (in_flight ? !atomic_load(&entered) : kmx_encodec_statistics(codec).calls != 26) {
        CHECK(now() - second < 1000000000u); pause_ms(1);
    }
    until(began + 280000000u);
    CHECK(now() - second < 250000000u);
    /* The 960 output is stale; the queued/in-flight 1000 output is timely but
     * predates that global abandonment and lacks DISCONTINUITY. Drop both. */
    accepted = kmx_encodec_receive(codec, &packet);
    CHECK(!in_flight || !accepted);
    if (in_flight) {
        atomic_store(&released, true);
        output_ready(codec);
        CHECK(now() - second < 250000000u);
        accepted = kmx_encodec_receive(codec, &packet);
    }
    printf("%s next-epoch accepted=%d pts=%llu flags=%u\n", in_flight ? "in-flight" : "queued", accepted,
        (unsigned long long)(accepted ? packet.pts_ms : 0), accepted ? packet.flags : 0);
    CHECK(!accepted);
    CHECK(kmx_encodec_statistics(codec).output_drops >= 2);
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 2000));
    output(codec, 2000, 5);
    CHECK(kmx_encodec_offer_pcm(codec, pcm, sizeof pcm, 2040));
    output(codec, 2040, 0);
    kmx_encodec_close(codec);
    printf("%s next-epoch invalidation and recovery PASS\n", in_flight ? "in-flight" : "queued");
}

int main(int argc, char **argv) {
    const char *mode;
    if (argc != 2 && argc != 3) return 2;
    mode = argc == 2 ? "all" : argv[2];
    if (!strcmp(mode, "all") || !strcmp(mode, "24000")) total_age(argv[1], 24000);
    if (!strcmp(mode, "all") || !strcmp(mode, "44100")) total_age(argv[1], 44100);
    if (!strcmp(mode, "all") || !strcmp(mode, "48000")) total_age(argv[1], 48000);
    if (!strcmp(mode, "all") || !strcmp(mode, "epoch")) invalidate_next_epoch(argv[1], false);
    if (!strcmp(mode, "all") || !strcmp(mode, "inflight")) invalidate_next_epoch(argv[1], true);
    if (!checks) return 2;
    printf("encodec age: %u checks PASS (injected scheduling; no performance qualification)\n", checks);
    return 0;
}
#else
int main(void) { fputs("requires ENCODEC=1 and explicit development model input\n", stderr); return 77; }
#endif
