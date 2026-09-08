#define _GNU_SOURCE
#include "kmx_encodec.h"

#include <string.h>

int kmx_audio_mode_parse(const char *text, kmx_audio_mode *mode) {
    if (!text || !mode) return -1;
    if (strcmp(text, "auto") == 0) *mode = KMX_AUDIO_AUTO;
    else if (strcmp(text, "pcm") == 0) *mode = KMX_AUDIO_PCM;
    else if (strcmp(text, "encodec") == 0) *mode = KMX_AUDIO_ENCODEC;
    else return -1;
    return 0;
}

int kmx_audio_bitrate_parse(const char *text, unsigned *bitrate) {
    if (!text || !bitrate) return -1;
    if (strcmp(text, "3") == 0) *bitrate = 3;
    else if (strcmp(text, "6") == 0) *bitrate = 6;
    else if (strcmp(text, "12") == 0) *bitrate = 12;
    else return -1;
    return 0;
}

uint8_t kmx_audio_rate_bit(unsigned bitrate) {
    return bitrate == 3 ? 1 : bitrate == 6 ? 2 : bitrate == 12 ? 4 : 0;
}

void kmx_audio_caps_write(unsigned char out[KMX_AUDIO_CAPS_BYTES], const kmx_audio_caps *caps) {
    memcpy(out, "KAC1", 4);
    out[4] = caps->kind; out[5] = caps->codecs; out[6] = caps->rates; out[7] = 0;
    out[8] = (unsigned char)(caps->maximum >> 8); out[9] = (unsigned char)caps->maximum;
    out[10] = out[11] = 0;
}

int kmx_audio_caps_read(kmx_audio_caps *out, const void *data, size_t size) {
    const unsigned char *p = data;
    kmx_audio_caps value;
    if (!out || !p || size != KMX_AUDIO_CAPS_BYTES || memcmp(p, "KAC1", 4) ||
        p[4] > 2 || p[7] || p[10] || p[11]) return -1;
    value.kind = p[4]; value.codecs = p[5]; value.rates = p[6];
    value.maximum = (uint16_t)((p[8] << 8) | p[9]);
    if ((value.kind == 1 && !((value.codecs == KMX_AUDIO_CODEC_PCM && value.rates == 0) ||
            (value.codecs == KMX_AUDIO_CODEC_ENCODEC && (value.rates == 1 || value.rates == 2 || value.rates == 4) &&
             value.maximum >= KMX_ENCODEC_PACKET_MAX))) ||
        (value.kind == 2 && (value.codecs || value.rates || value.maximum))) return -1;
    *out = value;
    return 0;
}

kmx_audio_caps kmx_audio_choose(const kmx_audio_caps *offer, bool ready, unsigned bitrate, kmx_audio_mode mode) {
    kmx_audio_caps chosen = {.kind = 2};
    if (!offer || offer->kind != 0) return chosen;
    if (mode != KMX_AUDIO_PCM && ready && (offer->codecs & KMX_AUDIO_CODEC_ENCODEC) &&
        (offer->rates & kmx_audio_rate_bit(bitrate)) && offer->maximum >= KMX_ENCODEC_PACKET_MAX) {
        chosen = (kmx_audio_caps){1, KMX_AUDIO_CODEC_ENCODEC, kmx_audio_rate_bit(bitrate), KMX_ENCODEC_PACKET_MAX};
    } else if (mode != KMX_AUDIO_ENCODEC && (offer->codecs & KMX_AUDIO_CODEC_PCM)) {
        chosen = (kmx_audio_caps){1, KMX_AUDIO_CODEC_PCM, 0, 0};
    }
    return chosen;
}

#ifdef KMX_HAVE_ENCODEC
#include <kilix_encodec.h>
#include <kilix_encodec_content.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <samplerate.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_MAX_FRAMES 1920u
#define CAPTURE_MAX_BYTES (CAPTURE_MAX_FRAMES * 8u * 2u)
#define SLOTS 2u

