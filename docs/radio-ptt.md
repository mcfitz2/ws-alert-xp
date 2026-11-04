# Radio push-to-talk: what the stock headset does

Notes from probing a stock WS ALERT XP (MRX21A2WS6) to see whether it can key a
two-way radio with a Kenwood K1 plug without 3M's FL6077-WS5 Bluetooth adapter.
Short answer: not yet. The headset has a radio mode, but it only turns it on
for a device that passes a 3M-proprietary handshake we haven't cracked.

Tools are in `tools/peltor_re/`. They ran on a Debian 13 laptop with BlueZ 5.82.

## The button

There's no dedicated PTT button. Once a radio adapter is connected, the
Bluetooth button is PTT: hold to transmit, release to stop. The manual marks
XPI-only features with `**`; the single radio connection isn't marked, so the
XP should support it (radio *or* phone, not both at once).

## What the headset advertises (SDP)

Address `00:15:F3:25:7D:E8` (OUI belongs to PELTOR AB), class `0x240404`,
name `WS ALERT XP`.

| Service | RFCOMM channel |
|---|---|
| Serial Port "SPP Dev" | 1 |
| Hands-Free (HF unit) | 10 |
| Headset (HS) | 11 |
| A2DP sink, AVRCP target + controller | L2CAP |

The SPP channel accepts a connection but ignored plain-text AT commands. It may
speak a binary protocol, or nothing at all.

## HFP handshake

With a phone (or PipeWire), the headset runs a normal HFP service level
connection, plus one CSR vendor command:

```
AT+BRSF=665  AT+BAC=1,2  AT+CIND=?  AT+CIND?  AT+CMER=3,0,0,1
AT+BCS=2  AT+BTRH?  AT+BIA=0,1,1,1,0,0,0,0  AT+CSRSF=0,0,0,0,0,0,3
```

**Only on the first connection after pairing**, about 2 s after the SLC is up,
it sends a short burst of undocumented commands:

```
AT+CTKST=0,0
AT+VOLOUT?
AT+CTKST=0,1
AT+CTKST=0,0      (about 2.1 s after the first)
```

Nothing public mentions `CTKST` or `VOLOUT`. Our reading is that this burst is
the headset asking "are you a 3M radio adapter?" Getting no answer it
recognises, it settles into phone mode for good (it doesn't ask again on later
connections; you have to re-pair to get another try).

## What we tried (`hfp_ag.py` as a fake Audio Gateway)

| Experiment | Result |
|---|---|
| Reply `ERROR` to the burst (default) | Phone mode |
| Reply `OK` to the burst | Phone mode |
| Reply `+CTKST: 0,1` / echo-style confirmations | Phone mode |
| Change our address to a PELTOR OUI (`00:15:F3:A1:B2:C3`) and name to `WS ADAPTER` | Phone mode; it doesn't check address or name |
| Hold an SCO audio link open, then press/hold buttons | No PTT command |

In phone mode the Bluetooth button only ever sends standard HFP: long hold is
`AT+BLDN` (redial), short press during a call is `AT+CHUP`. Nothing PTT-shaped
shows up, because the headset never decided it was talking to a radio.

## Volume, while we were at it

The headset's volume buttons change FM and ambient (surround) volume locally.
It never sends `AT+VGS`, and it rejected our AVRCP absolute-volume request.
That's why it doesn't change the phone's volume during music playback. The
replacement board can do better: the BM83 supports AVRCP absolute volume.

## The 3M Connected Equipment app (com.mmm.csce)

Decompiled for clues. The app doesn't detect this headset (it's older than the
2022 models it supports), but its command set shows that 3M's firmware has a
full PTT state machine:

- Control runs over BLE GATT, not the HFP channel. Vendor service `0xFE53`,
  characteristics `FE53AA1x-02D0-334D-B424-0002A5D5C51B` (Qualcomm GAIA style).
  Peltor command category byte `0x17`.
- Commands include `PTT_REQUEST` (0xA1), `GET_PTT_CONFIG_DATA` (0xA2),
  `SET_PTT_CONFIG_DATA` (0xA3), `SET_PTT_AUDIO_DATA` (0xA5),
  `GET_RADIO_STATE` (0xB0), `REQUEST_RADIO` (0xB1).
