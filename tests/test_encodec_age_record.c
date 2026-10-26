/* Check the server's KMX_ENCODEC_EPOCH_AGE text for nonzero and largest values.
 * The actual report_epoch_start_ages() runs; only its accounting read is
 * replaced (-Wl,--wrap=kmx_encodec_epoch_start_ages). No worker or model runs. */
#define main unused_serve_main
#include "../tools/kmx_serve.c"
#undef main

#define CHECK(value, tag) do { if (!(value)) { fprintf(stderr, "%s line %d\n", tag, __LINE__); exit(1); } } while (0)
static kmx_encodec_epoch_ages forced;
kmx_encodec_epoch_ages __wrap_kmx_encodec_epoch_start_ages(const kmx_encodec *codec);
kmx_encodec_epoch_ages __wrap_kmx_encodec_epoch_start_ages(const kmx_encodec *codec) {
    (void)codec;
    return forced;
}

static size_t record(unsigned profile, char *text, size_t size) {
    FILE *captured = tmpfile();
    size_t length;
    int saved;
    CHECK(captured != NULL, "CAPTURE");
    fflush(stderr);
    saved = dup(2); CHECK(saved >= 0 && dup2(fileno(captured), 2) == 2, "CAPTURE");
    report_epoch_start_ages(NULL, profile);
    fflush(stderr); CHECK(dup2(saved, 2) == 2, "CAPTURE"); close(saved);
    rewind(captured); length = fread(text, 1, size - 1, captured); text[length] = 0; fclose(captured);
    return length;
}

int main(void) {
    char text[4096];
    static const char distinct[] =
        "KMX_ENCODEC_EPOCH_AGE v=1 profile=C5-R4 instance=1 delivered=1001 undelivered=2002 "
        "undelivered_at_start=3003 max_ms=4004 bucket_ms=10 buckets=100,101,102,103,104,105,106,"
        "107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125\n";
    char largest[4096];
    int used;
    /* Every field and bucket differs, so an argument swap or reordering shows. */
    forced = (kmx_encodec_epoch_ages){.delivered = 1001, .undelivered = 2002,
        .undelivered_at_start = 3003, .max_delivered_ms = 4004};
    for (unsigned i = 0; i < KMX_ENCODEC_AGE_BUCKETS; i++) forced.buckets[i] = 100u + i;
    record(1, text, sizeof text);
    CHECK(strcmp(text, distinct) == 0, "KILL-AGE-RECORD-FIELDS");
    record(0, text, sizeof text);
    CHECK(strncmp(text, "KMX_ENCODEC_EPOCH_AGE v=1 profile=C0 instance=1 delivered=1001 ", 63) == 0 &&
          strcmp(text + 63, distinct + 66) == 0, "KILL-AGE-RECORD-C0");
    /* The largest values: one whole record, never a silently omitted one. */
    forced.delivered = forced.undelivered = forced.undelivered_at_start = forced.max_delivered_ms = UINT64_MAX;
    for (unsigned i = 0; i < KMX_ENCODEC_AGE_BUCKETS; i++) forced.buckets[i] = UINT64_MAX;
    used = snprintf(largest, sizeof largest, "KMX_ENCODEC_EPOCH_AGE v=1 profile=C5-R4 instance=1 delivered=%s "
        "undelivered=%s undelivered_at_start=%s max_ms=%s bucket_ms=10 buckets=", "18446744073709551615",
        "18446744073709551615", "18446744073709551615", "18446744073709551615");
    for (unsigned i = 0; i < KMX_ENCODEC_AGE_BUCKETS; i++)
        used += snprintf(largest + used, sizeof largest - (size_t)used, "%s18446744073709551615", i ? "," : "");
    used += snprintf(largest + used, sizeof largest - (size_t)used, "\n");
    CHECK(used == 752, "AGE-RECORD-LARGEST-SIZE");
    CHECK(record(1, text, sizeof text) == 752 && strcmp(text, largest) == 0, "KILL-AGE-RECORD-LARGEST");
    printf("epoch-age record: fields, profiles and the 752-byte largest record PASS\n");
    return 0;
}
