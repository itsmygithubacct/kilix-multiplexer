#define _POSIX_C_SOURCE 200809L
#include "encodec_double.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct kenc_model { int unused; };
struct kenc_encoder { uint64_t epoch; bool reset; unsigned profile; };
struct kenc_decoder { unsigned profile; };
static atomic_bool armed, entered, released;
void kenc_double_arm(void) { atomic_store(&entered, false); atomic_store(&released, false); atomic_store(&armed, true); }
bool kenc_double_entered(void) { return atomic_load(&entered); }
void kenc_double_release(void) { atomic_store(&released, true); }
kenc_options kenc_options_default(void) { return (kenc_options){24000, 960, 25, 8, 2}; }
kenc_result kenc_model_load(kenc_model **out, const char *path) {
    /* Explicit magic test input, with no filesystem read or native fallback. */
    if (!path || strcmp(path, "double-only")) return KENC_ERR_MODEL;
    *out = calloc(1, sizeof **out); return *out ? KENC_OK : KENC_ERR_MODEL;
}
void kenc_model_free(kenc_model *model) { free(model); }
kenc_result kenc_model_load_fds(kenc_model **out, const kenc_asset_set *assets) {
    (void)out; (void)assets; return KENC_ERR_MODEL;
}
kenc_result kenc_installed_assets_open(kenc_installed_assets **out, uint8_t profile,
    const char *root, uint32_t timeout, kenc_admission_cancelled cancelled, void *context) {
    (void)out; (void)profile; (void)root; (void)timeout; (void)cancelled; (void)context;
    return KENC_ERR_MODEL;
}
const kenc_asset_set *kenc_installed_assets_files(const kenc_installed_assets *assets) { (void)assets; return NULL; }
void kenc_installed_assets_free(kenc_installed_assets *assets) { (void)assets; }
kenc_result kenc_encoder_create(kenc_encoder **out, kenc_model *model, const kenc_options *options) {
    (void)model; (void)options; *out = calloc(1, sizeof **out);
    if (*out) (*out)->reset = true;
    return *out ? KENC_OK : KENC_ERR_MODEL;
}
kenc_result kenc_decoder_create(kenc_decoder **out, kenc_model *model, const kenc_options *options) {
    (void)model; (void)options; *out = calloc(1, sizeof **out); return *out ? KENC_OK : KENC_ERR_MODEL;
}
kenc_result kenc_encoder_set_epoch_start(kenc_encoder *encoder, kenc_epoch_start profile) { encoder->profile = (unsigned)profile; return KENC_OK; }
kenc_result kenc_decoder_set_epoch_start(kenc_decoder *decoder, kenc_epoch_start profile) { decoder->profile = (unsigned)profile; return KENC_OK; }
void kenc_encoder_reset(kenc_encoder *encoder) { encoder->reset = true; }
void kenc_decoder_reset(kenc_decoder *decoder) { (void)decoder; }
void kenc_encoder_free(kenc_encoder *encoder) { free(encoder); }
void kenc_decoder_free(kenc_decoder *decoder) { free(decoder); }
kenc_result kenc_encoder_push_s16(kenc_encoder *encoder, const int16_t *pcm, size_t samples,
    uint64_t pts, uint8_t *packet, size_t capacity, size_t *written) {
    (void)pcm;
    if (atomic_exchange(&armed, false)) {
        atomic_store(&entered, true);
        while (!atomic_load(&released)) { struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL); }
    }
    kenc_packet_metadata metadata = {.packet = {.pts_ms = pts, .samples = (uint16_t)samples}};
    if (encoder->reset || pts % 1000u == 0) {
        encoder->epoch++;
        metadata.packet.flags = (uint8_t)(encoder->reset ? 5 : 1);
        if (encoder->profile) metadata.packet.flags |= 8;
    }
    encoder->reset = false;
    metadata.epoch = encoder->epoch; metadata.index = pts % 1000u / 40u;
    if (capacity < sizeof metadata) return KENC_ERR_MODEL;
    memcpy(packet, &metadata, sizeof metadata); *written = sizeof metadata;
    return KENC_OK;
}
kenc_result kenc_packet_metadata_read(kenc_packet_metadata *metadata, const uint8_t *packet,
    size_t size, const kenc_options *options) {
    (void)options;
    if (size != sizeof *metadata) return KENC_ERR_MODEL;
    memcpy(metadata, packet, size); return KENC_OK;
}
kenc_result kenc_decoder_pull_s16(kenc_decoder *decoder, const uint8_t *packet, size_t bytes,
    int16_t *pcm, size_t capacity, size_t *samples, kenc_packet_info *info) {
    (void)decoder;
    kenc_packet_metadata metadata;
    if (kenc_packet_metadata_read(&metadata, packet, bytes, NULL) != KENC_OK || capacity < 960) return KENC_ERR_MODEL;
    memset(pcm, 0, 960 * sizeof *pcm); *samples = 960; *info = metadata.packet; return KENC_OK;
}