- `PttRequest`: RECONNECT 0, PTT_ON 1, PTT_OFF 2, DISCONNECT 3.
- `PttState`: IDLE 0, RECONNECTING 1, CONNECTED 2, ACTIVE 3, DISCONNECTING 4.
- Paired device types: UNKNOWN 0, PHONE 1, PTT 2. So the headset records each
  paired device as a phone or a radio, which fits the one-shot handshake above.

The APK contains no firmware. Updates are downloaded at runtime from
`/api/deviceupdates/productmodel/{model}` and only reach newer models. The PC
PELTOR Service Tool needs a license from 3M.

## Newer-model firmware (WS ALERT XP+ / XPV)

The app's OTA server (`cs-ps-device-prod-01.azurewebsites.net`, static API key
in the app) lists firmware for the XP+ (`MRX21A2WS7`, our model's successor),
XPI+ and XPV. Several builds are published unencrypted (`*-dfu.bin`, as
opposed to `*-dfu-encrypted.bin`). They're Qualcomm QCC514x DFU images
(`APPUHDR5`, `PARTDATA` chunks); the customer application is partition 9.

XP+ 1.1 has no radio strings. XP+ 1.2.0 and XPV 1.3.0 have a two-way radio
module, `bttwr.c`, next to `peltor.c`, `tetra.c` (`+MCCFG`, `+MCKEY`,
`+MCAUTH`), and `bt3mclient.c` / `bt3mserver.c`. Offsets below are in
`MRX21A2WS7-1.2.0-dfu.bin`; file offset = address − 0x75E8AFAC.

Strings the headset *sends* (`bttwr.c`, `peltor.c`):

```
AT+CIND=?   AT+CIND?   AT+CMER=3,0,0,1   AT+VOLOUT?   AT+CTKST=%d,%d
```

The no-space `AT+CMER=3,0,0,1` differs from the stock HF library's
`AT+CMER=3, 0, 0, 1`, which identifies the third SLC in our captures as the
radio module's own setup.

Strings the headset *parses* (table at 0x2266d0, string → id):

| Prefix | id |
|---|---|
| `+CTKST=` | 1 |
| `+VOLOUT?` | 2 |
| `+VOLOUT:` | 3 |
| `+VOLOUT=` | 3 |

Note `+CTKST=` with an equals sign, not the usual `+CTKST:`. Every reply we
had tried used a colon.

The module's state machine (descriptor at 0x226858: 37 transitions at
0x22672c, a 14-entry parent table at 0x22671b, 15 action handlers). Records
are `from, to, 0, 0, event (u32 LE)`; reading the 14-entry array as each
state's parent gives:

```
0 root
├─ 1
└─ 2  link up; HFP library events 0x0204xxxx
   ├─ 3  idle (phone behaviour)          0x05010016 → 6
   └─ 4  radio active; the only state that takes button events 0x0016000x
      ├─ 5 → 6 → 7 → 8 → 9               steps on 0x05010019 (the four setup commands)
      └─ 10/11 → 12 → 13                 steps on 0x05010018; 13 → 3 on 0x0501001a
```

That matches the capture: setup commands, `CTKST` 0,0 / 0,1, then roughly 2 s
later a drop back to state 3, after which the Bluetooth button acts like a
phone button again. Whatever keeps the module in state 4 is what the radio
adapter sends, most likely a `+CTKST=` line.

## Accepted reply grammar (from the XP+ firmware data)

The radio module's parse table (0x2266d0, string → id) is the complete set of
lines the headset accepts back from a radio:

| Token | id |
|---|---|
| `+CTKST=` | 1 |
| `+VOLOUT?` | 2 |
| `+VOLOUT:` | 3 |
| `+VOLOUT=` | 3 |

So the radio's whole reply vocabulary is `+CTKST=a,b` and `+VOLOUT:n`
(`+VOLOUT=n` / `+VOLOUT?` variants). No other token is recognised.

## Disassembly: Kalimba, now readable (kobjdump)

The QCC5144 "Apps" images are a **Kalimba DSP core**, not ARM. A GNU binutils
2.18 QTIL build of objdump for `elf32-littlekalimba` (`kobjdump.exe`, from the
openaptx project) decodes them. Run under wine on the laptop; it reports the
sample objects as architecture `QCC517x_audio` and disassembles cleanly.

Partition map of `MRX21A2WS7-1.2.0-dfu.bin` (DFU `PARTDATA` chunks, 16-byte
header, big-endian part id in the last header word):

