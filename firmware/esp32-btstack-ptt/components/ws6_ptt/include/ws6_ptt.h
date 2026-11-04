#ifndef WS6_PTT_H
#define WS6_PTT_H

/*
 * ws6_ptt: BTstack HFP Audio Gateway that puts a stock 3M Peltor WS Alert XP
 * (WS6) headset into its proprietary radio mode and reports its Bluetooth
 * button as a push-to-talk key.
 *
 * The library owns the whole Bluetooth side: GAP/SDP setup, dialing the
 * headset, the radio-mode handshake, the SCO link and the talk-key state. The
 * application supplies hooks for what to do with it (key a radio, feed audio).
 *
 * All hooks run on the BTstack run loop; call the library from there too.
 */

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"

typedef enum {
    WS6_PTT_DISCONNECTED,  // no service-level connection
    WS6_PTT_CONNECTED,     // SLC up, radio-mode probe not finished yet
    WS6_PTT_RADIO_MODE,    // probe done: the button now reports as PTT
} ws6_ptt_state_t;

typedef struct {
    // Talk key pressed (true) or released (false). Also called with false on
    // link loss and TX timeout.
    void (*on_ptt)(bool keyed);
    // Connection / radio-mode state changes.
    void (*on_state)(ws6_ptt_state_t state);
    // SCO audio link up (codec is HFP_CODEC_CVSD or HFP_CODEC_MSBC) / down.
    void (*on_audio_start)(uint8_t codec);
    void (*on_audio_stop)(void);
    // Fill one outbound SCO payload (CVSD: 16-bit PCM at 8 kHz; mSBC: encoded
    // stream bytes). NULL sends silence, which is enough to keep radio mode.
    void (*sco_fill)(uint8_t *payload, uint16_t len);
    // Inbound SCO packet from the headset mic (HCI SCO packet, 3-byte header).
    void (*sco_rx)(const uint8_t *packet, uint16_t len);
} ws6_ptt_hooks_t;

typedef struct {
    const char *headset_addr;    // e.g. "00:15:F3:25:7D:E8"
    const char *name;            // SDP service name and GAP name prefix
    bool offer_msbc;             // offer mSBC (16 kHz) besides CVSD (8 kHz)
    uint8_t volout_level;        // reply to the headset's AT+VOLOUT?
    uint32_t tx_timeout_ms;      // force release after this long keyed; 0 = off
    uint32_t redial_holdoff_ms;  // after an SLC drop, let the headset reconnect
    uint32_t class_of_device;
} ws6_ptt_config_t;

#define WS6_PTT_CONFIG_DEFAULT {          \
    .headset_addr = NULL,                 \
    .name = "WS-PTT",                     \
    .offer_msbc = true,                   \
    .volout_level = 8,                    \
    .tx_timeout_ms = 60000,               \
    .redial_holdoff_ms = 60000,           \
    .class_of_device = 0x200408,          \
}

// Set up BTstack for the headset and power on. Call once, after
// btstack_init() and before the run loop starts. cfg and hooks are copied.
void ws6_ptt_init(const ws6_ptt_config_t *cfg, const ws6_ptt_hooks_t *hooks);

ws6_ptt_state_t ws6_ptt_get_state(void);
bool ws6_ptt_is_keyed(void);

#endif