typedef struct {
    unsigned char bytes[CAPTURE_MAX_BYTES];
    size_t size;
    uint64_t pts_ms, offered_ms, generation;
} codec_input;
typedef struct {
    kmx_encodec_output value;
    uint64_t generation;
} codec_output;

struct kmx_encodec {
    bool encode;
    int rate, channels;
    kenc_options options;
    kenc_encoder *encoder;
    kenc_decoder *decoder;
    SRC_STATE *resampler;
    pthread_t thread;
    int input_event, output_event;
    atomic_bool stop;
    atomic_uint_fast64_t in_head, in_tail, out_head, out_tail;
    atomic_uint_fast64_t generation, broken_until;
    atomic_uint_fast64_t calls, inference_ns, input_drops, output_drops, discontinuities;
    codec_input input[SLOTS];
    codec_output output[SLOTS];
};

static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static uint64_t milliseconds(void) { return nanoseconds() / UINT64_C(1000000); }
static void notify(int fd) {
    uint64_t value = 1;
    ssize_t result;
    do { result = write(fd, &value, sizeof value); } while (result < 0 && errno == EINTR);
}
static void drain_event(int fd) {
    uint64_t value;
    while (read(fd, &value, sizeof value) == (ssize_t)sizeof value) {}
}

static uint64_t generation_now(const kmx_encodec *codec) {
    /* The encoder's monotonic broken boundary also invalidates queued and
     * in-flight work atomically. A decoder has a separate reconnect counter. */
    return codec->encode ? atomic_load(&codec->broken_until) : atomic_load(&codec->generation);
}

static void break_epoch(kmx_encodec *codec, uint64_t pts_ms) {
    uint64_t boundary = pts_ms > UINT64_MAX - 1000u ? UINT64_MAX : (pts_ms / 1000u + 1u) * 1000u;
    uint64_t previous = atomic_load(&codec->broken_until);
    while (previous < boundary && !atomic_compare_exchange_weak(&codec->broken_until, &previous, boundary)) {}
}

static bool publish(kmx_encodec *codec, const kmx_encodec_output *output, uint64_t generation) {
    uint64_t head = atomic_load_explicit(&codec->out_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&codec->out_tail, memory_order_acquire) >= SLOTS) {
        atomic_fetch_add(&codec->output_drops, 1);
        if (codec->encode) break_epoch(codec, output->pts_ms);
        return false;
    }
    codec->output[head % SLOTS] = (codec_output){.value = *output, .generation = generation};
    atomic_store_explicit(&codec->out_head, head + 1, memory_order_release);
    notify(codec->output_event);
    return true;
}

/* Return mono floats with arithmetic channel averaging. Each channel's
 * contribution is scaled before addition, so full-scale inputs cannot wrap.
 * The resampler is continuous across ordinary codec epochs. */
static long resample(kmx_encodec *codec, const codec_input *input, float *out) {
    float mono[CAPTURE_MAX_FRAMES];
    size_t frames = input->size / ((size_t)codec->channels * 2u);
    size_t frame;
    for (frame = 0; frame < frames; frame++) {
        float value = 0;
        int channel;
        for (channel = 0; channel < codec->channels; channel++) {
            size_t at = (frame * (size_t)codec->channels + (size_t)channel) * 2u;
            uint16_t word = (uint16_t)(input->bytes[at] | ((unsigned)input->bytes[at + 1] << 8));
            int sample = word >= 32768u ? (int)word - 65536 : (int)word;
            value += (float)sample / (32768.0f * (float)codec->channels);
        }
        mono[frame] = value;
    }
    if (codec->rate == 24000) {
        memcpy(out, mono, frames * sizeof(float));
        return (long)frames;
    }
    {
        SRC_DATA data = {.data_in = mono, .data_out = out, .input_frames = (long)frames,
                         .output_frames = CAPTURE_MAX_FRAMES, .src_ratio = 24000.0 / codec->rate};
        if (src_process(codec->resampler, &data) || data.input_frames_used != (long)frames) return -1;
        return data.output_frames_gen;
    }
}