| Part | File body | Contents |
|---|---|---|
| 5 | 0x2ef8–0x92a68 | code image (where the handler addresses point) |
| 6, 7 | 0x92a78 / 0xc4060 | "File"-magic read-only filesystem blobs |
| 9 | 0x175054–0x22be68 | radio module: code head + **all the DATA** (strings, parse table, state machine) |

Partition 9's tail holds the data we need and maps to runtime addresses by
`runtime = 0x78000000 + (p9 file offset − 0x175054)`. The parse table, parent
array and transition table were all read straight out of it (below).

Minim (16-bit) Kalimba decodes too. `kobjdump -b binary` only ever decodes
maxim (32-bit); minim is a per-section mode keyed off the section flag
`0x50000000` (name suffix `__minim`). Wrapping a raw partition in a hand-built
ELF32 (machine `0xdb`, one PROGBITS section, flags `0x50000006`) makes kobjdump
report the section `(MINIM)` and disassemble it cleanly — verified against the
sample `aptXAdaptiveEncode.po`, which comes out as readable functions with
`pushm <…rLink>` prologues and resolved jump targets. Wrapper script and ELFs
are in the scratchpad (`wrap.py`, `p5_minim.elf`, `p9_minim.elf`).

Still blocked: **locating the handler functions**. The state-machine
descriptor (p9 data 0xb1810+) holds a contiguous array of PM entry addresses
— `0x10f44, 0x574c5, 0x574e3, 0x574e5, 0x574e7, 0x57683, 0x578c5, 0x579d3,
0x57a7b, 0x57b55, …` (odd = minim tag bit 0). The deltas between the larger
ones (0x19c, 0x242, 0x10e, 0xa8, 0x80, 0x40, …) read like consecutive function
sizes, so it is a table of handler entry points. But these PM addresses do not
map to partition 5 or 9 by any constant offset (brute-forcing a base that lands
them all on `pushm` prologues fails — best 3/11). The DFU image is stripped of
symbols and relocations, and its PM link base isn't derivable from the
partition headers we parsed. So the exact `+CTKST=` accept test still can't be
read without the PM memory map (QCC514x curator/partition load addresses) or a
real-adapter capture.

Confirmed from the `PARTDATA` headers (`"PARTDATA"` + u32be len + u32be id):
partitions **5 and 9 both begin with the identical Kalimba reset preamble**, so
both are full PM code images (processors P0 and P1); the radio module runs on
P1 = partition 9, whose RAM is aliased as PM (low addresses) and DM
(`0x78000000+`). The handler entry addresses still don't resolve to prologues
at offset 0 of either partition, nor at any single base (tried base
brute-force, prologue alignment, ×2 word-addressing — all ≤3/15). Path 1 is
exhausted without the curator load-map.

## State machine (read from partition 9 data)

Descriptor at p9 data 0xb1804: 37 transitions, 14 states, parent array
`[255,0,0,2,2,4,5,5,5,5,4,4,4,4]`, action-handler pointers
`0x574c5, 0x574e3, 0x574e5, 0x574e7, 0x57683, 0x578c5, 0x579d3` (into
partition 5). The 37 transitions (`from, to, event u32 LE`):

```
state 3 (idle/phone)  --0x05010016-->  state 6   enter radio-active subtree
state 6 --0x05010019--> 7 --0x05010019--> 8 --0x05010019--> 9    four setup steps
state 4 (radio active) --0x05010017--> state 3   the ~2.1 s drop back to phone
state 4 --0x001600xx--> 4             button events (PTT) while radio active
state 13 --0x0501001a--> 3            later teardown path
```

Parent array puts states 5,6,7,8,9,10,11,12,13 under state 4, so reaching any
of them *is* "radio active". The gate is therefore: make the module emit
`0x05010019` (setup step accepted) for each of the four setup commands instead
of `0x05010017` (abort). `0x05010019` vs `0x05010017` is decided by the minim
handler code when it validates each reply.

The four setup commands the headset sends (firmware send-strings, `bttwr.c` /
`peltor.c`): `AT+CMER=3,0,0,1` (no space), `AT+VOLOUT?`, `AT+CTKST=0,0`,
`AT+CTKST=0,1`. Each needs a reply the handler accepts.

## Blind guesses tried (in the handshake window, via `--resp`)

| Reply to `AT+CTKST=0,0` / `=0,1` | Result |
|---|---|
| `ERROR` | phone mode |
| `OK` only | phone mode |
| `+CTKST: 0,1` (colon) | phone mode |
| `+CTKST=0,0` / `+CTKST=0,1` (echo) | phone mode |
| `+CTKST=1,0` / `+CTKST=1,1` | phone mode |

