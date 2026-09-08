#ifndef KMX_ENCODEC_H
#define KMX_ENCODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A separate post-HELLO exchange preserves the legacy authentication bytes.
 * No peer is sent KMA2 without a compatible offer and an explicit selection. */
#define KMX_AUDIO_CAPS_BYTES 12u
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

/* All admission, graph creation and warm-up complete before success. The
 * normal path admits fresh installed assets via native's embedded F100 API.
 * development_assets is an explicit oracle/test-only path, never an automatic
 * fallback. Decoder capture rate/channels are ignored. Both use two ORT threads.
 * After opening, only the owned worker invokes inference/resampling. */
kmx_encodec *kmx_encodec_open(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets);
void kmx_encodec_close(kmx_encodec *codec);
int kmx_encodec_event_fd(const kmx_encodec *codec);
/* Each encoder offer is exactly 40 ms of interleaved little-endian PCM16.
 * There are exactly two fixed input slots. Offers never wait for inference.
 * Overflow/staleness resumes at the next one-second source boundary. */
bool kmx_encodec_offer_pcm(kmx_encodec *codec, const void *pcm, size_t bytes, uint64_t pts_ms);
bool kmx_encodec_offer_packet(kmx_encodec *codec, const void *packet, size_t bytes);
bool kmx_encodec_receive(kmx_encodec *codec, kmx_encodec_output *output);
/* A reconnected decoder has no continuity claim about the new connection. */
void kmx_encodec_restart(kmx_encodec *codec);
kmx_encodec_stats kmx_encodec_statistics(const kmx_encodec *codec);

#endif