static void *worker(void *opaque) {
    kmx_encodec *codec = opaque;
    uint64_t generation = generation_now(codec), next_capture = 0, next_pts = 0;
    uint64_t accumulated_offered_ms = 0;
    bool started = false, waiting = true;
    int16_t accumulated[KMX_ENCODEC_SAMPLES];
    size_t used = 0;
    while (!atomic_load(&codec->stop)) {
        uint64_t tail = atomic_load_explicit(&codec->in_tail, memory_order_relaxed);
        codec_input input;
        uint64_t current_generation = generation_now(codec);
        if (generation != current_generation) {
            generation = current_generation;
            if (codec->decoder) kenc_decoder_reset(codec->decoder);
            else { started = false; used = 0; }
            waiting = true;
        }
        /* Output capacity never expands. Stop consuming input until the
         * scheduler drains it; a continuing capture source then hits the
         * existing two-input overflow rule, which owns epoch recovery. */
        if (atomic_load_explicit(&codec->out_head, memory_order_relaxed) -
            atomic_load_explicit(&codec->out_tail, memory_order_acquire) >= SLOTS) {
            struct pollfd event = {.fd = codec->input_event, .events = POLLIN};
            (void)poll(&event, 1, 100);
            drain_event(codec->input_event);
            continue;
        }
        if (tail == atomic_load_explicit(&codec->in_head, memory_order_acquire)) {
            struct pollfd event = {.fd = codec->input_event, .events = POLLIN};
            (void)poll(&event, 1, 100);
            drain_event(codec->input_event);
            continue;
        }
        input = codec->input[tail % SLOTS];
        atomic_store_explicit(&codec->in_tail, tail + 1, memory_order_release);
        if (input.generation != generation) {
            atomic_fetch_add(&codec->input_drops, 1); continue;
        }
        if (codec->encode) {
            float converted[CAPTURE_MAX_FRAMES];
            long frames, at;
            uint64_t boundary = atomic_load(&codec->broken_until);
            if (input.pts_ms < boundary || milliseconds() - input.offered_ms > KMX_ENCODEC_STALE_MS) {
                atomic_fetch_add(&codec->input_drops, 1);
                if (input.pts_ms >= boundary) break_epoch(codec, input.pts_ms);
                started = false; used = 0;
                continue;
            }
            if (!started || input.pts_ms != next_capture) {
                if (input.pts_ms % 1000u) {
                    break_epoch(codec, input.pts_ms);
                    atomic_fetch_add(&codec->input_drops, 1);
                    started = false; used = 0;
                    continue;
                }
                if (codec->resampler) src_reset(codec->resampler);
                kenc_encoder_reset(codec->encoder);
                atomic_fetch_add(&codec->discontinuities, 1);
                next_pts = input.pts_ms; used = 0; started = true;
            }
            next_capture = input.pts_ms + 40;
            frames = resample(codec, &input, converted);
            if (frames < 0) {
                break_epoch(codec, input.pts_ms); started = false; used = 0;
                atomic_fetch_add(&codec->input_drops, 1); continue;
            }
            for (at = 0; at < frames; at++) {
                float value = converted[at] * 32768.0f;
                if (!isfinite(value)) value = 0;
                if (value > 32767.0f) value = 32767.0f;
                if (value < -32768.0f) value = -32768.0f;
                if (!used) accumulated_offered_ms = input.offered_ms;
                accumulated[used++] = (int16_t)lrintf(value);
                if (used == KMX_ENCODEC_SAMPLES) {
                    kmx_encodec_output output = {0};
                    kenc_packet_metadata metadata;
                    uint64_t began = nanoseconds();
                    kenc_result result = kenc_encoder_push_s16(codec->encoder, accumulated, used, next_pts,
                        output.packet, sizeof output.packet, &output.size);
                    atomic_fetch_add(&codec->inference_ns, nanoseconds() - began);
                    atomic_fetch_add(&codec->calls, 1);
                    used = 0;
                    if (result != KENC_OK || milliseconds() - accumulated_offered_ms > KMX_ENCODEC_STALE_MS ||
                        kenc_packet_metadata_read(&metadata, output.packet, output.size, &codec->options) != KENC_OK) {
                        break_epoch(codec, input.pts_ms); started = false;
                        atomic_fetch_add(&codec->output_drops, 1); break;
                    }
                    output.pts_ms = metadata.packet.pts_ms; output.flags = metadata.packet.flags;
                    output.epoch = metadata.epoch; output.created_ms = accumulated_offered_ms;
                    if (!publish(codec, &output, generation)) { started = false; break; }
                    next_pts += 40;
                }
            }
        } else {
            kenc_packet_metadata metadata;
            kmx_encodec_output output = {0};
            kenc_packet_info info;
            size_t samples = 0;
            uint64_t began;
            kenc_result result;
            if (milliseconds() - input.offered_ms > KMX_ENCODEC_STALE_MS ||
                kenc_packet_metadata_read(&metadata, input.bytes, input.size, &codec->options) != KENC_OK) {
                waiting = true; atomic_fetch_add(&codec->input_drops, 1); continue;
            }
            if (waiting && !(metadata.packet.flags & KENC_PACKET_FLAG_RESET)) {
                atomic_fetch_add(&codec->input_drops, 1); continue;
            }
            began = nanoseconds();
            result = kenc_decoder_pull_s16(codec->decoder, input.bytes, input.size,
                output.pcm, KMX_ENCODEC_SAMPLES, &samples, &info);
            atomic_fetch_add(&codec->inference_ns, nanoseconds() - began);
            atomic_fetch_add(&codec->calls, 1);
            if (result != KENC_OK) { waiting = true; atomic_fetch_add(&codec->input_drops, 1); continue; }
            waiting = false;
            if (info.flags & KENC_PACKET_FLAG_DISCONTINUITY) atomic_fetch_add(&codec->discontinuities, 1);
            if (milliseconds() - input.offered_ms > KMX_ENCODEC_STALE_MS) {
                waiting = true; atomic_fetch_add(&codec->output_drops, 1); continue;
            }
            output.size = samples * 2u; output.pts_ms = info.pts_ms;
            output.flags = info.flags; output.epoch = metadata.epoch; output.created_ms = input.offered_ms;
            (void)publish(codec, &output, generation);
        }
    }
    return NULL;
}

