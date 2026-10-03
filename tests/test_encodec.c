#define _POSIX_C_SOURCE 200809L
#include "kmx_encodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static void capabilities(void) {
    unsigned bitrate = 99, i, codecs, rates;
    kmx_audio_mode mode = KMX_AUDIO_AUTO;
    const char *bad[] = {"", "1", "03", "+3", "6 ", "-1", "24", "3kbps"};
    const unsigned supported[] = {3,6,12};
    unsigned char bytes[KMX_AUDIO_CAPS_BYTES + 1];
    kmx_audio_caps value, sentinel;
    memset(&sentinel, 0x35, sizeof sentinel);
    CHECK(kmx_audio_mode_parse("auto", &mode) == 0 && mode == KMX_AUDIO_AUTO);
    CHECK(kmx_audio_mode_parse("pcm", &mode) == 0 && mode == KMX_AUDIO_PCM);
    CHECK(kmx_audio_mode_parse("encodec", &mode) == 0 && mode == KMX_AUDIO_ENCODEC);
    CHECK(kmx_audio_mode_parse("ENCODEC", &mode) == -1 && mode == KMX_AUDIO_ENCODEC);
    for (i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(kmx_audio_bitrate_parse(bad[i], &bitrate) == -1 && bitrate == 99);
    CHECK(kmx_audio_bitrate_parse("3", &bitrate) == 0 && bitrate == 3);
    CHECK(kmx_audio_bitrate_parse("6", &bitrate) == 0 && bitrate == 6);
    CHECK(kmx_audio_bitrate_parse("12", &bitrate) == 0 && bitrate == 12);
    {
        unsigned threads = 99;
        const char *invalid_threads[] = {"", "1", "3", "04", "+4", "4 ", "8"};
        for (i = 0; i < sizeof invalid_threads / sizeof *invalid_threads; i++)
            CHECK(kmx_audio_threads_parse(invalid_threads[i], &threads) == -1 && threads == 99);
        CHECK(kmx_audio_threads_parse("2", &threads) == 0 && threads == 2);
        CHECK(kmx_audio_threads_parse("4", &threads) == 0 && threads == 4);
        CHECK(kmx_audio_threads_parse(NULL, &threads) == -1 && threads == 4);
        CHECK(kmx_audio_threads_parse("4", NULL) == -1);
    }
    for (codecs = 0; codecs < 256; codecs++) for (rates = 0; rates < 8; rates++) for (i = 0; i < 3; i++) {
        kmx_audio_caps offer = {0, (uint8_t)codecs, (uint8_t)rates, KMX_ENCODEC_PACKET_MAX};
        kmx_audio_caps chosen = kmx_audio_choose(&offer, true, supported[i], KMX_AUDIO_AUTO);
        unsigned expected = (codecs & 2u) && (rates & (1u << i)) ? 2 : codecs & 1u;
        CHECK(chosen.codecs == expected && chosen.kind == (expected ? 1 : 2));
        kmx_audio_caps_write(bytes, &chosen);
        CHECK(kmx_audio_caps_read(&value, bytes, KMX_AUDIO_CAPS_BYTES) == 0 && value.codecs == expected);
        chosen = kmx_audio_choose(&offer, false, supported[i], KMX_AUDIO_AUTO);
        CHECK(chosen.codecs == (codecs & 1u));
        chosen = kmx_audio_choose(&offer, true, supported[i], KMX_AUDIO_PCM);
        CHECK(chosen.codecs == (codecs & 1u));
        chosen = kmx_audio_choose(&offer, false, supported[i], KMX_AUDIO_ENCODEC);
        CHECK(chosen.kind == 2);
    }
    {
        kmx_audio_caps good = {1,2,2,160};
        kmx_audio_caps_write(bytes, &good);
        for (i = 0; i < KMX_AUDIO_CAPS_BYTES; i++) {
            value = sentinel;
            CHECK(kmx_audio_caps_read(&value, bytes, i) == -1 && memcmp(&value, &sentinel, sizeof value) == 0);
        }
        CHECK(kmx_audio_caps_read(&value, bytes, KMX_AUDIO_CAPS_BYTES + 1) == -1);
        for (i = 0; i < KMX_AUDIO_CAPS_BYTES; i++) {
            unsigned char original = bytes[i];
            if (i == 8 || i == 9) continue;
            bytes[i] ^= 0x80u; value = sentinel;
            CHECK(kmx_audio_caps_read(&value, bytes, KMX_AUDIO_CAPS_BYTES) == -1 && memcmp(&value, &sentinel, sizeof value) == 0);
            bytes[i] = original;
        }
        good.kind = 0; good.maximum = 159;
        CHECK(kmx_audio_choose(&good, true, 6, KMX_AUDIO_ENCODEC).kind == 2);
    }
    CHECK(kmx_encodec_offer_pcm(NULL, bytes, sizeof bytes, 0) == false);
    CHECK(kmx_encodec_offer_packet(NULL, bytes, sizeof bytes) == false);
    CHECK(kmx_encodec_receive(NULL, NULL) == false);
    kmx_encodec_close(NULL); kmx_encodec_restart(NULL);
    {
        kmx_audio_profile profile = {0, UINT32_C(0x80000003)}, parsed;
        kmx_audio_profile_write(bytes, &profile);
        CHECK(kmx_audio_profile_read(&parsed, bytes, KMX_AUDIO_PROFILE_BYTES) == 0);
        CHECK(parsed.kind == 0 && parsed.value == profile.value);
        CHECK(kmx_audio_profile_choose(3, parsed.value) == 1);
        CHECK(kmx_audio_profile_choose(3, 0) == 0);
        CHECK(kmx_audio_profile_choose(3, 1) == 0);
        CHECK(kmx_audio_profile_choose(1, 3) == 0);
        for (i = 0; i < KMX_AUDIO_PROFILE_BYTES; i++) {
            memset(&parsed, 0x35, sizeof parsed);
            kmx_audio_profile before = parsed;
            CHECK(kmx_audio_profile_read(&parsed, bytes, i) == -1);
            CHECK(memcmp(&parsed, &before, sizeof parsed) == 0);
        }
        CHECK(kmx_audio_profile_read(&parsed, bytes, KMX_AUDIO_PROFILE_BYTES + 1) == -1);
        for (i = 0; i < 8; i++) {
            unsigned char saved = bytes[i]; bytes[i] ^= 0x80;
            CHECK(kmx_audio_profile_read(&parsed, bytes, KMX_AUDIO_PROFILE_BYTES) == -1);
            bytes[i] = saved;
        }
        profile.value = 2; kmx_audio_profile_write(bytes, &profile);
        CHECK(kmx_audio_profile_read(&parsed, bytes, KMX_AUDIO_PROFILE_BYTES) == -1);
        profile.kind = 1;
        for (i = 0; i < 3; i++) {
            profile.value = i; kmx_audio_profile_write(bytes, &profile);
            CHECK(kmx_audio_profile_read(&parsed, bytes, KMX_AUDIO_PROFILE_BYTES) == (i < 2 ? 0 : -1));
        }
    }
}

