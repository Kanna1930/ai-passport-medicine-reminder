#include "time_sync.h"
#include "blufi_security.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "time_sync";
static const char *DEVICE_NAME = "BLUFI_FoloPassport";
static const char *NTP_SERVER = "pool.ntp.org";

#define GOT_IP_BIT BIT0
#define CONNECT_FAIL_BIT BIT1
#define BLUFI_AP_LIST_COUNT 12
#define SAVED_WIFI_RETRY_LIMIT 3
#define SAVED_WIFI_TIMEOUT_MS 20000
#define SNTP_TIMEOUT_MS 12000

typedef struct {
    bool netif_ready;
    bool event_loop_ready;
    esp_netif_t *sta_netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    bool wifi_initialized;
    bool wifi_started;
    bool wifi_handler_registered;
    bool ip_handler_registered;

    bool blufi_active;
    bool host_initialized;
    bool host_running;
    bool gatt_initialized;
    bool btc_initialized;
    bool profile_initialized;
    bool ble_connected;
    SemaphoreHandle_t host_stopped;

    wifi_config_t sta_config;
    EventGroupHandle_t events;
    TaskHandle_t task;
    volatile time_sync_state_t state;
    volatile esp_err_t error;
    volatile bool provisioning;
    volatile int disconnect_count;
} time_sync_ctx_t;

static time_sync_ctx_t s;

static void set_state(time_sync_state_t state) {
    s.state = state;
}

bool time_sync_clock_valid(void) {
    time_t now = 0;
    time(&now);
    return now >= 1700000000;
}

bool time_sync_busy(void) {
    return s.task != NULL;
}

time_sync_state_t time_sync_get_state(void) {
    return s.state;
}

esp_err_t time_sync_last_error(void) {
    return s.error;
}

const char *time_sync_blufi_name(void) {
    return DEVICE_NAME;
}

static esp_err_t network_prepare(void) {
    if (!s.netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s.netif_ready = true;
    }
    if (!s.event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s.event_loop_ready = true;
    }
    return ESP_OK;
}

static void send_wifi_report(esp_blufi_sta_conn_state_t state) {
    if (!s.ble_connected) return;
    wifi_mode_t mode = WIFI_MODE_STA;
    (void)esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = {0};
    size_t ssid_len = strnlen((const char *)s.sta_config.sta.ssid,
                              sizeof(s.sta_config.sta.ssid));
    if (ssid_len > 0) {
        info.sta_ssid = s.sta_config.sta.ssid;
        info.sta_ssid_len = ssid_len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void send_wifi_list(void) {
    uint16_t count = BLUFI_AP_LIST_COUNT;
    wifi_ap_record_t records[BLUFI_AP_LIST_COUNT] = {0};
    esp_blufi_ap_record_t list[BLUFI_AP_LIST_COUNT] = {0};
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    for (uint16_t i = 0; i < count; ++i) {
        list[i].rssi = records[i].rssi;
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
    }
    if (s.ble_connected) esp_blufi_send_wifi_list(count, list);
}

static void request_wifi_connect(void) {
    s.disconnect_count = 0;
    set_state(TIME_SYNC_WIFI_CONNECTING);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s.error = err;
        set_state(TIME_SYNC_FAILED);
        if (s.events) xEventGroupSetBits(s.events, CONNECT_FAIL_BIT);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event ? event->reason : 0);
        if (s.provisioning) {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
            set_state(s.ble_connected ? TIME_SYNC_PHONE_CONNECTED : TIME_SYNC_PROVISIONING);
            return;
        }
        if (++s.disconnect_count < SAVED_WIFI_RETRY_LIMIT) {
            set_state(TIME_SYNC_CONNECTING);
            esp_wifi_connect();
        } else if (s.events) {
            xEventGroupSetBits(s.events, CONNECT_FAIL_BIT);
        }
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        if (s.provisioning) send_wifi_list();
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
    if (s.events) xEventGroupSetBits(s.events, GOT_IP_BIT);
}

static esp_err_t wifi_start(bool clear_credentials) {
    esp_err_t err = network_prepare();
    if (err != ESP_OK) return err;

    s.sta_netif = esp_netif_create_default_wifi_sta();
    if (!s.sta_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    s.wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s.wifi_handler);
    if (err != ESP_OK) return err;
    s.wifi_handler_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event, NULL, &s.ip_handler);
    if (err != ESP_OK) return err;
    s.ip_handler_registered = true;

    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s.wifi_started = true;

    if (clear_credentials) {
        wifi_config_t empty = {0};
        (void)esp_wifi_disconnect();
        err = esp_wifi_set_config(WIFI_IF_STA, &empty);
        memset(&s.sta_config, 0, sizeof(s.sta_config));
        return err;
    }
    return esp_wifi_get_config(WIFI_IF_STA, &s.sta_config);
}