kmx_encodec *kmx_encodec_open(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets) {
    kmx_encodec *codec = NULL;
    kenc_model *model = NULL;
    kenc_installed_assets *assets = NULL;
    kenc_encoder *warm_encoder = NULL;
    kenc_decoder *warm_decoder = NULL;
    int16_t silence[KMX_ENCODEC_SAMPLES] = {0}, decoded[KMX_ENCODEC_SAMPLES];
    unsigned char packet[KMX_ENCODEC_PACKET_MAX];
    size_t written = 0, samples = 0;
    kenc_packet_info info;
    char root[PATH_MAX], passwd_buffer[16384];
    struct passwd account, *found = NULL;
    int error;
    if (!kmx_audio_rate_bit(bitrate) || (encode &&
        ((capture_rate != 24000 && capture_rate != 44100 && capture_rate != 48000) ||
         capture_channels < 1 || capture_channels > 8))) return NULL;
    codec = calloc(1, sizeof *codec);
    if (!codec) return NULL;
    atomic_init(&codec->stop, false);
    atomic_init(&codec->in_head, 0); atomic_init(&codec->in_tail, 0);
    atomic_init(&codec->out_head, 0); atomic_init(&codec->out_tail, 0);
    atomic_init(&codec->generation, 0); atomic_init(&codec->broken_until, 0);
    atomic_init(&codec->calls, 0); atomic_init(&codec->inference_ns, 0);
    atomic_init(&codec->input_drops, 0); atomic_init(&codec->output_drops, 0);
    atomic_init(&codec->discontinuities, 0);
    codec->input_event = codec->output_event = -1;
    codec->encode = encode; codec->rate = capture_rate; codec->channels = capture_channels;
    codec->options = kenc_options_default(); codec->options.threads = 2;
    codec->options.codebooks = (uint8_t)(bitrate * 4u / 3u);
    if (development_assets) {
        if (kenc_model_load(&model, development_assets) != KENC_OK) goto fail;
    } else {
        if (!content_root || !*content_root) {
            if (getpwuid_r(geteuid(), &account, passwd_buffer, sizeof passwd_buffer, &found) ||
                !found || !account.pw_dir || account.pw_dir[0] != '/') goto fail;
            error = snprintf(root, sizeof root, "%s/.local/gpu_terminal/kilix/data/desktop-apps", account.pw_dir);
            if (error < 0 || (size_t)error >= sizeof root) goto fail;
            content_root = root;
        }
        if (kenc_installed_assets_open(&assets, 1, content_root, 120000, NULL, NULL) != KENC_OK ||
            kenc_model_load_fds(&model, kenc_installed_assets_files(assets)) != KENC_OK) goto fail;
        kenc_installed_assets_free(assets); assets = NULL;
    }
    if (kenc_encoder_create(&warm_encoder, model, &codec->options) != KENC_OK ||
        kenc_decoder_create(&warm_decoder, model, &codec->options) != KENC_OK ||
        kenc_encoder_push_s16(warm_encoder, silence, KMX_ENCODEC_SAMPLES, 0, packet, sizeof packet, &written) != KENC_OK ||
        kenc_decoder_pull_s16(warm_decoder, packet, written, decoded, KMX_ENCODEC_SAMPLES, &samples, &info) != KENC_OK ||
        samples != KMX_ENCODEC_SAMPLES) goto fail;
    if (encode) { codec->encoder = warm_encoder; warm_encoder = NULL; }
    else { codec->decoder = warm_decoder; warm_decoder = NULL; kenc_decoder_reset(codec->decoder); }
    kenc_encoder_free(warm_encoder); warm_encoder = NULL;
    kenc_decoder_free(warm_decoder); warm_decoder = NULL;
    kenc_model_free(model); model = NULL;
    if (encode && capture_rate != 24000) {
        codec->resampler = src_new(SRC_SINC_FASTEST, 1, &error);
        if (!codec->resampler) goto fail;
    }
    codec->input_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    codec->output_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (codec->input_event < 0 || codec->output_event < 0 || pthread_create(&codec->thread, NULL, worker, codec)) goto fail;
    return codec;
fail:
    kenc_installed_assets_free(assets); kenc_model_free(model);
    kenc_encoder_free(warm_encoder); kenc_decoder_free(warm_decoder);
    if (codec) {
        kenc_encoder_free(codec->encoder); kenc_decoder_free(codec->decoder);
        if (codec->resampler) src_delete(codec->resampler);
        if (codec->input_event >= 0) close(codec->input_event);
        if (codec->output_event >= 0) close(codec->output_event);
        free(codec);
    }
    return NULL;
}

