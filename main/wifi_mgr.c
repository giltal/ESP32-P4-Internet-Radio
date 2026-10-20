#include "wifi_mgr.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define EV_CONNECTED  BIT0

static EventGroupHandle_t s_ev;
static volatile wifi_state_t s_state = WIFI_STATE_IDLE;
static char s_ssid[33];
static char s_pass[65];
static int s_retries;
static bool s_scanning;
static bool s_sntp_started;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

/* Credentials are kept in RAM only; WiFi.txt on the SD card is the persistent copy */
static void load_credentials(void)
{
    strlcpy(s_ssid, CONFIG_RADIO_WIFI_SSID, sizeof(s_ssid));
    strlcpy(s_pass, CONFIG_RADIO_WIFI_PASSWORD, sizeof(s_pass));
}

static void apply_and_connect(void)
{
    if (!s_ssid[0]) {
        s_state = WIFI_STATE_IDLE;
        return;
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_state = WIFI_STATE_CONNECTING;
    s_retries = 0;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        apply_and_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_CONNECTED);
        if (!s_ssid[0] || s_scanning) {
            return;
        }
        s_retries++;
        s_state = s_retries > 8 ? WIFI_STATE_FAILED : WIFI_STATE_CONNECTING;
        /* keep trying forever, but back off once it's clearly failing */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, s_retries > 8 ? 10000000 : 1000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected to %s, IP " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        s_state = WIFI_STATE_CONNECTED;
        s_retries = 0;
        xEventGroupSetBits(s_ev, EV_CONNECTED);
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    }
}

esp_err_t wifi_mgr_start(void)
{
    s_ev = xEventGroupCreate();
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    load_credentials();

    /* Israel: UTC+2, DST from the Friday before the last Sunday of March to the last Sunday of October */
    setenv("TZ", "IST-2IDT,M3.4.4/26,M10.5.0", 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Streaming wants throughput, not power saving */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

wifi_state_t wifi_mgr_state(void)
{
    return s_state;
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(s_ev, EV_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & EV_CONNECTED;
}

void wifi_mgr_ssid(char *buf, size_t len)
{
    strlcpy(buf, s_ssid, len);
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_STATE_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));
    ESP_LOGI(TAG, "switching to %s", s_ssid);
    xEventGroupClearBits(s_ev, EV_CONNECTED);
    s_state = WIFI_STATE_CONNECTING;
    esp_timer_stop(s_retry_timer);
    s_scanning = true;              /* suppress auto-reconnect to the old network */
    esp_wifi_disconnect();
    s_scanning = false;
    apply_and_connect();
    return ESP_OK;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_info_t *)b)->rssi - ((const wifi_ap_info_t *)a)->rssi;
}

size_t wifi_mgr_scan(wifi_ap_info_t *out, size_t max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    /* a scan is refused while the driver is busy (re)connecting, so pause that */
    bool was_connected = s_state == WIFI_STATE_CONNECTED;
    s_scanning = true;
    if (!was_connected) {
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
    }
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (!was_connected && s_ssid[0]) {
        apply_and_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t count = 0;
    for (uint16_t i = 0; i < n && count < max; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[count].ssid, (char *)recs[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(recs);
    qsort(out, count, sizeof(out[0]), cmp_rssi);
    return count;
}
