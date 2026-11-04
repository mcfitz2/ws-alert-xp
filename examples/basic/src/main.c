/*
 * Minimal ws6_ptt example: connect to a WS Alert XP headset and print its
 * push-to-talk button. Put your own action (a GPIO, a message, ...) in
 * on_ptt().
 *
 * Before the first connection, put the headset in pairing mode. Read "The
 * headset types each AG on its first session" in docs/radio-ptt.md first.
 */

#include <stdbool.h>
#include <stdio.h>

#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"
#include "esp_mac.h"
#include "ws6_ptt.h"

// Your headset's Bluetooth address.
#define HEADSET_ADDR "00:15:F3:25:7D:E8"

// The headset remembers each Bluetooth address it has seen as either a phone or
// a radio, for good. If this ESP32's address was ever typed as a phone, the
// button stays silent; set 1 and pick an address the headset has never seen.
#define BT_ADDR_OVERRIDE 0
#if BT_ADDR_OVERRIDE
static const uint8_t bt_addr_override[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
#endif

static void on_ptt(bool keyed)
{
    printf("PTT %s\n", keyed ? "pressed" : "released");
}

static void on_state(ws6_ptt_state_t state)
{
    static const char *names[] = { "disconnected", "connected", "radio mode (button armed)" };
    printf("headset %s\n", names[state]);
}

void app_main(void)
{
#if BT_ADDR_OVERRIDE
    esp_iface_mac_addr_set(bt_addr_override, ESP_MAC_BT);
#endif
    btstack_init();

    ws6_ptt_config_t cfg = WS6_PTT_CONFIG_DEFAULT;
    cfg.headset_addr = HEADSET_ADDR;
    ws6_ptt_hooks_t hooks = {
        .on_ptt = on_ptt,
        .on_state = on_state,
    };
    ws6_ptt_init(&cfg, &hooks);

    btstack_run_loop_execute();
}