void kmx_encodec_close(kmx_encodec *codec) {
    if (!codec) return;
    atomic_store(&codec->stop, true); notify(codec->input_event);
    pthread_join(codec->thread, NULL);
    kenc_encoder_free(codec->encoder); kenc_decoder_free(codec->decoder);
    if (codec->resampler) src_delete(codec->resampler);
    close(codec->input_event); close(codec->output_event); free(codec);
}

int kmx_encodec_event_fd(const kmx_encodec *codec) { return codec ? codec->output_event : -1; }

static bool offer(kmx_encodec *codec, const void *bytes, size_t size, uint64_t pts_ms) {
    uint64_t head = atomic_load_explicit(&codec->in_head, memory_order_relaxed);
    codec_input *input;
    if (codec->encode && pts_ms < atomic_load(&codec->broken_until)) {
        atomic_fetch_add(&codec->input_drops, 1); return false;
    }
    if (head - atomic_load_explicit(&codec->in_tail, memory_order_acquire) >= SLOTS) {
        atomic_fetch_add(&codec->input_drops, 1);
        if (codec->encode) break_epoch(codec, pts_ms);
        return false;
    }
    input = &codec->input[head % SLOTS];
    memcpy(input->bytes, bytes, size); input->size = size; input->pts_ms = pts_ms;
    input->offered_ms = milliseconds(); input->generation = generation_now(codec);
    atomic_store_explicit(&codec->in_head, head + 1, memory_order_release);
    notify(codec->input_event);
    return true;
}