static void blufi_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s.error = ESP_FAIL;
    set_state(TIME_SYNC_FAILED);
}

static void blufi_sync(void) {
    int rc = esp_blufi_profile_init();
    if (rc == 0) {
        s.profile_initialized = true;
    } else {
        s.error = ESP_FAIL;
        set_state(TIME_SYNC_FAILED);
    }
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    if (s.host_stopped) xSemaphoreGive(s.host_stopped);
    nimble_port_freertos_deinit();
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param) {
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            esp_blufi_adv_start_with_name(DEVICE_NAME);
            set_state(TIME_SYNC_PROVISIONING);
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            s.ble_connected = true;
            esp_blufi_adv_stop();
            if (blufi_security_init() != 0) {
                s.error = ESP_ERR_NO_MEM;
                set_state(TIME_SYNC_FAILED);
            } else {
                set_state(TIME_SYNC_PHONE_CONNECTED);
            }
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            s.ble_connected = false;
            blufi_security_deinit();
            esp_blufi_adv_start_with_name(DEVICE_NAME);
            set_state(TIME_SYNC_PROVISIONING);
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
            esp_wifi_set_mode(WIFI_MODE_STA);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            memcpy(s.sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            s.sta_config.sta.bssid_set = true;
            esp_wifi_set_config(WIFI_IF_STA, &s.sta_config);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID:
            if (param->sta_ssid.ssid_len >= sizeof(s.sta_config.sta.ssid)) {
                esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
                break;
            }
            memset(s.sta_config.sta.ssid, 0, sizeof(s.sta_config.sta.ssid));
            memset(s.sta_config.sta.password, 0, sizeof(s.sta_config.sta.password));
            memset(s.sta_config.sta.bssid, 0, sizeof(s.sta_config.sta.bssid));
            s.sta_config.sta.bssid_set = false;
            s.sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
            memcpy(s.sta_config.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
            esp_wifi_set_config(WIFI_IF_STA, &s.sta_config);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
            if (param->sta_passwd.passwd_len >= sizeof(s.sta_config.sta.password)) {
                esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
                break;
            }
            memset(s.sta_config.sta.password, 0, sizeof(s.sta_config.sta.password));
            memcpy(s.sta_config.sta.password, param->sta_passwd.passwd,
                   param->sta_passwd.passwd_len);
            esp_wifi_set_config(WIFI_IF_STA, &s.sta_config);
            break;
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
            request_wifi_connect();
            break;
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            esp_wifi_disconnect();
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
            send_wifi_report((xEventGroupGetBits(s.events) & GOT_IP_BIT)
                                 ? ESP_BLUFI_STA_CONN_SUCCESS
                                 : ESP_BLUFI_STA_CONN_FAIL);
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            wifi_scan_config_t scan = {0};
            if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
            esp_blufi_disconnect();
            break;
        case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
            esp_wifi_disconnect();
            break;
        case ESP_BLUFI_EVENT_REPORT_ERROR:
            esp_blufi_send_error_info(param->report_error.state);
            break;
        default:
            break;
    }
}

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = blufi_security_negotiate,
    .encrypt_func = blufi_security_encrypt,
    .decrypt_func = blufi_security_decrypt,
    .checksum_func = blufi_security_checksum,
};

static esp_err_t blufi_start(void) {
    s.provisioning = true;
    esp_err_t err = esp_blufi_register_callbacks(&s_blufi_callbacks);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    s.host_initialized = true;
    s.host_stopped = xSemaphoreCreateBinary();
    if (!s.host_stopped) return ESP_ERR_NO_MEM;
    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    if (esp_blufi_gatt_svr_init() != 0) return ESP_FAIL;
    s.gatt_initialized = true;
    if (ble_svc_gap_device_name_set(DEVICE_NAME) != 0) return ESP_FAIL;
    esp_blufi_btc_init();
    s.btc_initialized = true;
    err = esp_nimble_enable(host_task);
    if (err == ESP_OK) {
        s.host_running = true;
        s.blufi_active = true;
        set_state(TIME_SYNC_PROVISIONING);
    }
    return err;
}

