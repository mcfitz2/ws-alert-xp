/*
 * sco_audio.c - stream the embedded commentary clip to the headset over SCO.
 *
 * Stand-in for the FM receiver: commentary.ul (16 kHz mono mu-law, built by
 * tools/make_commentary.sh) loops into the SCO link, encoded as mSBC (16 kHz)
 * or CVSD (8 kHz, every other sample) depending on the negotiated codec.
 * sco_audio_mute() ramps the level to zero and back (PTT held = FM muted).
 */

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/hfp_codec.h"
#include "sco_audio.h"

extern const uint8_t commentary_start[] asm("_binary_commentary_ul_start");
extern const uint8_t commentary_end[]   asm("_binary_commentary_ul_end");

// Mute/unmute ramp, in 16 kHz samples (20 ms): avoids clicks on PTT edges.
#define RAMP_SAMPLES 320
#define GAIN_ONE 32768

static int16_t ulaw_table[256];
static size_t clip_pos;
static int32_t gain = GAIN_ONE;
static int32_t gain_target = GAIN_ONE;

static uint8_t codec;
static hfp_codec_t hfp_codec;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_context;

static int16_t ulaw_decode(uint8_t u)
{
    u = ~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

// Next 16 kHz sample from the looping clip, with the mute ramp applied.
static int16_t next_sample(void)
{
    size_t len = (size_t)(commentary_end - commentary_start);
    int16_t s = ulaw_table[commentary_start[clip_pos]];
    if (++clip_pos >= len) clip_pos = 0;

    if (gain < gain_target) {
        gain += GAIN_ONE / RAMP_SAMPLES;
        if (gain > gain_target) gain = gain_target;
    } else if (gain > gain_target) {
        gain -= GAIN_ONE / RAMP_SAMPLES;
        if (gain < gain_target) gain = gain_target;
    }
    return (int16_t)((s * gain) >> 15);
}

void sco_audio_init(void)
{
    for (int i = 0; i < 256; i++) ulaw_table[i] = ulaw_decode((uint8_t)i);
    printf("[sco] commentary clip %u bytes (%u s)\n",
           (unsigned)(commentary_end - commentary_start),
           (unsigned)((commentary_end - commentary_start) / 16000));
}

void sco_audio_start(uint8_t negotiated_codec)
{
    codec = negotiated_codec;
    if (codec == HFP_CODEC_MSBC) {
        const btstack_sbc_encoder_t *enc =
            btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_context);
        hfp_codec_init_msbc_with_codec(&hfp_codec, enc, &sbc_encoder_context);
    }
}

void sco_audio_stop(void)
{
    if (codec == HFP_CODEC_MSBC) hfp_codec_deinit(&hfp_codec);
}

void sco_audio_mute(bool mute)
{
    gain_target = mute ? 0 : GAIN_ONE;
}

void sco_audio_fill(uint8_t *payload, uint16_t len)
{
    if (codec != HFP_CODEC_MSBC) {
        // CVSD: 16-bit linear at 8 kHz, so drop every other 16 kHz sample.
        for (uint16_t i = 0; i < len / 2; i++) {
            int16_t s = next_sample();
            next_sample();
            little_endian_store_16(payload, i * 2, (uint16_t)s);
        }
        return;
    }
    while (len > 0) {
        if (hfp_codec_can_encode_audio_frame_now(&hfp_codec)) {
            int16_t pcm[120];
            uint16_t n = hfp_codec_num_audio_samples_per_frame(&hfp_codec);
            for (uint16_t i = 0; i < n; i++) pcm[i] = next_sample();
            hfp_codec_encode_audio_frame(&hfp_codec, pcm);
        }
        uint16_t avail = hfp_codec_num_bytes_available(&hfp_codec);
        uint16_t take = avail < len ? avail : len;
        hfp_codec_read_from_stream(&hfp_codec, payload, take);
        payload += take;
        len -= take;
    }
}
