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
 * Phase A (this file): PTT_GPIO drives the onboard LED so you can watch it
 * follow the headset button. No radio, no K1 plug.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "hci_dump_embedded_stdout.h"
#include "sco_demo_util.h"
#include "driver/gpio.h"

// GPIO that follows the talk-key. GPIO2 is the onboard LED on most ESP32
// devkits; repoint it at the radio PTT drive for Phase B.
#define PTT_GPIO GPIO_NUM_2

// Output volume reported to the headset's AT+VOLOUT? query.
#define VOLOUT_LEVEL 8

// The WS Alert XP headset.
static const char *PEER_ADDR_STR = "00:15:F3:25:7D:E8";
static bd_addr_t peer_addr;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t connect_timer;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco_handle = HCI_CON_HANDLE_INVALID;
static bool slc_connected = false;

// Only offer CVSD (codec id 1): the WS6 radio-mode handshake used narrow-band
// in the laptop capture, and Phase A does not bridge wide-band audio.
static const uint8_t codecs[] = { HFP_CODEC_CVSD };

static int ag_indicators_nr = 7;
static hfp_ag_indicator_t ag_indicators[] = {
    // index, name, min, max, status, mandatory, enabled, status changed
    {1, "service",   0, 1, 1, 0, 0, 0},
    {2, "call",      0, 1, 0, 1, 1, 0},
    {3, "callsetup", 0, 3, 0, 1, 1, 0},
    {4, "battchg",   0, 5, 3, 0, 0, 0},
    {5, "signal",    0, 5, 5, 0, 1, 0},
    {6, "roam",      0, 1, 0, 0, 1, 0},
    {7, "callheld",  0, 2, 0, 1, 1, 0},
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
enum { ID_VOLOUT = 1, ID_CTKST, ID_CSRSF };
static hfp_custom_at_command_t cmd_volout = { .command = "AT+VOLOUT?", .command_id = ID_VOLOUT };
static hfp_custom_at_command_t cmd_ctkst  = { .command = "AT+CTKST=",  .command_id = ID_CTKST };
static hfp_custom_at_command_t cmd_csrsf  = { .command = "AT+CSRSF=",  .command_id = ID_CSRSF };

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

static void set_ptt(bool on)
{
    gpio_set_level(PTT_GPIO, on ? 1 : 0);
    printf("[ptt] PTT %s\n", on ? "KEYED" : "released");
}

// Re-dial the headset until the SLC is up. The WS6 is always the acceptor (it
// never pages, even in pairing mode), so the AG must initiate.
static void connect_timer_handler(btstack_timer_source_t *ts)
{
    if (!slc_connected) {
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
        char resp[24];
        snprintf(resp, sizeof(resp), "+VOLOUT: %d", VOLOUT_LEVEL);
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    char *p = strstr(cmd, "CTKST=");
    if (p) {
        // Talk-key state: +CTKST=<a>,<b>. b is the key state (1 pressed).
        int a = 0, b = 0;
        sscanf(p + 6, "%d,%d", &a, &b);
        set_ptt(b != 0);
        char resp[24];
        snprintf(resp, sizeof(resp), "+CTKST=%d,%d", a, b);  // echo, as the real AG does
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    if (strstr(cmd, "CSRSF")) {
        // fire4 (the proven laptop capture) answered AT+CSRSF with a bare OK.
        // Sending a "+CSRSF: ..." line back appears to stall the WS6 at the last
        // SLC step so it never starts the radio-mode probe (VOLOUT?/CTKST=0,0).
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
                btstack_run_loop_set_timer(&connect_timer, 2000);
                btstack_run_loop_add_timer(&connect_timer);
            }
            break;

        case HCI_EVENT_SCO_CAN_SEND_NOW:
            sco_send_silence(hci_event_sco_can_send_now_get_handle(event));
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
                printf("[ptt] SLC up, opening SCO audio (the radio-mode gate)\n");
                // The gate: open SCO as soon as the SLC is up.
                hfp_ag_establish_audio_connection(acl_handle);
                break;
            }
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
                printf("[ptt] SLC released\n");
                slc_connected = false;
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
                hci_request_sco_can_send_now_event_for_con_handle(sco_handle);
                printf("[ptt] SCO up (handle 0x%04x) - headset now in radio mode\n", sco_handle);
                break;
            }
            case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
                printf("[ptt] SCO released\n");
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

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PTT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    set_ptt(false);

    // NOTE: the plain-text HCI dump is intentionally left off. Once SCO audio
    // flows it prints every 60-byte SCO packet over the 115200 serial line,
    // starving the run loop so the SCO RX buffer overflows ("dropping packet")
    // and real events (CTKST) get buried. Re-enable only for handshake
    // debugging with no SCO:
    //   hci_dump_init(hci_dump_embedded_stdout_get_instance());

    sco_demo_init();

    gap_set_local_name("WS-PTT 00:00:00:00:00:00");
    gap_discoverable_control(1);
    // Look like a phone (the class the WS6 expects of an audio gateway).
    gap_set_class_of_device(0x200408);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();

    // No eSCO (HFP_AGSF_ESCO_S4): the ESP32's SCO-over-HCI path and the WS6
    // (BRSF=665 advertises no eSCO S4) only do basic CVSD SCO/HV3. Advertising
    // eSCO makes BTstack attempt an EV3 setting the controller rejects (0x0a).
    // Advertise codec negotiation so the SLC runs the HF's AT+BAC / AG +BCS
    // exchange and the SCO is opened as a *negotiated* CVSD link, the way the
    // BlueZ laptop (fire4) did it. Without it BTstack force-opens a raw SCO that
    // the WS6 accepts (no 2.1 s timeout) but does not fully treat as radio
    // audio, so the PTT button never arms. We still offer only CVSD (codec 1),
    // so negotiation resolves to CVSD and the HV3 SCO type is unchanged.
    // BRSF 0 (no features) regressed: the WS6 stalled right after AT+CSRSF and
    // never ran the CTKST/VOLOUT radio-mode probe. Advertising in-band ring +
    // extended error codes is what makes the WS6 run the probe to the stable
    // talk-key-idle state. Keep codec negotiation OFF (fire4 parity).
    // WS6 BRSF=665 sets bit 9 (eSCO S4 supported) — it wants an eSCO voice
    // link, which is what a real two-way-radio adapter and fire4's Linux SCO
    // socket used. Advertise eSCO S4 so BTstack negotiates an EV3/2EV3 eSCO
    // link instead of lossy legacy HV3; the headset may only arm PTT on a
    // reliable eSCO link.
    uint16_t supported_features =
        (1 << HFP_AGSF_EXTENDED_ERROR_RESULT_CODES) |
        (1 << HFP_AGSF_IN_BAND_RING_TONE) |
        (1 << HFP_AGSF_ESCO_S4);

    rfcomm_init();
    hfp_ag_init(rfcomm_channel_nr);
    hfp_ag_init_supported_features(supported_features);
    hfp_ag_init_codecs(sizeof(codecs), codecs);
    hfp_ag_init_ag_indicators(ag_indicators_nr, ag_indicators);
    hfp_ag_init_hf_indicators(hf_indicators_nr, hf_indicators);
    hfp_ag_init_call_hold_services(call_hold_services_nr, call_hold_services);

    // Allow eSCO (EV3 = S1, 2EV3 = S3/S4) with HV3 as fallback. The earlier
    // 0x0a eSCO reject was "synchronous connection limit exceeded" from
    // MAX_SYNC_CONN=0, now set to 1. If the controller still rejects an eSCO
    // setting, BTstack retries the next one down to HV3.
    hfp_set_sco_packet_types(SCO_PACKET_TYPES_EV3 | SCO_PACKET_TYPES_2EV3 |
                             SCO_PACKET_TYPES_HV3);

    // Answer the WS6's proprietary commands ourselves.
    hfp_ag_register_custom_at_command(&cmd_volout);
    hfp_ag_register_custom_at_command(&cmd_ctkst);
    hfp_ag_register_custom_at_command(&cmd_csrsf);

    sdp_init();
    memset(hfp_service_buffer, 0, sizeof(hfp_service_buffer));
    hfp_ag_create_sdp_record_with_codecs(hfp_service_buffer, sdp_create_service_record_handle(),
                                         rfcomm_channel_nr, hfp_ag_service_name, 0,
                                         supported_features, sizeof(codecs), codecs);
    sdp_register_service(hfp_service_buffer);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&packet_handler);
    hfp_ag_register_packet_handler(&packet_handler);

    sscanf_bd_addr(PEER_ADDR_STR, peer_addr);

    hci_power_control(HCI_POWER_ON);
    return 0;
}
