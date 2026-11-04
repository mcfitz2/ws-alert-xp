# ws-alert-xp

`ws6_ptt` is an ESP32 library that turns a stock 3M PELTOR WS ALERT XP (WS6)
Bluetooth headset's button into a push-to-talk key. It speaks the headset's
proprietary radio-mode protocol, which was reverse engineered for this
project, and leaves the rest (keying a radio, audio) to your code through
hooks.

Not affiliated with or endorsed by 3M.

## Layout

- **`ws6_ptt/`**: the library (an ESP-IDF component on top of BlueKitchen
  BTstack).
- **`examples/basic/`**: connect to the headset and print PTT presses.
- **`examples/radio_combo/`**: keys a Baofeng UV-5G through GPIO4 and streams
  an audio clip to the headset over SCO, muted while transmitting.
- **`docs/radio-ptt.md`**: the protocol (SDP, the HFP handshake, the
  proprietary AT commands, the radio-mode probe) and every live test that
  found it.
- **`tools/peltor_re/`**: Bluetooth reverse-engineering tools for Linux/BlueZ
  (fake HFP AG, SCO, SPP probes).
- **`patches/`**, **`tools/setup_btstack.sh`**: the pinned BTstack checkout
  and the small patch it needs.

## Requirements

- A classic ESP32 (ESP32-D0WD and similar). The S2, S3, C3 and C6 have no
  Bluetooth Classic.
- PlatformIO (ESP-IDF 5.3).

## Quick start

```sh
tools/setup_btstack.sh     # clones BTstack into btstack/ and applies the patch
cd examples/basic
# set HEADSET_ADDR in src/main.c
pio run -t upload -t monitor
```

Put the headset in pairing mode for the first connection. The ESP32 dials
the headset (it never connects on its own), opens audio, and the headset
runs its radio-mode handshake. After that, the button prints
`PTT pressed` / `PTT released`.

**Read this before the first pairing.** The headset remembers every
Bluetooth address it pairs with as either a phone or a radio, permanently.
The library opens audio early enough that a new address is typed as a
radio. But if an address was ever typed as a phone (for example by other
firmware on the same ESP32), the button stays silent for it, and neither
re-pairing nor a headset factory reset fixes that. Use a
fresh address instead (`BT_ADDR_OVERRIDE` in the example's `main.c`). See
"The headset types each AG on its first session" in `docs/radio-ptt.md`.

## API

```c
#include "ws6_ptt.h"

static void on_ptt(bool keyed) { /* key / unkey your radio */ }

ws6_ptt_config_t cfg = WS6_PTT_CONFIG_DEFAULT;
cfg.headset_addr = "00:15:F3:25:7D:E8";
ws6_ptt_hooks_t hooks = { .on_ptt = on_ptt };
ws6_ptt_init(&cfg, &hooks);      // after btstack_init(), before the run loop
```

Config (`WS6_PTT_CONFIG_DEFAULT` holds the tested values): `headset_addr`,
`name`, `offer_msbc` (16 kHz audio, default on), `volout_level`,
`tx_timeout_ms` (forced release, default 60 s), `redial_holdoff_ms`,
`class_of_device`.

Hooks (all optional, all called on the BTstack run loop):

| Hook | When |
|---|---|
| `on_ptt(bool keyed)` | Button pressed or released. Also `false` on link loss and TX timeout. |
| `on_state(state)` | `WS6_PTT_DISCONNECTED` → `CONNECTED` → `RADIO_MODE` (button armed). |
| `on_audio_start(codec)` / `on_audio_stop()` | SCO link up (`HFP_CODEC_CVSD` 8 kHz or `HFP_CODEC_MSBC` 16 kHz) or down. |
| `sco_fill(payload, len)` | Fill one outbound SCO packet. Leave it `NULL` to send silence. |
| `sco_rx(packet, len)` | Inbound headset mic audio. |

`ws6_ptt_get_state()` and `ws6_ptt_is_keyed()` poll the same state.

Keying a transmitter needs the license for its band (e.g. GMRS in the US).

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md): free for personal, hobby,
research and other noncommercial use.

BTstack is not included; `tools/setup_btstack.sh` fetches it. It is
BlueKitchen GmbH's, under its own license (free for noncommercial use;
commercial use needs a license from BlueKitchen). `patches/btstack.patch` is
a modification of BTstack and falls under BTstack's license.
