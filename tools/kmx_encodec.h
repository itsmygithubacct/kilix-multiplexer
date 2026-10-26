#ifndef KMX_ENCODEC_H
#define KMX_ENCODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A separate post-HELLO exchange preserves the legacy authentication bytes.
 * No peer is sent KMA2 without a compatible offer and an explicit selection. */
#define KMX_AUDIO_CAPS_BYTES 12u
#define KMX_AUDIO_PROFILE_BYTES 12u
#define KMX_AUDIO_PROFILE_C0 0u
#define KMX_AUDIO_PROFILE_C5_R4 1u
#define KMX_AUDIO_PROFILE_BIT(profile) (UINT32_C(1) << (profile))
typedef struct {
    uint8_t kind; /* 0 capability offer, 1 selected marker */
    uint32_t value;
} kmx_audio_profile;
void kmx_audio_profile_write(unsigned char out[KMX_AUDIO_PROFILE_BYTES], const kmx_audio_profile *profile);
int kmx_audio_profile_read(kmx_audio_profile *out, const void *data, size_t size);
uint32_t kmx_audio_profile_choose(uint32_t local, uint32_t peer);
#define KMX_AUDIO_CODEC_PCM 1u
#define KMX_AUDIO_CODEC_ENCODEC 2u
#define KMX_ENCODEC_PACKET_MAX 160u
#define KMX_ENCODEC_SAMPLES 960u
#define KMX_ENCODEC_STALE_MS 250u

typedef enum { KMX_AUDIO_AUTO, KMX_AUDIO_PCM, KMX_AUDIO_ENCODEC } kmx_audio_mode;
typedef struct {
    uint8_t kind; /* 0 offer, 1 selection, 2 refusal */
    uint8_t codecs;
    uint8_t rates; /* 3/6/12 kb/s use bits 0/1/2 */
    uint16_t maximum;
} kmx_audio_caps;

int kmx_audio_mode_parse(const char *text, kmx_audio_mode *mode);
int kmx_audio_bitrate_parse(const char *text, unsigned *bitrate);
int kmx_audio_threads_parse(const char *text, unsigned *threads);
uint8_t kmx_audio_rate_bit(unsigned bitrate);
void kmx_audio_caps_write(unsigned char out[KMX_AUDIO_CAPS_BYTES], const kmx_audio_caps *caps);
int kmx_audio_caps_read(kmx_audio_caps *out, const void *data, size_t size);
/* Unknown offer bits are ignored. The selected codec/rate must be known. */
kmx_audio_caps kmx_audio_choose(const kmx_audio_caps *offer, bool ready, unsigned bitrate, kmx_audio_mode mode);

typedef struct kmx_encodec kmx_encodec;
typedef struct {
    unsigned char packet[KMX_ENCODEC_PACKET_MAX];
    size_t size;
    int16_t pcm[KMX_ENCODEC_SAMPLES];
    uint64_t pts_ms;
    uint64_t epoch;
    uint64_t created_ms;
    uint8_t flags;
} kmx_encodec_output;
typedef struct {
    uint64_t calls, inference_ns, input_drops, output_drops, discontinuities;
} kmx_encodec_stats;
typedef struct {
    uint64_t monotonic_ns, pts_ms, epoch;
    uint8_t flags;
    kmx_encodec_stats before;
} kmx_encodec_boundary;
/* Encoder-only epoch-start ages. Each RESET packet that finishes encoding is
 * one sample. A packet that kmx_encodec_receive hands to the scheduler is
 * counted in the 10ms bucket of its oldest capture's age at that hand-off; the
 * last bucket holds exactly 250ms. A packet never handed over (output ring full,
 * stale or invalidated) is undelivered and lies beyond the budget. Packets still
 * queued when the worker stops are not samples. undelivered_at_start is the
 * undelivered count captured with the first publication boundary; nothing can
 * be delivered before that boundary. */
#define KMX_ENCODEC_AGE_BUCKET_MS 10u
#define KMX_ENCODEC_AGE_BUCKETS (KMX_ENCODEC_STALE_MS / KMX_ENCODEC_AGE_BUCKET_MS + 1u)
typedef struct {
    uint64_t delivered, undelivered, undelivered_at_start, max_delivered_ms;
    uint64_t buckets[KMX_ENCODEC_AGE_BUCKETS];
} kmx_encodec_epoch_ages;

/* All admission, graph creation and warm-up complete before success. The
 * normal path admits fresh installed assets via native's embedded F100 API.
 * development_assets is an explicit oracle/test-only path, never an automatic
 * fallback. Decoder capture rate/channels are ignored. Both use two ORT threads.
 * After opening, only the owned worker invokes inference/resampling. */
kmx_encodec *kmx_encodec_open(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets);
/* Explicit CPU allocation for the mono codec, including its inference caller.
 * The legacy open function retains two threads. This accepts only two or four. */
kmx_encodec *kmx_encodec_open_with_threads(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets, unsigned threads);
/* Immutable profile selection precedes warm-up and worker creation. The two
 * older open functions keep C0. Only markers 0 and 1 are accepted. */
kmx_encodec *kmx_encodec_open_profile(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets,
    unsigned threads, uint32_t profile);
void kmx_encodec_close(kmx_encodec *codec);
/* Scheduler teardown: stop and join the worker before its final statistics.
 * Idempotent; like close/offer/receive, called only by the owning scheduler. */
void kmx_encodec_stop(kmx_encodec *codec);
int kmx_encodec_event_fd(const kmx_encodec *codec);
/* Each encoder offer is exactly 40 ms of interleaved little-endian PCM16.
 * The fixed queue accepts six frames, 240ms of capture, within the 250ms age budget,
 * including C5-R4 reset work. Offers never wait for inference.
 * Overflow/staleness resumes at the next one-second source boundary. */
bool kmx_encodec_offer_pcm(kmx_encodec *codec, const void *pcm, size_t bytes, uint64_t pts_ms);
bool kmx_encodec_offer_packet(kmx_encodec *codec, const void *packet, size_t bytes);
/* Single-producer decoder backpressure; retains the same two input slots.
 * Deferred transport data keeps its original monotonic receipt timestamp. */
bool kmx_encodec_packet_ready(const kmx_encodec *codec);
bool kmx_encodec_offer_packet_at(kmx_encodec *codec, const void *packet, size_t bytes, uint64_t received_ms);
bool kmx_encodec_receive(kmx_encodec *codec, kmx_encodec_output *output);
/* A reconnected decoder has no continuity claim about the new connection. */
void kmx_encodec_restart(kmx_encodec *codec);
kmx_encodec_stats kmx_encodec_statistics(const kmx_encodec *codec);
/* Encoder-only, immutable until close. The first packet publication captures
 * all counters immediately BEFORE releasing the output ring's first entry.
 * Counter updates and capture use one short mutex, never held for inference.
 * Later drops, including stale/invalidated delivery of that entry, are after
 * the boundary. Peer reconnects and idle periods never create a new boundary. */
bool kmx_encodec_stream_boundary(const kmx_encodec *codec, kmx_encodec_boundary *boundary);
/* A decoder, or an encoder before any RESET packet, reports all zeros. */
kmx_encodec_epoch_ages kmx_encodec_epoch_start_ages(const kmx_encodec *codec);

#endif