Unsolicited `+CTKST=` lines injected *after* the window get no reaction at
all. The radio-vs-phone decision is made only inside the ~2.1 s window, and
the headset never re-evaluates. In every case the button then sends `AT+BLDN`
(phone redial), never a PTT command.

## Untested lead: complete all four setup steps, not just CTKST

> Superseded: the real `+VOLOUT: n` reply was supplied in fire2–fire4 and did
> not change the outcome on its own. The actual gate was the SCO audio link —
> see "SOLVED (fire4)" below.


Every blind guess so far varied only the reply to `AT+CTKST=0,0` / `=0,1`. But
the state machine advances one step per *accepted* setup reply, and the second
step is `AT+VOLOUT?` — which we only ever answered with `OK`, never a real
`+VOLOUT: n`. If that step is rejected, the module aborts (`0x05010017`) before
CTKST is even reached, which would explain every "phone mode" result.

Next live test: reply to the whole burst as a radio would —
`AT+CMER=3,0,0,1` → `OK`, `AT+VOLOUT?` → `+VOLOUT: 4` then `OK`,
`AT+CTKST=0,0` → `OK`, `AT+CTKST=0,1` → `OK` (and a variant echoing
`+CTKST=0,1`). Cheap: one re-pair. This needs no further disassembly.

Also still open: `AT+CSRSF=0,0,0,0,0,0,3` is answered only with `OK`; a real
CSR-to-CSR link would send its own `+CSRSF: ...`. The radio module's parse
table has no `+CSRSF:` entry, so this is a weak lead.

## Live tests done (fire2, fire3): reply content is ignored

Two fresh-pairing live tests fed the burst a real `+VOLOUT: 8` and varied the
CTKST reply, with the AG owning the link (PipeWire bluez monitor off):

- **fire2** — echo `+CTKST=0,1`, then fire an unsolicited `+CTKST=1,1`
  immediately after. Window 2.148 s, then fall-back `AT+CTKST=0,0`.
- **fire3** — replace the echo: answer `AT+CTKST=0,1` with `+CTKST=1,1` only.
  Window 2.156 s, then fall-back `AT+CTKST=0,0`.

Both dropped. The decisive observation: **the fall-back fires at a fixed
~2.15 s after `AT+CTKST=0,1` in every run, identical regardless of reply
content.** So the module is *not* gating on the CTKST reply value at all — it
enables talk-key, opens a fixed 2.15 s window waiting for some condition our AT
replies do not affect, times out, and disables. After the drop the BT button
emits nothing over HFP (not even `AT+BLDN`); the link stays connected but
silent.

The one condition a real FL6077-WS5 supplies that we had never supplied is a
**SCO/eSCO audio link**. The real adapter opens audio when the radio keys.

## SOLVED (fire4): SCO audio is the gate; the button then reports PTT as CTKST

fire4: same clean handshake (real `+VOLOUT: 8`, plain `+CTKST=` echoes), but
immediately after `connect` succeeded, `sco.py` opened a CVSD SCO link and held
it. Result, decisively different from every no-audio run:

- The setup burst **did not run the enable/timeout probe at all.** Instead of
  `CTKST=0,0 → VOLOUT? → CTKST=0,1 → (2.1 s) → CTKST=0,0`, it went
  `CTKST=0,0 → VOLOUT? → CTKST=0,0 → CTKST=0,0` — straight to a stable
  talk-key-idle state. No 2.1 s fall-back, because there was nothing to abort.
- With the headset now in radio mode, the **physical Bluetooth button reports
  PTT over HFP as talk-key state**: press → `AT+CTKST=0,1`, release →
  `AT+CTKST=0,0`, cleanly, every cycle. (In the no-SCO run fire3 the same
  button emitted nothing at all.)

So the gate for radio mode is an **active SCO audio link**, not any AT reply
value — which is exactly why every reply-content guess failed and the 2.1 s
drop was invariant: the module was waiting for audio, timing out, and
disabling. Supply the audio and it stays in radio mode, and the button becomes
a standard `AT+CTKST` PTT line.

This fully reproduces the FL6077-WS5 behaviour with the **stock** headset. No
custom board, no firmware patch, no $200 adapter.

### Recipe for a cheap K1-plug dongle

