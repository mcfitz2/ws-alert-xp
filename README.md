# ws-alert-xp

Reverse engineering of the 3M PELTOR WS ALERT XP (WS6) Bluetooth headset's
proprietary radio push-to-talk protocol, and ESP32 firmware that uses it to
key a two-way radio from the stock headset. Not affiliated with or endorsed
by 3M.

## What's here

- **`firmware/esp32-btstack-ptt/`** — ESP32 (classic, BR/EDR) firmware that
  pairs with a stock WS ALERT XP, puts it in its proprietary radio mode, and
  turns the headset's Bluetooth button into push-to-talk for a two-way radio
  (tested with a Baofeng UV-5G), while streaming audio to the headset over
  SCO. The Bluetooth side is a reusable library with hooks
  (`components/ws6_ptt`); `src/app.c` is the application. See
  [docs/radio-ptt.md](docs/radio-ptt.md) for the reverse-engineered protocol
  and how the recipe was found.
- **`firmware/esp32-ptt/`** — the earlier ESP-IDF Bluedroid attempt, kept for
  reference (Bluedroid can't answer the headset's mid-handshake AT command).
- **`docs/radio-ptt.md`** — protocol notes: SDP, the HFP handshake, the
  proprietary AT commands, the radio-mode probe, firmware disassembly notes
  and every live test (fire2-fire44).
- **`tools/peltor_re/`** — Bluetooth reverse-engineering tools for Linux/BlueZ
  (fake HFP AG, SCO, SPP probes).

## Building the PTT firmware

```sh
cd firmware/esp32-btstack-ptt
tools/setup_btstack.sh   # clones BTstack and applies patches/btstack.patch
pio run -t upload
```

Set the headset's Bluetooth address in `src/app.c` and read the "types each
AG on its first session" section of `docs/radio-ptt.md` before the first
pairing: get it wrong and that ESP32 address is stuck as a "phone".

Keying a transmitter needs the appropriate license for the band you use
(e.g. GMRS in the US).

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md): free for personal, hobby,
research and other noncommercial use.

BTstack is not included; `tools/setup_btstack.sh` fetches it. It is
BlueKitchen GmbH's, under its own license (free for noncommercial use;
commercial use needs a license from BlueKitchen). `patches/btstack.patch` is
a modification of BTstack and falls under BTstack's license.
