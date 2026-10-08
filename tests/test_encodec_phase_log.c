/* Exercise the actual server's final-log function around in-flight work.
 * The unused server main never runs; no pane/socket/model/capture is opened. */
#define main unused_serve_main
#include "../tools/kmx_serve.c"
#undef main
#include "encodec_double.h"
#include <pthread.h>

#define CHECK(value, tag) do { if (!(value)) { fprintf(stderr, "%s line %d\n", tag, __LINE__); exit(1); } } while (0)
static void pause_ms(unsigned ms) { struct timespec delay = {0, (long)ms * 1000000L}; nanosleep(&delay, NULL); }
static void *release_native(void *unused) { (void)unused; pause_ms(30); kenc_double_release(); return NULL; }
int main(void) {
    bool reported[2] = {false, false};
    kmx_encodec *inactive = kmx_encodec_open_profile(true, 6, 24000, 1, NULL, "double-only", 4, 0);
    kmx_encodec *active = kmx_encodec_open_profile(true, 6, 24000, 1, NULL, "double-only", 4, 1);
    CHECK(inactive && active, "DOUBLE-OPEN");
    kmx_encodec_stats zero = finish_encodec_profile(inactive, 0, 0, &reported[0]);
    CHECK(!reported[0] && zero.calls == 0, "KILL-INACTIVE-LOG");
    unsigned char pcm[1920] = {0};
    kenc_double_arm();
    CHECK(kmx_encodec_offer_pcm(active, pcm, sizeof pcm, 0), "OFFER");
    for (unsigned i = 0; i < 2000 && !kenc_double_entered(); i++) pause_ms(1);
    CHECK(kenc_double_entered(), "DOUBLE-GATE");
    pthread_t release;
    CHECK(pthread_create(&release, NULL, release_native, NULL) == 0, "THREAD");
    kmx_encodec_stats final = finish_encodec_profile(active, 1, 0, &reported[1]);
    pthread_join(release, NULL);
    CHECK(reported[1] && final.calls == 1 && final.discontinuities == 1, "KILL-SERVER-FINAL-JOIN");
    CHECK(final.input_drops == 0 && final.output_drops == 0, "KILL-SERVER-FINAL-COUNTS");
    /* Same aggregate printf as the server, with the actual returned finals. */
    fprintf(stderr, "kmx-serve: encodec calls=%llu rtf=%.6f input_drops=%llu output_drops=%llu discontinuities=%llu client_drops=0\n",
        (unsigned long long)final.calls, (double)final.inference_ns / ((double)final.calls * 40000000.0),
        (unsigned long long)final.input_drops, (unsigned long long)final.output_drops,
        (unsigned long long)final.discontinuities);
    kmx_encodec_close(inactive); kmx_encodec_close(active);
    puts("server phase log: PASS (native ABI double; no server main or qualification)");
    return 0;
}