1. Act as an **HFP AG** to the headset (the `tools/peltor_re` script stack is a
   working reference).
2. Answer the SLC handshake; answer `AT+VOLOUT?` with a real `+VOLOUT: n`
   (not bare `OK`); echo `AT+CTKST=` back.
3. **Open a CVSD SCO link as soon as the SLC is up** and hold it. This is the
   gate that puts the headset into radio mode.
4. The button now sends `AT+CTKST=0,1` on press and `AT+CTKST=0,0` on release.
   Drive the radio's K1 PTT line from those events.
5. Bridge SCO audio: headset mic → radio TX audio, radio RX audio → headset
   speaker.

A small BT-HFP + K1 dongle (ESP32/nRF-class radio with an HFP-AG stack, or a
headset-SoC module) replaces the FL6077-WS5. Component sourcing per the
project's JLC/DigiKey rule; this is radio-side, not a headset board.

### ESP32 implementation (`firmware/esp32-ptt/`)

A classic ESP32 (BR/EDR — ESP32-D0WD etc.; **not** S2/S3/C3/C6, which have no
Bluetooth Classic) can run the whole AG side with ESP-IDF's Bluedroid HFP-AG:

- `ESP_HF_UNAT_RESPONSE_EVT` delivers the headset's proprietary AT commands
  (`AT+VOLOUT?`, `AT+CTKST=`, `AT+CSRSF`) to the app as raw strings, and
  `esp_hf_ag_unknown_at_send()` sends the raw replies back — so the exact
  laptop recipe ports directly.
- `esp_hf_ag_audio_connect()` opens the SCO link right after
  `ESP_HF_CONNECTION_STATE_SLC_CONNECTED` — the radio-mode gate.
- Incoming `AT+CTKST=0,1` / `=0,0` toggle a GPIO.

Phases: **A** mirror the talk-key to the onboard LED (no radio, no K1 plug —
protocol proof on bare hardware); **B** point the GPIO at a transistor across
the radio's K1 PTT line; **C** add an I2S/PCM codec to bridge SCO audio to the
radio's mic/speaker. Built with PlatformIO (`pio run -t upload`).

#### Bring-up result: Bluedroid stalls the SLC the WS6 needs

Phase A got far on a classic ESP32 (ESP32-D0WD-V3) but **does not complete the
service-level connection with the WS6**, and the cause is in ESP-IDF's
Bluedroid HFP-AG, not the wiring:

- The ESP32 AG pairs fine (SSP just-works, `auth ok`), opens RFCOMM to the
  headset's HF channel (scn 10), and answers the first two SLC commands:
  `AT+BRSF` → `+BRSF:` and `AT+CIND=?` → the 132-byte indicator-range list.
- The headset then sends a **third command that Bluedroid neither answers nor
  forwards to the app** (no `PORT_WriteData`, no `ESP_HF_UNAT_RESPONSE_EVT`).
  The SLC never reaches `SLC_CONNECTED`; after a fixed ~5 s SLC timer Bluedroid
  tears the link down (`port_rfc_closed ... res: 19`, `rsn 0x16`), then redials
  and loops. Verified with RFCOMM/HCI traces at DEBUG/VERBOSE.
- Bluedroid only delivers *unknown* AT commands (`AT+VOLOUT?`, `AT+CTKST=`,
  `AT+CSRSF`, …) to the app **after** the SLC is up. The WS6 injects a
  proprietary command *during* SLC setup and waits for a reply, so Bluedroid's
  post-SLC-only `unknown_at` hook can never service it. The BlueZ laptop AG
  worked precisely because its profile answered *every* AT line, mid-SLC
  included.
- The WS6 never initiates (it is always the acceptor, even in pairing mode), so
  "let the headset drive the SLC" is not an option — the AG must drive it, and
  Bluedroid's AG can't answer the mid-SLC proprietary command.

Net: the ESP32 **radio/BR-EDR hardware is fine**; the blocker is the Bluedroid
host stack. To finish the free-ESP32 path, use a host that exposes raw AT
handling during SLC — **BlueKitchen BTstack** (its HFP-AG has per-command AT
hooks and runs on the classic ESP32) is the natural port, or patch Bluedroid's
`bta_ag` to forward/ack unknown AT before `SLC_CONNECTED`. The proven recipe
(answer `AT+VOLOUT?`, echo `AT+CTKST=`, open CVSD SCO right after SLC) is
unchanged; only the stack that carries it must let the app answer every line.

