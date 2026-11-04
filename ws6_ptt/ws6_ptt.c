/*
 * ws6_ptt: BTstack HFP Audio Gateway that drives push-to-talk from a stock
 * 3M Peltor WS Alert XP (WS6) headset. See ws6_ptt.h and docs/radio-ptt.md.
 *
 * Why BTstack and not ESP-IDF's Bluedroid: the WS6 injects a proprietary AT
 * command *during* the service-level connection handshake and waits for a
 * reply. Bluedroid only forwards unknown AT to the app *after* the SLC is up,
 * so its AG stalls and the SLC times out. BTstack lets the app answer any AT
 * command at any time: hfp_ag_register_custom_at_command() lets us answer the
 * headset's proprietary ones precisely, mid-SLC included.
 *
 * Recipe (fire42 CVSD, fire43 mSBC): act as HFP AG with the fire4 feature set
 * and indicator table, open SCO the moment the SLC is up, answer AT+CSRSF with
 * a bare OK, AT+VOLOUT? with a real +VOLOUT value, and echo AT+CTKST=. The
 * headset then runs its radio-mode probe (CTKST=0,0, VOLOUT?, 0,0, 0,0) and
 * from then on reports the Bluetooth button as AT+CTKST=0,1 (press) / 0,0
 * (release).
 *
 * The WS6 types a device (by BT address) on its first session. If SCO is not
 * up by the time the radio module sends its setup burst, the probe times out
 * (CTKST=0,1 -> ~2.1 s -> 0,0) and the address is stored as a phone for good:
 * no probe, dead button, and neither an NVS erase nor a headset factory reset
 * undoes it. Only a fresh BT address does.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "ws6_ptt.h"

#define DIAL_FIRST_MS 2000
#define DIAL_PERIOD_MS 5000

static ws6_ptt_config_t cfg;
static ws6_ptt_hooks_t hooks;

static bd_addr_t peer_addr;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t connect_timer;
static btstack_timer_source_t tx_timeout_timer;
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco_handle = HCI_CON_HANDLE_INVALID;
static ws6_ptt_state_t state = WS6_PTT_DISCONNECTED;
static bool keyed = false;
static uint32_t slc_released_ms = 0;
static bool slc_was_up = false;
// Radio-mode probe progress: 0 = waiting for VOLOUT?, 1..2 = VOLOUT? seen and
// that many CTKST since, 3 = complete. Talk-key reports inside the probe are
// part of the handshake, not button presses.
static int probe_step;

static uint8_t codecs[2];
static uint8_t codecs_nr;

// Order and values match fire4's CIND exactly: service,call,callsetup,callheld,
// signal,roam,battchg with CIND? = 1,0,0,0,5,0,5. The WS6 keys radio-mode logic
// on indicator positions (another order put battchg at 4 / callheld at 7, which
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
// BTstack replaces the zeros with the local BD address.
static char gap_name[48];

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

ws6_ptt_state_t ws6_ptt_get_state(void)
{
    return state;
}

bool ws6_ptt_is_keyed(void)
{
    return keyed;
}

static void set_state(ws6_ptt_state_t s)
{
    if (s == state) return;
    state = s;
    if (hooks.on_state) hooks.on_state(s);
}

static void set_ptt(bool on);

static void tx_timeout_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    printf("[ptt] TX timeout (%lu ms), forcing release\n", (unsigned long)cfg.tx_timeout_ms);
    set_ptt(false);
}

static void set_ptt(bool on)
{
    keyed = on;
    printf("[ptt] PTT %s\n", on ? "KEYED" : "released");
    if (hooks.on_ptt) hooks.on_ptt(on);
    btstack_run_loop_remove_timer(&tx_timeout_timer);
    if (on && cfg.tx_timeout_ms) {
        btstack_run_loop_set_timer_handler(&tx_timeout_timer, &tx_timeout_handler);
        btstack_run_loop_set_timer(&tx_timeout_timer, cfg.tx_timeout_ms);
        btstack_run_loop_add_timer(&tx_timeout_timer);
    }
}

// Keep the SCO link fed on every can-send-now slot, with app audio or silence.
// The link only has to be up to hold radio mode; a controller underrun drops it.
static void sco_send(hci_con_handle_t h)
{
    if (h == HCI_CON_HANDLE_INVALID) return;
    int pkt_len = hci_get_sco_packet_length_for_connection(h);
    int payload_len = pkt_len - 3;

    hci_reserve_packet_buffer();
    uint8_t *pkt = hci_get_outgoing_packet_buffer();
    if (hooks.sco_fill) {
        hooks.sco_fill(&pkt[3], (uint16_t)payload_len);
    } else {
        memset(&pkt[3], 0, payload_len);
    }
    little_endian_store_16(pkt, 0, h);
    pkt[2] = (uint8_t)payload_len;
    hci_send_sco_packet_buffer(pkt_len);

    hci_request_sco_can_send_now_event_for_con_handle(h);
}

// Re-dial the headset until the SLC is up. The WS6 is always the acceptor (it
// never pages, even in pairing mode), so the AG must initiate. After an SLC
// drop, hold off so the headset can come back on its own (in fire4 it
// reconnected ~47 s later and then ran the radio-mode probe).
static void connect_timer_handler(btstack_timer_source_t *ts)
{
    bool holdoff = slc_was_up &&
        (btstack_run_loop_get_time_ms() - slc_released_ms) < cfg.redial_holdoff_ms;
    if (state == WS6_PTT_DISCONNECTED && holdoff) {
        printf("[ptt] waiting for headset to reconnect...\n");
    } else if (state == WS6_PTT_DISCONNECTED) {
        printf("[ptt] dialing headset...\n");
        hfp_ag_establish_service_level_connection(peer_addr);
    }
    btstack_run_loop_set_timer(ts, DIAL_PERIOD_MS);
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
        snprintf(resp, sizeof(resp), "\r\n+VOLOUT: %d\r\n", cfg.volout_level);
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        if (probe_step == 0) probe_step = 1;
        return;
    }

    if (strstr(cmd, "BLDN")) {
        // Seen occasionally (fire3, fire5). Not the PTT path, so only log it.
        printf("[ptt] AT+BLDN (ignored)\n");
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    char *p = strstr(cmd, "CTKST=");
    if (p) {
        // Talk-key state: +CTKST=<a>,<b>. b is the key state (1 pressed).
        int a = 0, b = 0;
        sscanf(p + 6, "%d,%d", &a, &b);
        bool in_probe = probe_step >= 1 && probe_step <= 2;
        if (!in_probe) set_ptt(b != 0);
        static char resp[24];
        snprintf(resp, sizeof(resp), "\r\n+CTKST=%d,%d\r\n", a, b);  // echo, as the real AG does
        hfp_ag_send_unsolicited_result_code(h, resp);
        hfp_ag_send_command_result_code(h, true);
        if (in_probe) {
            if (b != 0) printf("[ptt] CTKST=0,1 in probe slot %d\n", probe_step);
            if (++probe_step == 3) {
                printf("[ptt] radio-mode probe complete\n");
                set_state(WS6_PTT_RADIO_MODE);
            }
        }
        return;
    }

    if (strstr(cmd, "CSRSF")) {
        // fire4 (BlueZ) answered with "+CSRSF: 1,1,1,1,1,0,3" then OK. With
        // BTstack only a bare OK has made the headset go on to the CTKST/VOLOUT
        // probe (fire5); the full line leaves it idle.
        hfp_ag_send_command_result_code(h, true);
        return;
    }

    hfp_ag_send_command_result_code(h, true);  // plain OK
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *event, uint16_t event_size)
{
    UNUSED(channel);

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(event)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(event) == HCI_STATE_WORKING) {
                printf("[ptt] BTstack up, ready as \"%s\"\n", cfg.name);
                btstack_run_loop_set_timer_handler(&connect_timer, &connect_timer_handler);
                btstack_run_loop_set_timer(&connect_timer, DIAL_FIRST_MS);
                btstack_run_loop_add_timer(&connect_timer);
            }
            break;

        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            // Headset forgot our pairing (e.g. evicted by a newer pairing):
            // drop the stale key so the next dial re-pairs from scratch. The
            // headset has to be in pairing mode for that to succeed.
            if (hci_event_authentication_complete_get_status(event) != ERROR_CODE_SUCCESS) {
                printf("[ptt] auth failed, status 0x%02x - dropping link key\n",
                       hci_event_authentication_complete_get_status(event));
                gap_drop_link_key_for_bd_addr(peer_addr);
            }
            break;

        case HCI_EVENT_SCO_CAN_SEND_NOW:
            sco_send(hci_event_sco_can_send_now_get_handle(event));
            break;

        case HCI_EVENT_HFP_META:
            switch (hci_event_hfp_meta_get_subevent_code(event)) {
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
                uint8_t st = hfp_subevent_service_level_connection_established_get_status(event);
                if (st != ERROR_CODE_SUCCESS) {
                    // Page/SDP/SLC failed (0x04 page timeout: headset off; 0x05
                    // auth failure: needs pairing mode). The dial timer retries.
                    printf("[ptt] SLC failed, status 0x%02x - will re-dial\n", st);
                    acl_handle = HCI_CON_HANDLE_INVALID;
                    break;
                }
                acl_handle = hfp_subevent_service_level_connection_established_get_acl_handle(event);
                slc_was_up = true;
                probe_step = 0;
                // Audio must be up before the radio module's AT+CSRSF burst, or
                // a first session types this address as a phone (see top).
                printf("[ptt] SLC up, opening SCO audio\n");
                hfp_ag_establish_audio_connection(acl_handle);
                set_state(WS6_PTT_CONNECTED);
                break;
            }
            case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
                printf("[ptt] SLC released\n");
                slc_released_ms = btstack_run_loop_get_time_ms();
                acl_handle = HCI_CON_HANDLE_INVALID;
                set_ptt(false);
                set_state(WS6_PTT_DISCONNECTED);
                break;
            case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
                uint8_t status = hfp_subevent_audio_connection_established_get_status(event);
                if (status != ERROR_CODE_SUCCESS) {
                    printf("[ptt] SCO failed, status 0x%02x\n", status);
                    break;
                }
                sco_handle = hfp_subevent_audio_connection_established_get_sco_handle(event);
                uint8_t codec = hfp_subevent_audio_connection_established_get_negotiated_codec(event);
                printf("[ptt] SCO up (handle 0x%04x), codec %s\n", sco_handle,
                       codec == HFP_CODEC_MSBC ? "mSBC 16 kHz" : "CVSD 8 kHz");
                if (hooks.on_audio_start) hooks.on_audio_start(codec);
                hci_request_sco_can_send_now_event_for_con_handle(sco_handle);
                break;
            }
            case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
                printf("[ptt] SCO released\n");
                sco_handle = HCI_CON_HANDLE_INVALID;
                if (hooks.on_audio_stop) hooks.on_audio_stop();
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

    case HCI_SCO_DATA_PACKET:
        if (hooks.sco_rx) hooks.sco_rx(event, event_size);
        break;

    default:
        break;
    }
}

void ws6_ptt_init(const ws6_ptt_config_t *config, const ws6_ptt_hooks_t *h)
{
    cfg = *config;
    if (h) hooks = *h;
    sscanf_bd_addr(cfg.headset_addr, peer_addr);

    // NOTE: leave the plain-text HCI dump off. Once SCO audio flows it prints
    // every SCO packet over the 115200 serial line, starving the run loop so
    // the SCO RX buffer overflows and real events (CTKST) get buried. Enable
    // only for handshake debugging:
    //   hci_dump_init(hci_dump_embedded_stdout_get_instance());

    // What BTstack's sco_demo_init() did in every proven run: BR/EDR Secure
    // Connections off (it does not get along with SCO on this controller) and
    // CVSD voice setting (mSBC switches to transparent mode by itself).
    gap_secure_connections_enable(false);
    hci_set_sco_voice_setting(0x60);  // linear, unsigned, 16-bit, CVSD

    snprintf(gap_name, sizeof(gap_name), "%s 00:00:00:00:00:00", cfg.name);
    gap_set_local_name(gap_name);
    gap_discoverable_control(1);
    gap_set_class_of_device(cfg.class_of_device);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    l2cap_init();

    // Exactly the feature set the working fire4 capture advertised (BRSF 3680):
    // AG bits 5,6,9,10,11 = reject-call, enhanced-call-status, codec-
    // negotiation, HF-indicators, eSCO-S4. Codec negotiation is required: the
    // PTT button only armed after the AT+BAC / +BCS / AT+BCS exchange. No
    // in-band ring, no extended error codes.
    uint16_t supported_features =
        (1 << HFP_AGSF_ABILITY_TO_REJECT_A_CALL) |
        (1 << HFP_AGSF_ENHANCED_CALL_STATUS) |
        (1 << HFP_AGSF_CODEC_NEGOTIATION) |
        (1 << HFP_AGSF_HF_INDICATORS) |
        (1 << HFP_AGSF_ESCO_S4);

    // The WS6 sends AT+BAC=1,2, so with mSBC offered BTstack picks it (16 kHz).
    codecs_nr = 0;
    codecs[codecs_nr++] = HFP_CODEC_CVSD;
    if (cfg.offer_msbc) codecs[codecs_nr++] = HFP_CODEC_MSBC;

    rfcomm_init();
    hfp_ag_init(rfcomm_channel_nr);
    hfp_ag_init_supported_features(supported_features);
    hfp_ag_init_codecs(codecs_nr, codecs);
    hfp_ag_init_ag_indicators(ag_indicators_nr, ag_indicators);
    hfp_ag_init_hf_indicators(hf_indicators_nr, hf_indicators);
    hfp_ag_init_call_hold_services(call_hold_services_nr, call_hold_services);

    // Force eSCO EV3 (HFP link setting S1) only. Legacy HV3 SCO on this ESP32's
    // SCO-over-HCI path reports tx/rx packet length 0 in Synchronous Connection
    // Complete, so no audio flows and the ACL dies at the 20 s supervision
    // timeout, and the WS6 never reaches the radio-mode probe without real
    // audio. The S4 setting (2EV3, latency 0x000c) is rejected by the
    // controller with 0x1E under codec negotiation.
    hfp_set_sco_packet_types(SCO_PACKET_TYPES_EV3);

    hfp_ag_register_custom_at_command(&cmd_volout);
    hfp_ag_register_custom_at_command(&cmd_ctkst);
    hfp_ag_register_custom_at_command(&cmd_csrsf);
    hfp_ag_register_custom_at_command(&cmd_bldn);

    sdp_init();
    memset(hfp_service_buffer, 0, sizeof(hfp_service_buffer));
    hfp_ag_create_sdp_record_with_codecs(hfp_service_buffer, sdp_create_service_record_handle(),
                                         rfcomm_channel_nr, cfg.name, 0,
                                         supported_features, codecs_nr, codecs);
    sdp_register_service(hfp_service_buffer);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&packet_handler);
    hfp_ag_register_packet_handler(&packet_handler);

    hci_power_control(HCI_POWER_ON);
}
