#include "clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "sdcard.h"

static const char *TAG = "clock";

#define RTC_ADDR        0x32
#define REG_SEC         0x00
#define REG_FLAG        0x0E
#define FLAG_VLF        0x02     /* oscillator stopped / battery lost: time is garbage */
#define VALID_EPOCH     1704067200   /* 2024-01-01: anything earlier is an unset clock */

static i2c_master_dev_handle_t s_rtc;
static bool s_valid;
static bool s_auto_ntp = true;
static bool s_sntp_started;
static bool s_force_sync;        /* "sync now" pressed while automatic time is off */

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(int d)     { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* UTC broken-down time -> epoch (newlib has no timegm) */
static time_t utc_to_epoch(const struct tm *t)
{
    int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, len, 100);
}

static esp_err_t rtc_write_utc(time_t now)
{
    if (!s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm t;
    gmtime_r(&now, &t);
    uint8_t buf[8] = {
        REG_SEC, dec2bcd(t.tm_sec), dec2bcd(t.tm_min), dec2bcd(t.tm_hour),
        (uint8_t)(1 << t.tm_wday), dec2bcd(t.tm_mday), dec2bcd(t.tm_mon + 1), dec2bcd(t.tm_year - 100),
    };
    esp_err_t err = i2c_master_transmit(s_rtc, buf, sizeof(buf), 100);
    if (err == ESP_OK) {
        uint8_t flag[2] = { REG_FLAG, 0 };   /* clear VLF: the time is good now */
        i2c_master_transmit(s_rtc, flag, sizeof(flag), 100);
    }
    return err;
}

static void set_system_time(time_t t)
{
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    s_valid = true;
}

esp_err_t clock_init(i2c_master_bus_handle_t bus)
{
    /* Israel: UTC+2, DST from the Friday before the last Sunday of March to the last Sunday of October */
    setenv("TZ", "IST-2IDT,M3.4.4/26,M10.5.0", 1);
    tzset();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_rtc);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t flag = 0, r[7];
    if (rtc_read(REG_FLAG, &flag, 1) != ESP_OK || rtc_read(REG_SEC, r, sizeof(r)) != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T not responding - time only from the network");
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    struct tm t = {
        .tm_sec = bcd2dec(r[0] & 0x7F), .tm_min = bcd2dec(r[1] & 0x7F), .tm_hour = bcd2dec(r[2] & 0x3F),
        .tm_mday = bcd2dec(r[4] & 0x3F), .tm_mon = bcd2dec(r[5] & 0x1F) - 1, .tm_year = bcd2dec(r[6]) + 100,
    };
    bool sane = !(flag & FLAG_VLF) && t.tm_mon >= 0 && t.tm_mon < 12 && t.tm_mday >= 1 && t.tm_mday <= 31 &&
                t.tm_hour < 24 && t.tm_min < 60 && t.tm_sec < 60;
    time_t epoch = sane ? utc_to_epoch(&t) : 0;
    if (epoch >= VALID_EPOCH) {
        set_system_time(epoch);
        ESP_LOGI(TAG, "time from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC has no valid time (battery lost?) - waiting for network or manual setting");
    }
    return ESP_OK;
}

bool clock_is_valid(void)
{
    return s_valid || time(NULL) >= VALID_EPOCH;
}

esp_err_t clock_set_local(const struct tm *local)
{
    struct tm t = *local;
    t.tm_isdst = -1;            /* let the TZ rules decide summer time */
    time_t epoch = mktime(&t);
    if (epoch < VALID_EPOCH) {
        return ESP_ERR_INVALID_ARG;
    }
    set_system_time(epoch);
    ESP_LOGI(TAG, "clock set by user: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min);
    return rtc_write_utc(epoch);
}

static void on_sntp_sync(struct timeval *tv)
{
    if (!s_auto_ntp && !s_force_sync) {
        return;     /* system time already moved, but we don't keep it: restore from RTC next boot */
    }
    s_force_sync = false;
    s_valid = true;
    rtc_write_utc(tv->tv_sec);
    ESP_LOGI(TAG, "time synced from the network, RTC updated");
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        esp_sntp_restart();
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    }
}

void clock_network_up(void)
{
    if (s_auto_ntp) {
        start_sntp();
    }
}

void clock_sync_now(void)
{
    s_force_sync = true;
    start_sntp();
}

bool clock_auto_ntp(void)
{
    return s_auto_ntp;
}

void clock_set_auto_ntp(bool on)
{
    s_auto_ntp = on;
    sdcard_save_int("auto_ntp.txt", on ? 1 : 0);
    if (!on && s_sntp_started) {
        esp_netif_sntp_deinit();    /* stop polling; manual time stays as set */
        s_sntp_started = false;
    }
}

void clock_load_settings(void)
{
    s_auto_ntp = sdcard_load_int("auto_ntp.txt", 1) != 0;
}