### BTstack implementation (`firmware/esp32-btstack-ptt/`): Phase A works

The same classic ESP32 running BlueKitchen BTstack's HFP-AG completes the SLC,
the headset runs its radio-mode probe, and the button drives GPIO4
(`AT+CTKST=0,1` → keyed, `AT+CTKST=0,0` → released; every press paired with
a release over dozens of presses). What it takes:

- `AT+BRSF` features 3680, CVSD only.
- `AT+CSRSF` → **bare `OK`**. The full `+CSRSF:` line that the BlueZ AG sent
  leaves the headset idle under BTstack.
- `AT+VOLOUT?` → `+VOLOUT: 8` then `OK`; `AT+CTKST=a,b` → echo `+CTKST=a,b`
  then `OK`.
- **Frame every custom reply as `\r\n…\r\n` and keep it in static storage.**
  `hfp_ag_send_unsolicited_result_code()` keeps the pointer and sends the
  string raw, with no CRLF added. Unframed replies made the headset re-poll
  `VOLOUT?`/`CTKST` forever (button never armed), and an unframed `+CSRSF:`
  line shut the headset off.

With that, a normal power-on reconnects and runs the probe with no re-pairing.
SCO must be eSCO EV3 (`SCO_PACKET_TYPES_EV3`); HV3 negotiates a zero packet
length and the link dies at 20 s.

#### The headset types each AG on its first session (fire28-fire42)

An earlier version of this section said "SCO is not needed for PTT". That
only holds for an AG the headset has *already* accepted as a radio. The XP
firmware keeps a per-device type (UNKNOWN / PHONE / PTT, see the XP+ notes
above) and sets it during that device's first session:

- **SCO up at SLC up** (`OPEN_SCO 1`, before the radio module sends
  `AT+CSRSF`): the probe runs `CTKST=0,0 → VOLOUT? → CTKST=0,0 → CTKST=0,0`
  and the device is stored as PTT. On later sessions the probe passes with or
  without SCO, and the button sends `CTKST=0,1` / `0,0`.
- **No SCO yet:** the probe takes the abort path
  (`VOLOUT? → CTKST=0,1 → (2.1 s) → CTKST=0,0`) and the device is stored as
  PHONE. From then on that address gets no probe at all (setup stops after
  `AT+CSRSF`) and the button sends nothing.

What does **not** recover a PHONE-typed address: headset power cycles, battery
out, erasing the ESP32's NVS (fresh SSP keys on the same address), the
headset's menu factory reset, or answering the probe's `0,1` with `+CTKST=0,0`.
Opening SCO inside the 2.1 s window (fire41) or after the probe is also too
late. The only reset found is a new Bluetooth address: `BT_ADDR_OVERRIDE` in
`src/main.c`, with the headset in pairing mode.

fire28 was misread at the time as "SCO before the probe kills radio mode". In
fact that address was already typed PHONE (the radio module never sent its
`AT+CIND=?`), so SCO had nothing to do with it. That misreading moved SCO
after the probe and typed every fresh address after it as a phone.

The BTstack patch also answers the radio module's own post-SLC `AT+CIND=?`
(stock BTstack ignores it after the SLC, which costs a 5 s stall before
`CIND?` / `CMER=3,0,0,1`). This is correct but not sufficient on its own;
fire40 answered it and was still typed PHONE without SCO.

Working first-session recipe (fire42, commit 22e5872): fresh address, NVS
erased, headset in pairing mode, `OPEN_SCO 1`, CVSD. The commentary clip
plays over SCO, the button keys GPIO4, and both survive a headset power cycle.

## Radio side: Baofeng UV-5G (K1 connector)

The target radio is a Baofeng UV-5G (GMRS) with the Kenwood-style 2-pin K1
jack: a 3.5 mm and a 2.5 mm plug. Mic and PTT are on the 3.5 mm plug; speaker
and ground are on the 2.5 mm plug (measured on the donor cable below).

| Plug   | Tip            | Ring  | Sleeve |
|--------|----------------|-------|--------|
| 3.5 mm | (yellow, cut?) | mic + | PTT    |
| 2.5 mm | speaker +      | –     | ground |

The radio transmits when the 3.5 mm sleeve (PTT) is pulled to the 2.5 mm
sleeve (ground).

### Donor cable: Baofeng shoulder mic

