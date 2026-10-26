/* Exercise the actual server's final-log function around in-flight work.
 * The unused server main never runs; no pane/socket/model/capture is opened. */
#define main unused_serve_main
#include "../tools/kmx_serve.c"
#undef main
#include "encodec_double.h"
#include <pthread.h>

#define CHECK(value, tag) do { if (!(value)) { fprintf(stderr, "%s line %d\n", tag, __LINE__); exit(1); } } while (0)
/* Capture the server's actual stderr records for the profile finals. */
static int saved_stderr = -1;
static FILE *captured;
static void capture_begin(void) {
    fflush(stderr); captured = tmpfile(); CHECK(captured != NULL, "CAPTURE");
    saved_stderr = dup(2); CHECK(saved_stderr >= 0 && dup2(fileno(captured), 2) == 2, "CAPTURE");
}
static void capture_end(char *text, size_t size) {
    size_t length;
    fflush(stderr); CHECK(dup2(saved_stderr, 2) == 2, "CAPTURE"); close(saved_stderr);
    rewind(captured); length = fread(text, 1, size - 1, captured); text[length] = 0; fclose(captured);
    fputs(text, stderr);
}
static unsigned occurrences(const char *text, const char *needle) {
    unsigned count = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) count++;
    return count;
}
static const char *line_after(const char *text, const char *prefix, const char *after) {
    const char *found = strstr(after ? after : text, prefix);
    return found && (found == text || found[-1] == '\n') ? found : NULL;
}
static void pause_ms(unsigned ms) { struct timespec delay = {0, (long)ms * 1000000L}; nanosleep(&delay, NULL); }
static void *release_native(void *unused) { (void)unused; pause_ms(30); kenc_double_release(); return NULL; }
int main(void) {
    bool reported[2] = {false, false};
    kmx_encodec *inactive = kmx_encodec_open_profile(true, 6, 24000, 1, NULL, "double-only", 4, 0);
    kmx_encodec *active = kmx_encodec_open_profile(true, 6, 24000, 1, NULL, "double-only", 4, 1);
    CHECK(inactive && active, "DOUBLE-OPEN");
    char text[8192];
    capture_begin();
    kmx_encodec_stats zero = finish_encodec_profile(inactive, 0, 0, &reported[0]);
    capture_end(text, sizeof text);
    CHECK(!reported[0] && zero.calls == 0, "KILL-INACTIVE-LOG");
    const char *end = line_after(text, "KMX_ENCODEC_PHASE v=1 event=end profile=C0 ", NULL);
    const char *age = line_after(text, "KMX_ENCODEC_EPOCH_AGE v=1 profile=C0 instance=1 delivered=0 undelivered=0 "
        "undelivered_at_start=0 max_ms=0 bucket_ms=10 buckets=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", end);
    const char *summary = line_after(text, "kmx-serve: encodec_profile ", age);
    CHECK(end && age && summary && occurrences(text, "KMX_ENCODEC_EPOCH_AGE") == 1, "KILL-AGE-LINE-INACTIVE");
    unsigned char pcm[1920] = {0};
    kenc_double_arm();
    CHECK(kmx_encodec_offer_pcm(active, pcm, sizeof pcm, 0), "OFFER");
    for (unsigned i = 0; i < 2000 && !kenc_double_entered(); i++) pause_ms(1);
    CHECK(kenc_double_entered(), "DOUBLE-GATE");
    pthread_t release;
    CHECK(pthread_create(&release, NULL, release_native, NULL) == 0, "THREAD");
    capture_begin();
    kmx_encodec_stats final = finish_encodec_profile(active, 1, 0, &reported[1]);
    capture_end(text, sizeof text);
    pthread_join(release, NULL);
    /* The joined RESET was published but is still queued at stop, so it is
     * not a sample; the record follows the quiescent end record. */
    end = line_after(text, "KMX_ENCODEC_PHASE v=1 event=end profile=C5-R4 ", NULL);
    age = line_after(text, "KMX_ENCODEC_EPOCH_AGE v=1 profile=C5-R4 instance=1 delivered=0 undelivered=0 "
        "undelivered_at_start=0 ", end);
    summary = line_after(text, "kmx-serve: encodec_profile ", age);
    CHECK(end && age && summary, "KILL-AGE-LINE-ACTIVE");
    CHECK(strchr(age, '\n') - age < 4096 && occurrences(text, "KMX_ENCODEC_EPOCH_AGE") == 1, "KILL-AGE-LINE-ACTIVE");
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
