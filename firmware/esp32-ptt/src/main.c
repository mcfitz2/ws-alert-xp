// ESP32 HFP Audio Gateway that drives radio push-to-talk from a stock
// 3M Peltor WS Alert XP (WS6) headset.
//
// The headset only enters its two-way-radio mode when, right after the
// service-level connection comes up, an AG opens a SCO audio link. Once in
// radio mode the Bluetooth button is reported over HFP as the proprietary
// talk-key command: AT+CTKST=0,1 on press, AT+CTKST=0,0 on release. This
// firmware reproduces that handshake and mirrors the talk-key onto a GPIO.
//
// Phase A (this file): no radio, no K1 plug. PTT_GPIO drives the onboard LED
// so you can watch it follow the headset button. Phase B points PTT_GPIO at a
// transistor/optocoupler across the radio's K1 PTT line; Phase C bridges the
// SCO audio to the radio's mic/speaker.
//
// Protocol background: ../../docs/radio-ptt.md

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ptt";

// GPIO that follows the talk-key. GPIO2 is the onboard LED on most ESP32
// devkits; repoint it at the radio PTT drive for Phase B.
#define PTT_GPIO GPIO_NUM_2

#define DEVICE_NAME "WS-PTT"

// The WS Alert XP headset. In pairing mode it waits for the phone (AG) to
// reach out, so the ESP32 initiates the service-level connection to it.
static const esp_bd_addr_t PEER_ADDR = {0x00, 0x15, 0xF3, 0x25, 0x7D, 0xE8};

// Output volume reported to the headset's AT+VOLOUT? query. Any valid level
// works; the laptop capture used 8.
#define VOLOUT_LEVEL 8

static esp_bd_addr_t s_peer;
static bool s_have_peer;
static volatile bool s_connected;
// Set once the headset is bonded. The WS6 only drives the AT handshake (sends
// AT+BRSF) when it opens the RFCOMM channel itself; an AG-initiated dial-in just
// sits idle until the SLC timer closes it. So we dial only to force the initial
// pairing, then stop and wait for the headset to reconnect to us on its own.
static volatile bool s_bonded;

static void set_ptt(bool on)
{
    gpio_set_level(PTT_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "PTT %s", on ? "KEYED" : "released");
}

// Reply to one of the headset's proprietary (non-standard) AT commands.
// Bluedroid passes anything it does not understand up as ESP_HF_UNAT_RESPONSE_EVT
// and appends the final OK after whatever we send here.
static void handle_unknown_at(const char *at)
{
    ESP_LOGI(TAG, "UNAT <= %s", at);
    if (!s_have_peer) {
        return;
    }

    if (strstr(at, "VOLOUT?")) {
        // Output-volume query. A real reply (not a bare OK) is required or the
        // radio-mode setup aborts.
        char resp[24];
        snprintf(resp, sizeof(resp), "+VOLOUT: %d", VOLOUT_LEVEL);
        esp_hf_ag_unknown_at_send(s_peer, resp);
        return;
    }

    char *p = strstr(at, "CTKST=");
    if (p) {
        // Talk-key state: +CTKST=<a>,<b>. b is the key state (1 pressed).
        int a = 0, b = 0;
        sscanf(p + 6, "%d,%d", &a, &b);
        set_ptt(b != 0);
        char resp[24];
        snprintf(resp, sizeof(resp), "+CTKST=%d,%d", a, b);
        esp_hf_ag_unknown_at_send(s_peer, resp);  // echo, as the real AG does
        return;
    }

    if (strstr(at, "CSRSF")) {
        esp_hf_ag_unknown_at_send(s_peer, "+CSRSF: 1,1,1,1,1,0,3");
        return;
    }

    // Everything else (AT+BIA, AT+BTRH?, AT+CSRBATT, ...) just needs an OK.
    esp_hf_ag_unknown_at_send(s_peer, "");
}

static void hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CONNECTION_STATE_EVT: {
        esp_hf_connection_state_t st = param->conn_stat.state;
        ESP_LOGI(TAG, "connection state %d", st);
        // Suppress re-dialing the moment anything is in progress; dialing again
        // mid-handshake tears down the forming SLC before it reaches state 3.
        s_connected = (st != ESP_HF_CONNECTION_STATE_DISCONNECTED);
        if (st == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_peer, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            s_have_peer = true;
            // The gate: open SCO as soon as the SLC is up. This is what flips
            // the headset into radio mode.
            ESP_LOGI(TAG, "SLC up, opening SCO audio");
            esp_hf_ag_audio_connect(s_peer);
        } else if (st == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            s_have_peer = false;
            set_ptt(false);
        }
        break;
    }
    case ESP_HF_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "audio state %d", param->audio_stat.state);
        break;
    case ESP_HF_UNAT_RESPONSE_EVT:
        handle_unknown_at(param->unat_rep.unat ? param->unat_rep.unat : "");
        break;
    case ESP_HF_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "volume type %d gain %d",
                 param->volume_control.type, param->volume_control.volume);
        break;
    default:
        ESP_LOGD(TAG, "hf event %d", event);
        break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "auth %s (stat %d)",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "FAIL",
                 param->auth_cmpl.stat);
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            s_bonded = true;
            ESP_LOGI(TAG, "bonded - stop dialing. Power-cycle the headset so IT "
                          "reconnects and drives the SLC");
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ssp confirm req, accepting");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey %" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "legacy pin req, replying 0000");
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "mode chg %d", param->mode_chg.mode);
        break;
    default:
        ESP_LOGI(TAG, "gap event %d", event);
        break;
    }
}

// Keep dialing the headset until the SLC comes up. In pairing mode the headset
// waits for the AG to initiate; an unbonded connect triggers just-works pairing.
static void connect_task(void *arg)
{
    while (1) {
        // Dial only until the headset is bonded. After that the headset must
        // open the RFCOMM itself (it won't drive the SLC on an AG-initiated
        // link), so we go passive and let it reconnect.
        if (!s_connected && !s_bonded) {
            ESP_LOGI(TAG, "dialing headset...");
            esp_hf_ag_slc_connect((uint8_t *)PEER_ADDR);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PTT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    set_ptt(false);

    // BR/EDR only: hand the BLE memory back before bringing the controller up.
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(gap_cb);
    esp_hf_ag_register_callback(hf_cb);
    ESP_ERROR_CHECK(esp_hf_ag_init());

    esp_bt_dev_set_device_name(DEVICE_NAME);

    // Look like a phone so the headset will initiate the HFP connection to us
    // (it only reaches out to a phone-class audio gateway).
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PHONE;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

    // Just-works pairing: no IO capability.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    // Discoverable + connectable so the headset can pair to and connect in.
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // If we already hold a bond with the headset (link key persisted in NVS), a
    // fresh AUTH_CMPL won't fire on reconnect, so detect it here and skip dialing
    // straight away - the headset must open the link itself to drive the SLC.
    int nbond = esp_bt_gap_get_bond_device_num();
    if (nbond > 0) {
        esp_bd_addr_t list[8];
        int n = nbond > 8 ? 8 : nbond;
        if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (memcmp(list[i], PEER_ADDR, sizeof(esp_bd_addr_t)) == 0) {
                    s_bonded = true;
                    ESP_LOGI(TAG, "already bonded - not dialing. Power-cycle the "
                                  "headset so IT reconnects and drives the SLC");
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "ready as \"%s\"%s", DEVICE_NAME,
             s_bonded ? "" : " - put the headset in pairing mode");
    xTaskCreate(connect_task, "connect", 3072, NULL, 5, NULL);
}