static void blufi_stop(void) {
    s.ble_connected = false;
    blufi_security_deinit();
    if (!s.host_initialized) return;
    if (s.profile_initialized) esp_blufi_adv_stop();
    if (s.gatt_initialized) {
        esp_blufi_gatt_svr_deinit();
        s.gatt_initialized = false;
    }
    bool host_stopped = !s.host_running;
    if (s.host_running) {
        int rc = nimble_port_stop();
        if (rc == 0) {
            xSemaphoreTake(s.host_stopped, pdMS_TO_TICKS(3000));
            host_stopped = true;
        }
    }
    if (host_stopped) nimble_port_deinit();
    s.host_running = false;
    if (s.profile_initialized) {
        esp_blufi_profile_deinit();
        s.profile_initialized = false;
    }
    if (s.btc_initialized) {
        esp_blufi_btc_deinit();
        s.btc_initialized = false;
    }
    s.host_initialized = false;
    s.blufi_active = false;
    if (s.host_stopped) {
        vSemaphoreDelete(s.host_stopped);
        s.host_stopped = NULL;
    }
}

static void wifi_stop(void) {
    if (s.wifi_started) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        esp_wifi_stop();
        s.wifi_started = false;
    }
    if (s.ip_handler_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s.ip_handler);
        s.ip_handler_registered = false;
    }
    if (s.wifi_handler_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s.wifi_handler);
        s.wifi_handler_registered = false;
    }
    if (s.wifi_initialized) {
        esp_wifi_deinit();
        s.wifi_initialized = false;
    }
    if (s.sta_netif) {
        esp_netif_destroy_default_wifi(s.sta_netif);
        s.sta_netif = NULL;
    }
}

static esp_err_t sync_sntp(void) {
    set_state(TIME_SYNC_SNTP);
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) return err;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS));
    esp_netif_sntp_deinit();
    if (err == ESP_OK && !time_sync_clock_valid()) err = ESP_FAIL;
    return err;
}

static void service_task(void *arg) {
    bool force_provision = (bool)(uintptr_t)arg;
    s.error = ESP_OK;
    s.disconnect_count = 0;
    s.provisioning = false;
    xEventGroupClearBits(s.events, GOT_IP_BIT | CONNECT_FAIL_BIT);

    esp_err_t err = wifi_start(force_provision);
    if (err != ESP_OK) goto fail;

    bool has_saved = s.sta_config.sta.ssid[0] != '\0';
    if (!force_provision && has_saved) {
        set_state(TIME_SYNC_CONNECTING);
        err = esp_wifi_connect();
        if (err == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(s.events, GOT_IP_BIT | CONNECT_FAIL_BIT,
                                                   pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(SAVED_WIFI_TIMEOUT_MS));
            if (bits & GOT_IP_BIT) goto got_ip;
        }
    }

    xEventGroupClearBits(s.events, GOT_IP_BIT | CONNECT_FAIL_BIT);
    err = blufi_start();
    if (err != ESP_OK) goto fail;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s.events, GOT_IP_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & GOT_IP_BIT) break;
    }

got_ip:
    err = sync_sntp();
    if (err != ESP_OK) goto fail;
    set_state(TIME_SYNC_DONE);
    blufi_stop();
    wifi_stop();
    s.provisioning = false;
    s.task = NULL;
    vTaskDelete(NULL);
    return;

fail:
    s.error = err == ESP_OK ? ESP_FAIL: err;
    ESP_LOGE(TAG, "Time sync failed: %s", esp_err_to_name(s.error));
    set_state(TIME_SYNC_FAILED);
    blufi_stop();
    wifi_stop();
    s.provisioning = false;
    s.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t time_sync_start(bool force_provision) {
    if (s.task) return ESP_ERR_INVALID_STATE;
    if (!s.events) {
        s.events = xEventGroupCreate();
        if (!s.events) return ESP_ERR_NO_MEM;
    }
    set_state(force_provision ? TIME_SYNC_PROVISIONING : TIME_SYNC_CONNECTING);
    if (xTaskCreate(service_task, "time_sync", 7168,
                    (void *)(uintptr_t)force_provision, 5, &s.task) != pdPASS) {
        s.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
