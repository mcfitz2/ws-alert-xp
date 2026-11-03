/*
 * a2dp_src.c - A2DP source test for the WS6.
 *
 * Question: can the headset play A2DP (music-quality SBC) from the same AG
 * that holds it in HFP radio mode, and does the PTT button (AT+CTKST) keep
 * working while it plays? If yes, FM/radio-RX audio can go over A2DP instead
 * of 8 kHz SCO.
 *
 * Streams BTstack's built-in MOD player (music makes codec quality audible)
 * as SBC. Trimmed from btstack/example/a2dp_source_demo.c: no AVRCP, no
 * inquiry, no stdin; one stream to a known address.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "mods/mod.h"
#include "a2dp_src.h"

#define NUM_CHANNELS     2
#define AUDIO_TIMEOUT_MS 10
#define SBC_STORAGE_SIZE 1030

// Scale samples down by 2^VOLUME_SHIFT so the first listen is not too loud.
#define VOLUME_SHIFT 2

static uint8_t media_sbc_codec_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) | AVDTP_SBC_STEREO,
    0xFF,
    2, 53
};
static uint8_t media_sbc_codec_configuration[4];
static uint8_t sdp_a2dp_source_service_buffer[150];

static struct {
    uint16_t a2dp_cid;
    uint8_t  local_seid;
    bool     streaming;
    btstack_timer_source_t audio_timer;
    uint32_t time_audio_data_sent;
    uint32_t acc_num_missed_samples;
    uint32_t samples_ready;
    int      max_media_payload_size;
    uint32_t rtp_timestamp;
    uint8_t  sbc_storage[SBC_STORAGE_SIZE];
    uint16_t sbc_storage_count;
    bool     sbc_ready_to_send;
    uint32_t packets_sent;
} media;

static const btstack_sbc_encoder_t *   sbc_encoder;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state;
static btstack_audio_generator_mod_t   generator;
static bool generator_initialized = false;
static int  sample_rate = 44100;

static void produce_audio(int16_t *pcm, int num_samples)
{
    btstack_audio_generator_generate(&generator.base, pcm, num_samples);
    for (int i = 0; i < num_samples * NUM_CHANNELS; i++) pcm[i] >>= VOLUME_SHIFT;
}

static void send_media_packet(void)
{
    int frame_len = sbc_encoder->sbc_buffer_length(&sbc_encoder_state);
    uint8_t num_frames = media.sbc_storage_count / frame_len;
    media.sbc_storage[0] = num_frames;
    a2dp_source_stream_send_media_payload_rtp(media.a2dp_cid, media.local_seid, 0,
                                              media.rtp_timestamp,
                                              media.sbc_storage, media.sbc_storage_count + 1);
    media.rtp_timestamp += num_frames * sbc_encoder->num_audio_frames(&sbc_encoder_state);
    media.sbc_storage_count = 0;
    media.sbc_ready_to_send = false;
    if ((++media.packets_sent % 500) == 0)
        printf("[a2dp] %lu media packets sent\n", (unsigned long)media.packets_sent);
}

static void fill_sbc_buffer(void)
{
    unsigned int samples_per_frame = sbc_encoder->num_audio_frames(&sbc_encoder_state);
    uint16_t frame_len = sbc_encoder->sbc_buffer_length(&sbc_encoder_state);
    while (media.samples_ready >= samples_per_frame &&
           (media.max_media_payload_size - media.sbc_storage_count) >= frame_len) {
        int16_t pcm[256 * NUM_CHANNELS];
        produce_audio(pcm, samples_per_frame);
        sbc_encoder->encode_signed_16(&sbc_encoder_state, pcm,
                                      &media.sbc_storage[1 + media.sbc_storage_count]);
        media.sbc_storage_count += frame_len;
        media.samples_ready -= samples_per_frame;
    }
}

static void audio_timeout_handler(btstack_timer_source_t *timer)
{
    btstack_run_loop_set_timer(timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(timer);
    uint32_t now = btstack_run_loop_get_time_ms();

    uint32_t period_ms = media.time_audio_data_sent ? now - media.time_audio_data_sent : AUDIO_TIMEOUT_MS;
    uint32_t num_samples = (period_ms * sample_rate) / 1000;
    media.acc_num_missed_samples += (period_ms * sample_rate) % 1000;
    while (media.acc_num_missed_samples >= 1000) {
        num_samples++;
        media.acc_num_missed_samples -= 1000;
    }
    media.time_audio_data_sent = now;
    media.samples_ready += num_samples;

    if (media.sbc_ready_to_send) return;
    fill_sbc_buffer();
    if ((media.sbc_storage_count + sbc_encoder->sbc_buffer_length(&sbc_encoder_state)) > media.max_media_payload_size) {
        media.sbc_ready_to_send = true;
        a2dp_source_stream_endpoint_request_can_send_now(media.a2dp_cid, media.local_seid);
    }
}

static void timer_start(void)
{
    media.max_media_payload_size = btstack_min(a2dp_max_media_payload_size(media.a2dp_cid, media.local_seid), SBC_STORAGE_SIZE);
    media.sbc_storage_count = 0;
    media.sbc_ready_to_send = false;
    media.streaming = true;
    btstack_run_loop_remove_timer(&media.audio_timer);
    btstack_run_loop_set_timer_handler(&media.audio_timer, audio_timeout_handler);
    btstack_run_loop_set_timer(&media.audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&media.audio_timer);
}

static void timer_stop(void)
{
    media.time_audio_data_sent = 0;
    media.acc_num_missed_samples = 0;
    media.samples_ready = 0;
    media.streaming = false;
    media.sbc_storage_count = 0;
    media.sbc_ready_to_send = false;
    btstack_run_loop_remove_timer(&media.audio_timer);
}

static void start_generator(void)
{
    if (generator_initialized) btstack_audio_generator_finalize(&generator.base);
    btstack_audio_generator_modplayer_init(&generator, sample_rate, NUM_CHANNELS,
                                           mod_titles[MOD_TITLE_DEFAULT].data,
                                           mod_titles[MOD_TITLE_DEFAULT].len);
    generator_initialized = true;
    media.sbc_storage_count = 0;
    media.samples_ready = 0;
}

static void a2dp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t status;
    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        status = a2dp_subevent_signaling_connection_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[a2dp] signaling connect failed, status 0x%02x\n", status);
            media.a2dp_cid = 0;
            break;
        }
        media.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        printf("[a2dp] signaling up, cid 0x%02x\n", media.a2dp_cid);
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
        printf("[a2dp] sink codec: SBC\n");
        break;
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY:
        printf("[a2dp] sink codec: AAC\n");
        break;
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY: {
        const uint8_t *info = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
        uint32_t vendor = little_endian_read_32(info, 0);
        uint16_t codec  = little_endian_read_16(info, 4);
        printf("[a2dp] sink codec: vendor 0x%04lx codec 0x%04x%s\n", (unsigned long)vendor, codec,
               (vendor == 0x4f && codec == 0x01) ? " (aptX)" :
               (vendor == 0xd7 && codec == 0x24) ? " (aptX HD)" :
               (vendor == 0xd7 && codec == 0xad) ? " (aptX Adaptive)" : "");
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        int num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
        sample_rate      = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        int block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
        int subbands     = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
        int max_bitpool  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
        uint8_t alloc    = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
        avdtp_channel_mode_t mode = (avdtp_channel_mode_t)
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);

        btstack_sbc_channel_mode_t sbc_mode;
        switch (mode) {
        case AVDTP_CHANNEL_MODE_JOINT_STEREO: sbc_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
        case AVDTP_CHANNEL_MODE_STEREO:       sbc_mode = SBC_CHANNEL_MODE_STEREO; break;
        case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: sbc_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
        default:                              sbc_mode = SBC_CHANNEL_MODE_MONO; break;
        }
        printf("[a2dp] SBC config: %d Hz, %d ch, mode %d, blocks %d, subbands %d, bitpool max %d\n",
               sample_rate, num_channels, (int)mode, block_length, subbands, max_bitpool);

        sbc_encoder = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state);
        sbc_encoder->configure(&sbc_encoder_state, SBC_MODE_STANDARD, block_length, subbands,
                               (btstack_sbc_allocation_method_t)(alloc - 1), sample_rate,
                               max_bitpool, sbc_mode);
        break;
    }

    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[a2dp] stream open failed, status 0x%02x\n", status);
            break;
        }
        printf("[a2dp] stream open, starting\n");
        a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
        break;

    case A2DP_SUBEVENT_STREAM_STARTED:
        start_generator();
        timer_start();
        printf("[a2dp] STREAMING (MOD player)\n");
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        send_media_packet();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        printf("[a2dp] stream SUSPENDED by headset\n");
        timer_stop();
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("[a2dp] stream released\n");
        timer_stop();
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("[a2dp] signaling released\n");
        media.a2dp_cid = 0;
        timer_stop();
        break;

    default:
        break;
    }
}

void a2dp_src_init(void)
{
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_packet_handler);

    avdtp_stream_endpoint_t *ep = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
        media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration));
    btstack_assert(ep != NULL);
    avdtp_set_preferred_sampling_frequency(ep, 44100);
    media.local_seid = avdtp_local_seid(ep);

    // Caller has already run sdp_init().
    memset(sdp_a2dp_source_service_buffer, 0, sizeof(sdp_a2dp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_service_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_service_buffer);
}

void a2dp_src_connect(bd_addr_t addr)
{
    if (media.a2dp_cid) return;
    uint8_t st = a2dp_source_establish_stream(addr, &media.a2dp_cid);
    printf("[a2dp] connecting to headset (status 0x%02x)\n", st);
}

bool a2dp_src_streaming(void)
{
    return media.streaming;
}