#ifdef KMX_HAVE_ENCODEC
#include <kilix_encodec.h>
#include <poll.h>

static void wait_output(kmx_encodec *codec, kmx_encodec_output *output) {
    unsigned i;
    for (i = 0; i < 100; i++) {
        struct pollfd event = {kmx_encodec_event_fd(codec), POLLIN, 0};
        if (kmx_encodec_receive(codec, output)) return;
        (void)poll(&event, 1, 20);
    }
    CHECK(!"native output missing");
}

static void native(const char *assets) {
    unsigned rates[] = {3,6,12}, which, i, thread_count, profile;
    unsigned char pcm[KMX_ENCODEC_SAMPLES * 2u];
    int16_t reference_pcm[KMX_ENCODEC_SAMPLES];
    for (i = 0; i < KMX_ENCODEC_SAMPLES; i++) {
        int16_t value = (int16_t)(((i * 31u) % 4000u) - 2000);
        reference_pcm[i] = value;
        pcm[i*2u] = (unsigned char)value; pcm[i*2u+1u] = (unsigned char)((uint16_t)value >> 8);
    }
    for (profile = 0; profile < 2; profile++)
    for (thread_count = 2; thread_count <= 4; thread_count += 2) for (which = 0; which < 3; which++) {
        kmx_encodec *encoder = profile ? kmx_encodec_open_profile(true, rates[which], 24000, 1, NULL, assets, thread_count, profile) :
            thread_count == 2 ? kmx_encodec_open(true, rates[which], 24000, 1, NULL, assets) :
            kmx_encodec_open_with_threads(true, rates[which], 24000, 1, NULL, assets, thread_count);
        kmx_encodec *decoder = profile ? kmx_encodec_open_profile(false, rates[which], 24000, 1, NULL, assets, thread_count, profile) :
            thread_count == 2 ? kmx_encodec_open(false, rates[which], 24000, 1, NULL, assets) :
            kmx_encodec_open_with_threads(false, rates[which], 24000, 1, NULL, assets, thread_count);
        kenc_model *model = NULL;
        kenc_decoder *reference = NULL;
        kenc_encoder *reference_encoder = NULL;
        kenc_options options = kenc_options_default();
        kmx_encodec_output packet, output;
        CHECK(encoder && decoder);
        options.codebooks = (uint8_t)(rates[which] * 4u / 3u); options.threads = 2;
        CHECK(kenc_model_load(&model, assets) == KENC_OK);
        CHECK(kenc_decoder_create(&reference, model, &options) == KENC_OK);
        CHECK(kenc_encoder_create(&reference_encoder, model, &options) == KENC_OK);
        CHECK(kenc_encoder_set_epoch_start(reference_encoder, (kenc_epoch_start)profile) == KENC_OK);
        CHECK(kenc_decoder_set_epoch_start(reference, (kenc_epoch_start)profile) == KENC_OK);
        {
            /* Match the adapter's admission warmup and first worker reset,
             * including the packet epoch number; no warmup state survives. */
            int16_t silence[KMX_ENCODEC_SAMPLES] = {0};
            unsigned char warm_packet[KMX_ENCODEC_PACKET_MAX];
            size_t warm_size = 0;
            CHECK(kenc_encoder_push_s16(reference_encoder, silence, KMX_ENCODEC_SAMPLES,
                0, warm_packet, sizeof warm_packet, &warm_size) == KENC_OK);
            kenc_encoder_reset(reference_encoder);
        }
        for (i = 0; i < 27; i++) {
            int16_t expected[KMX_ENCODEC_SAMPLES];
            size_t samples = 0;
            kenc_packet_info info;
            unsigned char expected_packet[KMX_ENCODEC_PACKET_MAX];
            size_t expected_size = 0;
            CHECK(kmx_encodec_offer_pcm(encoder, pcm, sizeof pcm, i * 40u));
            wait_output(encoder, &packet);
            CHECK(packet.pts_ms == i * 40u && packet.size <= KMX_ENCODEC_PACKET_MAX);
            CHECK((packet.flags & KENC_PACKET_FLAG_EPOCH_PREROLL) ==
                  ((profile && (packet.flags & KENC_PACKET_FLAG_RESET)) ? KENC_PACKET_FLAG_EPOCH_PREROLL : 0));
            CHECK(kenc_encoder_push_s16(reference_encoder, reference_pcm, KMX_ENCODEC_SAMPLES,
                i * 40u, expected_packet, sizeof expected_packet, &expected_size) == KENC_OK);
            CHECK(expected_size == packet.size && memcmp(expected_packet, packet.packet, packet.size) == 0);
            CHECK(kmx_encodec_offer_packet(decoder, packet.packet, packet.size));
            wait_output(decoder, &output);
            CHECK(kenc_decoder_pull_s16(reference, packet.packet, packet.size, expected,
                                       KMX_ENCODEC_SAMPLES, &samples, &info) == KENC_OK);
            CHECK(samples == KMX_ENCODEC_SAMPLES && output.size == sizeof expected);
            CHECK(output.pts_ms == info.pts_ms && output.flags == info.flags);
            if (thread_count == 2) CHECK(memcmp(expected, output.pcm, sizeof expected) == 0);
            else {
                /* Parallel reduction can cross a PCM16 rounding boundary.
                 * Keep exact packet parity and a one-LSB waveform bound. */
                unsigned sample;
                for (sample = 0; sample < KMX_ENCODEC_SAMPLES; sample++)
                    CHECK(abs((int)expected[sample] - (int)output.pcm[sample]) <= 1);
            }
        }
        CHECK(kmx_encodec_statistics(encoder).input_drops == 0);
        CHECK(kmx_encodec_statistics(decoder).input_drops == 0);
        CHECK(kmx_encodec_statistics(encoder).calls == 27);
        /* Consumer reconnect explicitly resets only its decoder. */
        kmx_encodec_restart(decoder);
        CHECK(kmx_encodec_offer_pcm(encoder, pcm, sizeof pcm, 2000));
        wait_output(encoder, &packet);
        CHECK(packet.pts_ms == 2000 && (packet.flags & 5u) == 5u);
        CHECK(kmx_encodec_offer_packet(decoder, packet.packet, packet.size));
        wait_output(decoder, &output);
        CHECK(output.pts_ms == 2000);
        kenc_encoder_free(reference_encoder); kenc_decoder_free(reference); kenc_model_free(model);
        kmx_encodec_close(encoder); kmx_encodec_close(decoder);
    }
    CHECK(kmx_encodec_open(true, 6, 32000, 1, NULL, assets) == NULL);
    CHECK(kmx_encodec_open(true, 6, 24000, 9, NULL, assets) == NULL);
    CHECK(kmx_encodec_open_with_threads(true, 6, 24000, 1, NULL, assets, 3) == NULL);
    CHECK(kmx_encodec_open_profile(true, 6, 24000, 1, NULL, assets, 4, 2) == NULL);
    {
        kmx_encodec *encoder = kmx_encodec_open(true, 6, 24000, 1, NULL, assets);
        kmx_encodec_output packet;
        struct timespec pause = {0, 150000000};
        CHECK(encoder != NULL);
        /* No output is drained during this burst. Two output slots stop the
         * worker, and the two fixed input slots must refuse continued input. */
        for (i = 0; i < 1000; i++) (void)kmx_encodec_offer_pcm(encoder, pcm, sizeof pcm, i * 40u);
        nanosleep(&pause, NULL);
        CHECK(kmx_encodec_statistics(encoder).input_drops > 0);
        CHECK(kmx_encodec_statistics(encoder).calls <= 2);
        while (kmx_encodec_receive(encoder, &packet)) {}
        nanosleep(&pause, NULL);
        CHECK(kmx_encodec_offer_pcm(encoder, pcm, sizeof pcm, 40000));
        wait_output(encoder, &packet);
        CHECK(packet.pts_ms == 40000 && (packet.flags & 5u) == 5u);
        CHECK(kmx_encodec_offer_pcm(encoder, pcm, sizeof pcm, 40040));
        wait_output(encoder, &packet);
        CHECK(packet.pts_ms == 40040 && packet.flags == 0);
        kmx_encodec_close(encoder);
    }
}
#endif

int main(int argc, char **argv) {
    capabilities();
#ifdef KMX_HAVE_ENCODEC
    if (argc == 3 && strcmp(argv[1], "--development-assets") == 0) native(argv[2]);
    else CHECK(argc == 1);
#else
    (void)argv; CHECK(argc == 1);
    CHECK(kmx_encodec_open(true, 6, 24000, 1, "/missing", NULL) == NULL);
#endif
    printf("encodec adapter: %u checks PASS\n", checks);
    return 0;
}
