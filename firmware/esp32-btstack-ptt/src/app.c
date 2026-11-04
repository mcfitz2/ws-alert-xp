/*
 * Combo device: FM commentary into the WS6 headset, headset button keys a
 * Baofeng UV-5G.
 *
 * ws6_ptt (components/ws6_ptt) does the Bluetooth side. This file decides what
 * the talk key and the SCO link are used for:
 *  - RADIO_PTT_GPIO pulls the radio's K1 PTT line (3.5 mm sleeve, 3.3 V pull-up
 *    on the UV-5G) to ground while the button is held. Wire it straight to the
 *    PTT wire and tie ESP32 GND to the K1 ground (2.5 mm sleeve).
 *  - LED_GPIO (onboard LED) follows the button.
 *  - With COMMENTARY, the embedded clip (sco_audio.c, standing in for the FM
 *    receiver) streams to the headset and is muted while keyed.
 */

#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "sco_audio.h"
#include "ws6_ptt.h"

// Onboard LED on most ESP32 devkits; lit while keyed.
#define LED_GPIO GPIO_NUM_2

// Radio PTT drive, open-drain: low = keyed, high-Z = released, so the pin never
// sources current into the radio. Not a strapping pin, and high-Z through
// reset/boot, so the radio can't key while the ESP32 starts.
#define RADIO_PTT_GPIO GPIO_NUM_4

// 1: stream the commentary clip over SCO. 0: silence (PTT only).
#define COMMENTARY 1

// The WS Alert XP headset.
#define HEADSET_ADDR "00:15:F3:25:7D:E8"

static void on_ptt(bool keyed)
{
    gpio_set_level(RADIO_PTT_GPIO, keyed ? 0 : 1);
    gpio_set_level(LED_GPIO, keyed ? 1 : 0);
#if COMMENTARY
    sco_audio_mute(keyed);  // FM commentary off while transmitting
#endif
}

void app_start(void)
{
    // Latch "released" (high-Z) before enabling the open-drain output so the
    // PTT line never glitches low.
    gpio_set_level(RADIO_PTT_GPIO, 1);
    gpio_config_t ptt_io = {
        .pin_bit_mask = 1ULL << RADIO_PTT_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&ptt_io);

    gpio_config_t led_io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_io);
    on_ptt(false);

    ws6_ptt_config_t cfg = WS6_PTT_CONFIG_DEFAULT;
    cfg.headset_addr = HEADSET_ADDR;

    ws6_ptt_hooks_t hooks = {
        .on_ptt = on_ptt,
    };
#if COMMENTARY
    sco_audio_init();
    hooks.on_audio_start = sco_audio_start;
    hooks.on_audio_stop = sco_audio_stop;
    hooks.sco_fill = sco_audio_fill;
#endif

    ws6_ptt_init(&cfg, &hooks);
}
