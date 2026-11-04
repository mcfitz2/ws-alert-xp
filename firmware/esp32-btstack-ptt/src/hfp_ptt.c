/*
 * BTstack HFP Audio Gateway that drives radio push-to-talk from a stock
 * 3M Peltor WS Alert XP (WS6) headset.
 *
 * Why BTstack and not ESP-IDF's Bluedroid: the WS6 injects a proprietary AT
 * command *during* the service-level connection handshake and waits for a
 * reply. Bluedroid only forwards unknown AT to the app *after* the SLC is up,
 * so its AG stalls and the SLC times out (see docs/radio-ptt.md). BTstack lets
 * the app answer any AT command at any time: it replies ERROR to unknown
 * commands by default, and hfp_ag_register_custom_at_command() lets us answer
 * the headset's proprietary ones precisely, mid-SLC included.
 *
 * Recipe (proven on the BlueZ laptop): act as HFP AG, answer AT+VOLOUT? with a
 * real +VOLOUT value, echo AT+CTKST=, open a CVSD SCO link as soon as the SLC
 * is up. The headset then enters radio mode and reports the Bluetooth button as
 * AT+CTKST=0,1 (press) / AT+CTKST=0,0 (release).
 *
 * Phase A: LED_GPIO (onboard LED) follows the headset button.
 * Phase B: RADIO_PTT_GPIO pulls the radio's K1 PTT line (3.5 mm sleeve, 3.3 V
 * pull-up on the Baofeng UV-5G) to ground while the button is held. Wire it
 * straight to the PTT wire and tie ESP32 GND to the K1 ground (2.5 mm sleeve).
 *
 * A2DP_TEST: after the SLC is up, also open an A2DP (SBC) stream to the
 * headset playing test music, to check A2DP and HFP radio mode coexist.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "hci_dump_embedded_stdout.h"
#include "sco_demo_util.h"
#include "a2dp_src.h"
#include "sco_audio.h"
#include "driver/gpio.h"

// Onboard LED on most ESP32 devkits; lit while keyed.
#define LED_GPIO GPIO_NUM_2

// Radio PTT drive, open-drain: low = keyed, high-Z = released, so the pin never
// sources current into the radio. Not a strapping pin, and high-Z through
// reset/boot, so the radio can't key while the ESP32 starts.
#define RADIO_PTT_GPIO GPIO_NUM_4

// Force PTT off after this long keyed, in case a release (AT+CTKST=0,0) is
// lost. 0 disables.
#define TX_TIMEOUT_MS 60000

// Output volume reported to the headset's AT+VOLOUT? query.
#define VOLOUT_LEVEL 8

// fire4 (the laptop run where the button emitted AT+CTKST=0,1) had no SCO
// link: hfp_ag.py never opens one. Set to 1 to open SCO once the SLC is up.
#define OPEN_SCO 0

// SCO audio test: SCO_AUDIO_DELAY_MS after the radio-mode probe completes
// (AT+VOLOUT? then two AT+CTKST=0,0), open SCO and stream the embedded
// commentary clip (sco_audio.c) to the headset, muted while PTT is held.
// fire28 opened SCO (fixed 10 s after SLC up) before the probe finished, and
// fire35 had a button press mid-probe; both left the button dead. No complete
// probe -> no SCO. Logs the negotiated codec. With
// OFFER_MSBC the AG offers mSBC too; the WS6 sends AT+BAC=1,2, so BTstack
// picks mSBC (16 kHz, eSCO T1). 0 = CVSD only (8 kHz) for an A/B by ear.
#define SCO_AUDIO_TEST 1
#define SCO_AUDIO_DELAY_MS 2000
// 1: answer a CTKST=0,1 that arrives inside the probe with +CTKST=0,0.
#define PROBE_REPLY_RELEASED 1
#define OFFER_MSBC 1

// 1: answer AT+CSRSF with "+CSRSF: 1,1,1,1,1,0,3" then OK (fire4). 0: bare OK.
#define CSRSF_FULL_REPLY 0

// After an SLC drop, stop dialing this long so the headset can reconnect on
// its own. In fire4 the first connection dropped right after AT+CSRSF and the
// headset came back ~47s later, then ran the radio-mode probe.
#define REDIAL_HOLDOFF_MS 60000

// Open an A2DP stream this long after SLC up (after the radio-mode probe).
#define A2DP_TEST 0
#define A2DP_DELAY_MS 8000
#define A2DP_RETRY_MS 6000
#define A2DP_MAX_TRIES 6
// A2DP only: no HFP SDP record and no HFP dial, retry A2DP until streaming.
// With a fresh BT address (main.c) this checks whether the WS6 takes A2DP
// from a device it has never seen run the radio-mode handshake.
#define A2DP_ONLY 0
// Open A2DP right at boot, before the HFP dial, instead of after SLC up.
// fire14/15: with the SLC up the WS6 refuses AVDTP with L2CAP "no resources".
#define A2DP_FIRST 1
#define A2DP_FIRST_HFP_DELAY_MS 15000

// 0x200408 = Audio/Video, Hands-free device (what PTT was proven with).
// 0x5A020C = Phone, Smartphone with telephony/audio/capture/networking
// services: test whether the WS6 only takes A2DP from a phone-class device.
#define CLASS_OF_DEVICE 0x200408

// fire20-22: while A2DP streams the WS6 never arms the PTT button. Pause the
// stream PAUSE_AT_MS after SLC up and resume it PAUSE_FOR_MS later, to see if
// the button works with the stream suspended (squelch-gated A2DP).
#define A2DP_PAUSE_TEST 1
#define A2DP_PAUSE_AT_MS 20000
#define A2DP_PAUSE_FOR_MS 30000

// The WS Alert XP headset.
static const char *PEER_ADDR_STR = "00:15:F3:25:7D:E8";
static bd_addr_t peer_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t connect_timer;
static btstack_timer_source_t tx_timeout_timer;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco_handle = HCI_CON_HANDLE_INVALID;
static bool slc_connected = false;
static uint32_t slc_released_ms = 0;
static bool slc_was_up = false;

#if A2DP_TEST
static btstack_timer_source_t a2dp_timer;
static int a2dp_tries;

#if A2DP_PAUSE_TEST
static btstack_timer_source_t pause_timer;
static bool pause_test_paused;

static void pause_timer_handler(btstack_timer_source_t *ts)
{
    if (!pause_test_paused) {
        printf("[ptt] TEST: pausing A2DP - press the button now\n");
        a2dp_src_pause();
        pause_test_paused = true;
        btstack_run_loop_set_timer(ts, A2DP_PAUSE_FOR_MS);
        btstack_run_loop_add_timer(ts);
    } else {
        printf("[ptt] TEST: resuming A2DP - press the button again\n");
        a2dp_src_resume();
    }
}
#endif

// The WS6 refused the first AVDTP connect with L2CAP "no resources" (fire14),
// so keep retrying for a while until a stream is up.
static void a2dp_timer_handler(btstack_timer_source_t *ts)
{
    if ((!A2DP_ONLY && !slc_connected) || a2dp_src_streaming()) return;
    if (a2dp_tries++ >= A2DP_MAX_TRIES && !A2DP_ONLY) {
        printf("[ptt] A2DP: giving up after %d tries\n", A2DP_MAX_TRIES);
        return;
    }
    printf("[ptt] starting A2DP test stream (try %d)\n", a2dp_tries);
    a2dp_src_connect(peer_addr);
    btstack_run_loop_set_timer(ts, A2DP_RETRY_MS);
    btstack_run_loop_add_timer(ts);
}
#endif

// CVSD (codec id 1): the WS6 radio-mode handshake used narrow-band in the
// laptop capture. mSBC (id 2) only for the SCO audio test.
#if SCO_AUDIO_TEST && OFFER_MSBC
static const uint8_t codecs[] = { HFP_CODEC_CVSD, HFP_CODEC_MSBC };
#else
static const uint8_t codecs[] = { HFP_CODEC_CVSD };
#endif

#if SCO_AUDIO_TEST
static btstack_timer_source_t sco_timer;
// Radio-mode probe progress: 0 = waiting for VOLOUT?, 1..2 = VOLOUT? seen and
// that many CTKST since, 3 = complete.
static int probe_step;

static void sco_timer_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    if (!slc_connected || sco_handle != HCI_CON_HANDLE_INVALID) return;
    printf("[ptt] TEST: opening SCO audio (offer mSBC: %d)\n", OFFER_MSBC);
    hfp_ag_establish_audio_connection(acl_handle);
}
#endif

// Order and values match fire4's CIND exactly: service,call,callsetup,callheld,
// signal,roam,battchg with CIND? = 1,0,0,0,5,0,5. The WS6 keys radio-mode logic
// on indicator positions (our old order put battchg at 4 / callheld at 7, which
// gave AT+BIA=0,1,1,0,0,0,1,0 instead of fire4's 0,1,1,1,0,0,0,0).
static int ag_indicators_nr = 7;
static hfp_ag_indicator_t ag_indicators[] = {
    // index, name, min, max, status, mandatory, enabled, status changed
    {1, "service",   0, 1, 1, 0, 0, 0},
    {2, "call",      0, 1, 0, 1, 1, 0},
    {3, "callsetup", 0, 3, 0, 1, 1, 0},
    {4, "callheld",  0, 2, 0, 1, 1, 0},
    {5, "signal",    0, 5, 5, 0, 1, 0},
    {6, "roam",      0, 1, 0, 0, 1, 0},
    {7, "battchg",   0, 5, 5, 0, 0, 0},
};

static int hf_indicators_nr = 2;
static hfp_generic_status_indicator_t hf_indicators[] = {
    {1, 1},
    {2, 1},
};

static int call_hold_services_nr = 5;
static const char *call_hold_services[] = {"1", "1x", "2", "2x", "3"};

static uint8_t hfp_service_buffer[150];
static const uint8_t rfcomm_channel_nr = 1;
static const char hfp_ag_service_name[] = "WS-PTT";

// Proprietary commands the WS6 uses. The parser hands us the command token with
// its trailing separator, so register the exact prefixes. The INFO log line
// "command string '...' -> cmd id ..." prints every token the headset sends, so
// unknown forms show up there and ERROR is returned until we handle them.
enum { ID_VOLOUT = 1, ID_CTKST, ID_CSRSF, ID_BLDN };
static hfp_custom_at_command_t cmd_volout = { .command = "AT+VOLOUT?", .command_id = ID_VOLOUT };
static hfp_custom_at_command_t cmd_ctkst  = { .command = "AT+CTKST=",  .command_id = ID_CTKST };
static hfp_custom_at_command_t cmd_csrsf  = { .command = "AT+CSRSF=",  .command_id = ID_CSRSF };
// AT+BLDN (redial) shows up now and then; register it so BTstack doesn't act
// on it as a redial request.
static hfp_custom_at_command_t cmd_bldn   = { .command = "AT+BLDN",    .command_id = ID_BLDN };

// Keep the SCO link alive by sending silence (zeros) on every can-send-now
// slot. We only need the SCO link UP to gate the WS6's radio mode; we do not
// bridge audio in Phase A. The sco_demo sine generator loaded the run loop and
// fed tone into the headset, which coincided with the WS6 faulting as SCO
// opened. A zero-filled payload is the lightest thing that keeps the baseband
// SCO flow fed so the controller never underruns.
static void sco_send_silence(hci_con_handle_t h)
{
    if (h == HCI_CON_HANDLE_INVALID) return;
    int sco_packet_length = hci_get_sco_packet_length_for_connection(h);
    int sco_payload_length = sco_packet_length - 3;

    hci_reserve_packet_buffer();
    uint8_t *sco_packet = hci_get_outgoing_packet_buffer();
    memset(&sco_packet[3], 0, sco_payload_length);
    little_endian_store_16(sco_packet, 0, h);
    sco_packet[2] = sco_payload_length;
    hci_send_sco_packet_buffer(sco_packet_length);

    hci_request_sco_can_send_now_event_for_con_handle(h);

    static uint32_t n = 0;
    if ((++n % 250) == 0) printf("[ptt] sco tx %lu\n", (unsigned long)n);
}

static void set_ptt(bool on);

static void tx_timeout_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    printf("[ptt] TX timeout (%d ms), forcing release\n", TX_TIMEOUT_MS);
    set_ptt(false);
}

static void set_ptt(bool on)
{
    gpio_set_level(RADIO_PTT_GPIO, on ? 0 : 1);
    gpio_set_level(LED_GPIO, on ? 1 : 0);
    printf("[ptt] PTT %s\n", on ? "KEYED" : "released");
#if SCO_AUDIO_TEST
    sco_audio_mute(on);  // FM commentary off while transmitting
#endif
#if TX_TIMEOUT_MS
    btstack_run_loop_remove_timer(&tx_timeout_timer);
    if (on) {
        btstack_run_loop_set_timer_handler(&tx_timeout_timer, &tx_timeout_handler);
        btstack_run_loop_set_timer(&tx_timeout_timer, TX_TIMEOUT_MS);
        btstack_run_loop_add_timer(&tx_timeout_timer);
    }
#endif
}

// Re-dial the headset until the SLC is up. The WS6 is always the acceptor (it
// never pages, even in pairing mode), so the AG must initiate.
static void connect_timer_handler(btstack_timer_source_t *ts)
{
    bool holdoff = slc_was_up &&
        (btstack_run_loop_get_time_ms() - slc_released_ms) < REDIAL_HOLDOFF_MS;
    if (!slc_connected && holdoff) {
        printf("[ptt] waiting for headset to reconnect...\n");
    } else if (!slc_connected) {
        printf("[ptt] dialing headset...\n");
        hfp_ag_establish_service_level_connection(peer_addr);
    }
    btstack_run_loop_set_timer(ts, 5000);
    btstack_run_loop_add_timer(ts);
}

static void handle_custom_at(const uint8_t *event)
{
    hci_con_handle_t h = hfp_subevent_custom_at_command_get_acl_handle(event);
    const char *cmd = hfp_subevent_custom_at_command_get_command_string(event);
    printf("[ptt] custom AT <= '%s'\n", cmd);

    if (strstr(cmd, "VOLOUT")) {
        // Output-volume query. A real reply (not a bare OK) is required or the
        // radio-mode setup aborts.
        // BTstack sends custom strings raw and keeps the pointer until the
        // RFCOMM slot frees up, so frame with CRLF and use static storage.
        static char resp[24];
        snprintf(resp, sizeof(resp), "\r\n+VOLOUT: %d\r\n", VOLOUT_LEVEL);
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        if (probe_step == 0) probe_step = 1;
        return;
    }

    if (strstr(cmd, "BLDN")) {
        // Seen occasionally (fire3, fire5). Not the PTT path - PTT arrives as
        // AT+CTKST=0,1 / 0,0 - so only log it.
        printf("[ptt] AT+BLDN (ignored)\n");
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    char *p = strstr(cmd, "CTKST=");
    if (p) {
        // Talk-key state: +CTKST=<a>,<b>. b is the key state (1 pressed).
        int a = 0, b = 0;
        sscanf(p + 6, "%d,%d", &a, &b);
        // fire33/35/36: on a fresh pairing the probe's third step is CTKST=0,1
        // (no button touched), and the button stays dead after it. Answer that
        // one with +CTKST=0,0, as if the AG reports the key released, and don't
        // key the radio for it.
        bool in_probe = probe_step >= 1 && probe_step <= 2;
        int echo_b = (in_probe && PROBE_REPLY_RELEASED) ? 0 : b;
        if (!in_probe) set_ptt(b != 0);
        static char resp[24];
        snprintf(resp, sizeof(resp), "\r\n+CTKST=%d,%d\r\n", a, echo_b);  // echo, as the real AG does
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        if (probe_step >= 1 && probe_step <= 2) {
            if (b != 0) {
                printf("[ptt] CTKST=0,1 in probe slot %d - replied +CTKST=0,%d\n",
                       probe_step, echo_b);
            }
            if (++probe_step == 3) {
                printf("[ptt] radio-mode probe complete\n");
#if SCO_AUDIO_TEST
                btstack_run_loop_remove_timer(&sco_timer);
                btstack_run_loop_set_timer_handler(&sco_timer, &sco_timer_handler);
                btstack_run_loop_set_timer(&sco_timer, SCO_AUDIO_DELAY_MS);
                btstack_run_loop_add_timer(&sco_timer);
#endif
            }
        }
        return;
    }

    if (strstr(cmd, "CSRSF")) {
        // fire4 (the run where the button emitted AT+CTKST=0,1) answered with
        // the full line, then OK. ws-pair.btsnoop predates fire4 and is not a
        // reference for this.
        // With BTstack, only a bare OK has made the headset go on to the
        // CTKST/VOLOUT probe (fire5); the full line leaves it idle.
#if CSRSF_FULL_REPLY
        hfp_ag_send_unsolicited_result_code(h, "\r\n+CSRSF: 1,1,1,1,1,0,3\r\n");
#endif
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    hfp_ag_send_command_result_code(h, true);  // plain OK
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *event, uint16_t event_size)
{
    UNUSED(channel);
    UNUSED(event_size);

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(event)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(event) == HCI_STATE_WORKING) {
                printf("[ptt] BTstack up, ready as \"%s\"\n", hfp_ag_service_name);
                connect_timer.process = &connect_timer_handler;
#if A2DP_TEST && A2DP_ONLY
                printf("[ptt] A2DP only: no HFP, dialing A2DP every %d ms\n", A2DP_RETRY_MS);
                btstack_run_loop_set_timer_handler(&a2dp_timer, &a2dp_timer_handler);
                btstack_run_loop_set_timer(&a2dp_timer, 2000);
                btstack_run_loop_add_timer(&a2dp_timer);
                break;
#elif A2DP_TEST && A2DP_FIRST
                printf("[ptt] A2DP first: dialing A2DP, HFP in %d ms\n", A2DP_FIRST_HFP_DELAY_MS);
                a2dp_src_connect(peer_addr);
                btstack_run_loop_set_timer(&connect_timer, A2DP_FIRST_HFP_DELAY_MS);
#else
                btstack_run_loop_set_timer(&connect_timer, 2000);
#endif
                btstack_run_loop_add_timer(&connect_timer);
            }
            break;

        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            // Headset forgot our pairing (e.g. evicted by a newer pairing):
            // drop the stale key so the next dial re-pairs from scratch.
            if (hci_event_authentication_complete_get_status(event) != ERROR_CODE_SUCCESS) {
                printf("[ptt] auth failed, status 0x%02x - dropping link key\n",
                       hci_event_authentication_complete_get_status(event));
                gap_drop_link_key_for_bd_addr(peer_addr);
            }
            break;

        case HCI_EVENT_SCO_CAN_SEND_NOW:
#if SCO_AUDIO_TEST
            sco_audio_send(hci_event_sco_can_send_now_get_handle(event));
#else
            sco_send_silence(hci_event_sco_can_send_now_get_handle(event));
#endif
            break;

        case HCI_EVENT_HFP_META:
            switch (hci_event_hfp_meta_get_subevent_code(event)) {
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
                uint8_t st = hfp_subevent_service_level_connection_established_get_status(event);
                if (st != ERROR_CODE_SUCCESS) {
                    // Page/SDP/SLC failed (e.g. status 0x04 page timeout). Stay
                    // not-connected so the dial timer keeps retrying.
                    printf("[ptt] SLC failed, status 0x%02x - will re-dial\n", st);
                    slc_connected = false;
                    acl_handle = HCI_CON_HANDLE_INVALID;
                    break;
                }
                acl_handle = hfp_subevent_service_level_connection_established_get_acl_handle(event);
                slc_connected = true;
                slc_was_up = true;
#if OPEN_SCO
                printf("[ptt] SLC up, opening SCO audio\n");
                hfp_ag_establish_audio_connection(acl_handle);
#else
                printf("[ptt] SLC up (no SCO, as in fire4)\n");
#endif
                probe_step = 0;
#if A2DP_TEST
                btstack_run_loop_remove_timer(&a2dp_timer);
                a2dp_tries = 0;
#if A2DP_PAUSE_TEST
                pause_test_paused = false;
                btstack_run_loop_remove_timer(&pause_timer);
                btstack_run_loop_set_timer_handler(&pause_timer, &pause_timer_handler);
                btstack_run_loop_set_timer(&pause_timer, A2DP_PAUSE_AT_MS);
                btstack_run_loop_add_timer(&pause_timer);
#endif
                btstack_run_loop_set_timer_handler(&a2dp_timer, &a2dp_timer_handler);
                btstack_run_loop_set_timer(&a2dp_timer, A2DP_DELAY_MS);
                btstack_run_loop_add_timer(&a2dp_timer);
#endif
                break;
            }
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
                printf("[ptt] SLC released\n");
                slc_connected = false;
                slc_released_ms = btstack_run_loop_get_time_ms();
                acl_handle = HCI_CON_HANDLE_INVALID;
                set_ptt(false);
                break;
            case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
                uint8_t status = hfp_subevent_audio_connection_established_get_status(event);
                if (status != ERROR_CODE_SUCCESS) {
                    printf("[ptt] SCO failed, status 0x%02x\n", status);
                    break;
                }
                sco_handle = hfp_subevent_audio_connection_established_get_sco_handle(event);
#if SCO_AUDIO_TEST
                uint8_t codec = hfp_subevent_audio_connection_established_get_negotiated_codec(event);
                printf("[ptt] SCO up (handle 0x%04x), codec %s\n", sco_handle,
                       codec == HFP_CODEC_MSBC ? "mSBC 16 kHz" : "CVSD 8 kHz");
                sco_audio_start(codec);
#else
                printf("[ptt] SCO up (handle 0x%04x) - headset now in radio mode\n", sco_handle);
#endif
                hci_request_sco_can_send_now_event_for_con_handle(sco_handle);
                break;
            }
            case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
                printf("[ptt] SCO released\n");
#if SCO_AUDIO_TEST
                sco_audio_stop();
#endif
                sco_handle = HCI_CON_HANDLE_INVALID;
                break;
            case HFP_SUBEVENT_CUSTOM_AT_COMMAND:
                handle_custom_at(event);
                break;
            default:
                break;
            }
            break;

        default:
            break;
        }
        break;

    case HCI_SCO_DATA_PACKET: {
        // Inbound mic audio from the headset. Phase A does not decode it, but we
        // count it: nonzero mic payload means the headset considers the SCO link
        // a live audio source (vs. an idle muted link), which is the state fire4
        // reached when the PTT button armed.
        static uint32_t rx_n = 0, rx_nonzero = 0;
        uint16_t sco_len = event_size > 3 ? event_size - 3 : 0;
        bool nz = false;
        for (uint16_t i = 3; i < event_size; i++) { if (event[i] != 0) { nz = true; break; } }
        if (nz) rx_nonzero++;
        if ((++rx_n % 250) == 0)
            printf("[ptt] sco rx %lu (nonzero %lu, len %u)\n",
                   (unsigned long)rx_n, (unsigned long)rx_nonzero, sco_len);
        break;
    }

    default:
        break;
    }
}

int btstack_main(int argc, const char *argv[]);
int btstack_main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

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
    set_ptt(false);

    // NOTE: the plain-text HCI dump is intentionally left off. Once SCO audio
    // flows it prints every 60-byte SCO packet over the 115200 serial line,
    // starving the run loop so the SCO RX buffer overflows ("dropping packet")
    // and real events (CTKST) get buried. Re-enable only for handshake
    // debugging with no SCO:
    //   hci_dump_init(hci_dump_embedded_stdout_get_instance());

    sco_demo_init();  // sets the CVSD voice setting (0x60)
#if SCO_AUDIO_TEST
    sco_audio_init();
#endif

    gap_set_local_name("WS-PTT 00:00:00:00:00:00");
    gap_discoverable_control(1);
    gap_set_class_of_device(CLASS_OF_DEVICE);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();

    // Exactly the feature set the working fire4 capture advertised: its
    // hfp_ag.py was run with --brsf 3680, i.e. AG bits 5,6,9,10,11 = reject-
    // call, enhanced-call-status, codec-negotiation, HF-indicators, eSCO-S4.
    // Codec negotiation (bit 9) is required: fire4 ran the AT+BAC / +BCS:1 /
    // AT+BCS=1 exchange, and the PTT button only armed after it. eSCO S4
    // (bit 11) gets a reliable EV3/2EV3 CVSD link. No in-band ring, no extended
    // error codes — fire4 advertised neither.
    uint16_t supported_features =
        (1 << HFP_AGSF_ABILITY_TO_REJECT_A_CALL) |
        (1 << HFP_AGSF_ENHANCED_CALL_STATUS) |
        (1 << HFP_AGSF_CODEC_NEGOTIATION) |
        (1 << HFP_AGSF_HF_INDICATORS) |
        (1 << HFP_AGSF_ESCO_S4);

    rfcomm_init();
    hfp_ag_init(rfcomm_channel_nr);
    hfp_ag_init_supported_features(supported_features);
    hfp_ag_init_codecs(sizeof(codecs), codecs);
    hfp_ag_init_ag_indicators(ag_indicators_nr, ag_indicators);
    hfp_ag_init_hf_indicators(hf_indicators_nr, hf_indicators);
    hfp_ag_init_call_hold_services(call_hold_services_nr, call_hold_services);

    // Force eSCO EV3 (HFP link setting S1) only. Legacy HV3 SCO on this ESP32's
    // SCO-over-HCI path reports tx/rx packet length 0 in Synchronous Connection
    // Complete, so no audio actually flows and the ACL dies at the 20 s
    // supervision timeout — and the WS6 never advances to the radio-mode probe
    // without real audio. eSCO delivers proper length-30 CVSD frames. The S4
    // setting (2EV3, latency 0x000c) is rejected by the controller with 0x1E
    // under codec negotiation, so constrain to plain EV3 (S1, latency 0x0007).
    hfp_set_sco_packet_types(SCO_PACKET_TYPES_EV3);

    // Answer the WS6's proprietary commands ourselves.
    hfp_ag_register_custom_at_command(&cmd_volout);
    hfp_ag_register_custom_at_command(&cmd_ctkst);
    hfp_ag_register_custom_at_command(&cmd_csrsf);
    hfp_ag_register_custom_at_command(&cmd_bldn);

    sdp_init();
    memset(hfp_service_buffer, 0, sizeof(hfp_service_buffer));
    hfp_ag_create_sdp_record_with_codecs(hfp_service_buffer, sdp_create_service_record_handle(),
                                         rfcomm_channel_nr, hfp_ag_service_name, 0,
                                         supported_features, sizeof(codecs), codecs);
#if !(A2DP_TEST && A2DP_ONLY)
    sdp_register_service(hfp_service_buffer);
#endif
#if A2DP_TEST
    a2dp_src_init();
#endif

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&packet_handler);
    hfp_ag_register_packet_handler(&packet_handler);

    sscanf_bd_addr(PEER_ADDR_STR, peer_addr);

    hci_power_control(HCI_POWER_ON);
    return 0;
}