A K1 cable salvaged from a Baofeng shoulder speaker-mic. Wire colours as
soldered to the shoulder-mic PCB, with the PCB pad labels and the measured K1
contact:

| Wire        | PCB label | K1 contact                         |
|-------------|-----------|------------------------------------|
| White       | Mic       | 3.5 mm ring                        |
| Red         | PTT       | 3.5 mm sleeve                      |
| Black       | GND       | 2.5 mm sleeve                      |
| White/green | SP        | 2.5 mm tip                         |
| Yellow      | (none, cut off at the PCB) | not yet traced; probably 3.5 mm tip |

With the wires still on the PCB, the speaker coil (~8 Ω) links SP and GND, so
on beep-continuity both appear on both 2.5 mm contacts. On the resistance
range: SP–tip 0 Ω, SP–sleeve 7 Ω, GND–sleeve 1 Ω, GND–tip 7 Ω (the 7 Ω is
the speaker coil).
PTT pull-up measured: with both plugs in and the radio on, Red-to-Black reads
**3.3 V DC**. Shorting Red to Black keys the transmitter (verified on the UV-5G).

Phase B drive (direct): ESP32 GPIO4 → Red (PTT), ESP32 GND → Black (GND).
GPIO4 is open-drain (low = keyed, high-Z = released), so it never sources
current into the radio's 3.3 V line, and it is high-Z through reset so the
radio can't key at boot. GPIO2 (onboard LED, a strapping pin) is only the
indicator. A 60 s TX timeout forces release if a `CTKST=0,0` is lost. If the
grounds ever need isolating, swap in a PC817: GPIO → 470 Ω → LED → GND,
collector → Red, emitter → Black. Phase C uses White (mic) and White/green
(speaker) against Black.

## Other commands, and why "push-to-listen" is not reachable over Bluetooth

Full enumeration of the AT/`+` tokens in partition 9 (the radio module):

- The module **sends** (HF role): `AT+CTKST=%d,%d` (talk-key), `AT+VOLOUT?`
  (query output volume), the CSR set `AT+CSRSF/CSRBATT/CSRGETSMS/CSRPWR`, plus
  standard HFP.
- The module **parses from the AG**: `+CTKST=`, `+VOLOUT:`, `+VOLOUT=`,
  `+VGS:` (speaker gain), `+VGM:` (mic gain), `+CIEV:` (indicator events),
  `+BVRA:` (voice-rec), standard HFP results.

So an adapter can, in principle, set BT/radio **audio** volume (`+VGS`,
`+VOLOUT=`), mute the stream, fake call indicators (`+CIEV:`), or trigger voice
recognition (`+BVRA: 1`). A live idle-state test of `+VGS`/`+VOLOUT=` produced
no audible or control-channel effect (headset not in a call), so these likely
need an active call/SCO state to take hold — untested further.

The firmware symbol/log strings expose an internal **`bttwr`** ("Bluetooth
two-way radio") subsystem matching the live behaviour: `bttwrPttOnAny` /
`bttwrPttOffAny`, `bttwrSet/GetVolume`, `bttwrSet/GetMode`,
`bttwrGet/SetBtButtonConfigData`, `bttwrGet/SetSideToneConfig`,
`bttwrGetVoiceSetting`, `bttwrGetRoutedDeviceId`. These are internal C calls
driven by the HFP handshake, not free-form remote commands.

**Push-to-listen (momentarily boost the ambient / level-dependent hear-through
so you can hear your surroundings) is not reachable over the Bluetooth link.**
The firmware has *no* `ambient` / `passthrough` / `hear-through` / `surround`
symbol — only `sidetone` (own-mic-to-own-ear, not ambient). The
level-dependent surround is a separate always-on hearing-protection path,
independent of the QCC Bluetooth SoC, so no BT command can address it. (An
apparent live "ambient turned down" during the volume test was the headset
auto-powering off — the link went to "Host is down".)

### App / SPP channel

The headset advertises Serial Port (`0x1101`); the 3M Connected app rides this
classic RFCOMM channel (channel 1), not BLE — the device exposes no GATT/LE
service UUIDs at all. Channel 1 accepts a connection but stays **silent** to
GAIA v1 frames, the 3M `dsrl` framings, and plain AT/text (`spp_probe.py`,
all sets). Reversing the app protocol would require a btsnoop capture of the
real 3M Connected app talking to a headset; it is unlikely to expose ambient
control, since that path is not in this firmware.