bool kmx_encodec_offer_pcm(kmx_encodec *codec, const void *pcm, size_t bytes, uint64_t pts_ms) {
    if (!codec || !codec->encode || !pcm || pts_ms % 40u || pts_ms > UINT64_MAX - 1000u ||
        bytes != (size_t)(codec->rate / 25) * (size_t)codec->channels * 2u) return false;
    return offer(codec, pcm, bytes, pts_ms);
}
bool kmx_encodec_offer_packet(kmx_encodec *codec, const void *packet, size_t bytes) {
    if (!codec || codec->encode || !packet || !bytes || bytes > KMX_ENCODEC_PACKET_MAX) return false;
    return offer(codec, packet, bytes, 0);
}

bool kmx_encodec_receive(kmx_encodec *codec, kmx_encodec_output *output) {
    if (!codec || !output) return false;
    drain_event(codec->output_event);
    for (;;) {
        uint64_t tail = atomic_load_explicit(&codec->out_tail, memory_order_relaxed);
        codec_output current;
        if (tail == atomic_load_explicit(&codec->out_head, memory_order_acquire)) return false;
        current = codec->output[tail % SLOTS];
        atomic_store_explicit(&codec->out_tail, tail + 1, memory_order_release);
        notify(codec->input_event);
        if (current.generation != generation_now(codec) ||
            (codec->encode && current.value.pts_ms < atomic_load(&codec->broken_until))) {
            atomic_fetch_add(&codec->output_drops, 1); continue;
        }
        if (milliseconds() - current.value.created_ms > KMX_ENCODEC_STALE_MS) {
            atomic_fetch_add(&codec->output_drops, 1);
            if (codec->encode) break_epoch(codec, current.value.pts_ms);
            continue;
        }
        *output = current.value;
        return true;
    }
}

void kmx_encodec_restart(kmx_encodec *codec) {
    if (!codec || codec->encode) return;
    atomic_fetch_add(&codec->generation, 1); notify(codec->input_event);
}

kmx_encodec_stats kmx_encodec_statistics(const kmx_encodec *codec) {
    if (!codec) return (kmx_encodec_stats){0};
    return (kmx_encodec_stats){atomic_load(&codec->calls), atomic_load(&codec->inference_ns),
        atomic_load(&codec->input_drops), atomic_load(&codec->output_drops), atomic_load(&codec->discontinuities)};
}
#else
kmx_encodec *kmx_encodec_open(bool encode, unsigned bitrate, int capture_rate,
    int capture_channels, const char *content_root, const char *development_assets) {
    (void)encode; (void)bitrate; (void)capture_rate; (void)capture_channels;
    (void)content_root; (void)development_assets; return NULL;
}
void kmx_encodec_close(kmx_encodec *codec) { (void)codec; }
int kmx_encodec_event_fd(const kmx_encodec *codec) { (void)codec; return -1; }
bool kmx_encodec_offer_pcm(kmx_encodec *codec, const void *pcm, size_t bytes, uint64_t pts_ms) {
    (void)codec; (void)pcm; (void)bytes; (void)pts_ms; return false;
}
bool kmx_encodec_offer_packet(kmx_encodec *codec, const void *packet, size_t bytes) {
    (void)codec; (void)packet; (void)bytes; return false;
}
bool kmx_encodec_receive(kmx_encodec *codec, kmx_encodec_output *output) { (void)codec; (void)output; return false; }
void kmx_encodec_restart(kmx_encodec *codec) { (void)codec; }
kmx_encodec_stats kmx_encodec_statistics(const kmx_encodec *codec) { (void)codec; return (kmx_encodec_stats){0}; }
#endif
