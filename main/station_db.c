#include "station_db.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdcard.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "stations";

#define MAX_STATIONS     CONFIG_RADIO_MAX_STATIONS
#define MAX_FAVORITES    48
#define MAX_JSON_BYTES   (1536 * 1024)
#define BLOB_VERSION     1

/* radio-browser.info asks clients to rotate between its mirrors */
static const char *const s_servers[] = {
    "all.api.radio-browser.info",
    "de1.api.radio-browser.info",
    "de2.api.radio-browser.info",
    "fi1.api.radio-browser.info",
    "nl1.api.radio-browser.info",
};

static station_t *s_all;
static size_t s_all_n;
static station_t *s_fav;
static size_t s_fav_n;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_rev_all;
static volatile uint32_t s_rev_fav;
static volatile uint32_t s_rev_status;
static volatile bool s_busy;
static char s_status[96] = "אין תחנות עדיין";

/* ------------------------------------------------------------------ */

static void *psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
    s_rev_status++;
}

/* Lists are cached on the SD card as a small header + raw station_t array */
static void save_blob(const char *file, const station_t *list, size_t n)
{
    size_t sz = 4 + n * sizeof(station_t);
    uint8_t *buf = psram_malloc(sz);
    if (!buf) {
        return;
    }
    buf[0] = BLOB_VERSION;
    buf[1] = sizeof(station_t) & 0xFF;
    buf[2] = sizeof(station_t) >> 8;
    buf[3] = 0;
    memcpy(buf + 4, list, n * sizeof(station_t));
    sdcard_save(file, buf, sz);
    free(buf);
}

static size_t load_blob(const char *file, station_t *list, size_t cap)
{
    size_t sz = 0, n = 0;
    uint8_t *buf = sdcard_load(file, &sz);
    if (buf && sz > 4 && buf[0] == BLOB_VERSION && (buf[1] | (buf[2] << 8)) == sizeof(station_t)) {
        n = (sz - 4) / sizeof(station_t);
        if (n > cap) {
            n = cap;
        }
        memcpy(list, buf + 4, n * sizeof(station_t));
    }
    free(buf);
    return n;
}

esp_err_t station_db_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_all = heap_caps_calloc(MAX_STATIONS, sizeof(station_t), MALLOC_CAP_SPIRAM);
    s_fav = heap_caps_calloc(MAX_FAVORITES, sizeof(station_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_all || !s_fav) {
        return ESP_ERR_NO_MEM;
    }
    static const cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks((cJSON_Hooks *)&hooks);

    return ESP_OK;
}

void station_db_load(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_all_n = load_blob("stations.bin", s_all, MAX_STATIONS);
    s_fav_n = load_blob("favorites.bin", s_fav, MAX_FAVORITES);
    xSemaphoreGive(s_lock);
    if (s_all_n) {
        snprintf(s_status, sizeof(s_status), "%u תחנות (שמורות)", (unsigned)s_all_n);
    }
    ESP_LOGI(TAG, "loaded %u stations, %u favorites", (unsigned)s_all_n, (unsigned)s_fav_n);
    s_rev_all++;
    s_rev_fav++;
    s_rev_status++;
}

/* ------------------------------------------------------------------ */
/* Download                                                            */
/* ------------------------------------------------------------------ */

static char *http_get_json(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-P4-Internet-Radio/1.0",
        .buffer_size = 4096,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    char *body = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
        goto out;
    }
    body = psram_malloc(MAX_JSON_BYTES + 1);
    if (!body) {
        goto out;
    }
    size_t len = 0;
    for (;;) {
        int r = esp_http_client_read(c, body + len, MAX_JSON_BYTES - len);
        if (r < 0) {
            free(body);
            body = NULL;
            goto out;
        }
        if (r == 0) {
            break;
        }
        len += r;
        if (len >= MAX_JSON_BYTES) {
            ESP_LOGW(TAG, "directory response truncated");
            break;
        }
    }
    body[len] = 0;
    *out_len = len;
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return body;
}

static void trim_copy(char *dst, size_t cap, const char *src)
{
    while (*src && isspace((unsigned char)*src)) {
        src++;
    }
    size_t n = strlen(src);
    while (n && isspace((unsigned char)src[n - 1])) {
        n--;
    }
    if (n >= cap) {
        /* don't cut a UTF-8 sequence in half */
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int json_int(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

/* Parse the radio-browser station array into `out`; returns count. */
static size_t parse_directory(const char *json, station_t *out, size_t cap)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }
    size_t n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (n >= cap) {
            break;
        }
        if (json_int(it, "hls") != 0 || json_int(it, "lastcheckok") != 1) {
            continue;
        }
        const char *codec = json_str(it, "codec");
        const char *norm;
        if (strcasecmp(codec, "MP3") == 0) {
            norm = "MP3";
        } else if (strcasecmp(codec, "AAC") == 0 || strcasecmp(codec, "AAC+") == 0) {
            norm = "AAC";
        } else {
            continue;   /* OGG, FLAC, UNKNOWN, video... */
        }
        const char *url = json_str(it, "url_resolved");
        if (!*url) {
            url = json_str(it, "url");
        }
        if (strncmp(url, "http", 4) != 0 || strlen(url) >= sizeof(out[0].url) ||
                strstr(url, ".m3u8")) {
            continue;
        }
        station_t st = { 0 };
        trim_copy(st.name, sizeof(st.name), json_str(it, "name"));
        if (!st.name[0]) {
            continue;
        }
        /* radio-browser often lists the same station several times */
        bool dup = false;
        for (size_t i = 0; i < n; i++) {
            if (strcasecmp(out[i].name, st.name) == 0 || strcmp(out[i].url, url) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(st.url, url, sizeof(st.url));
        strlcpy(st.codec, norm, sizeof(st.codec));
        int br = json_int(it, "bitrate");
        st.bitrate = br > 0 && br < 2000 ? br : 0;
        out[n++] = st;
    }
    cJSON_Delete(root);
    return n;
}

esp_err_t station_db_refresh(void)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    set_status("מעדכן את רשימת התחנות…");

    station_t *tmp = heap_caps_calloc(MAX_STATIONS, sizeof(station_t), MALLOC_CAP_SPIRAM);
    esp_err_t result = ESP_FAIL;
    size_t got = 0;
    if (!tmp) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    char url[256];
    for (size_t i = 0; i < sizeof(s_servers) / sizeof(s_servers[0]) && !got; i++) {
        for (int scheme = 0; scheme < 2 && !got; scheme++) {
            snprintf(url, sizeof(url),
                     "%s://%s/json/stations/bycountrycodeexact/%s"
                     "?hidebroken=true&order=clickcount&reverse=true&limit=%d",
                     scheme == 0 ? "https" : "http", s_servers[i],
                     CONFIG_RADIO_COUNTRY_CODE, MAX_STATIONS * 2);
            ESP_LOGI(TAG, "GET %s", url);
            size_t len = 0;
            char *json = http_get_json(url, &len);
            if (json) {
                got = parse_directory(json, tmp, MAX_STATIONS);
                ESP_LOGI(TAG, "%u bytes -> %u playable stations", (unsigned)len, (unsigned)got);
                free(json);
            }
        }
    }

    if (got) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_all, tmp, got * sizeof(station_t));
        s_all_n = got;
        xSemaphoreGive(s_lock);
        s_rev_all++;
        save_blob("stations.bin", tmp, got);
        set_status("%u תחנות", (unsigned)got);
        result = ESP_OK;
    } else if (s_all_n) {
        set_status("העדכון נכשל, %u תחנות שמורות", (unsigned)s_all_n);
    } else {
        set_status("אין גישה ל-radio-browser.info");
    }

done:
    free(tmp);
    s_busy = false;
    s_rev_status++;
    return result;
}

static void refresh_task(void *arg)
{
    station_db_refresh();
    vTaskDelete(NULL);
}

void station_db_refresh_async(void)
{
    if (!s_busy) {
        /* internal-RAM stack for the TLS handshake */
        xTaskCreatePinnedToCore(refresh_task, "stations", 10240, NULL, 3, NULL, 0);
    }
}

bool station_db_busy(void)
{
    return s_busy;
}

uint32_t station_db_revision(station_list_t list)
{
    return list == STATION_LIST_FAVORITES ? s_rev_fav : s_rev_all;
}

uint32_t station_db_status_revision(void)
{
    return s_rev_status;
}

size_t station_db_count(station_list_t list)
{
    return list == STATION_LIST_FAVORITES ? s_fav_n : s_all_n;
}

bool station_db_get(station_list_t list, size_t index, station_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (list == STATION_LIST_FAVORITES && index < s_fav_n) {
        *out = s_fav[index];
        ok = true;
    } else if (list == STATION_LIST_ALL && index < s_all_n) {
        *out = s_all[index];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static int fav_index(const station_t *st)
{
    for (size_t i = 0; i < s_fav_n; i++) {
        if (strcmp(s_fav[i].url, st->url) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool station_db_is_favorite(const station_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fav = fav_index(st) >= 0;
    xSemaphoreGive(s_lock);
    return fav;
}

void station_db_set_favorite(const station_t *st, bool fav)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = fav_index(st);
    bool changed = false;
    if (fav && idx < 0 && s_fav_n < MAX_FAVORITES) {
        s_fav[s_fav_n++] = *st;
        changed = true;
    } else if (!fav && idx >= 0) {
        memmove(&s_fav[idx], &s_fav[idx + 1], (s_fav_n - idx - 1) * sizeof(station_t));
        s_fav_n--;
        changed = true;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        save_blob("favorites.bin", s_fav, s_fav_n);
        s_rev_fav++;
    }
}

void station_db_status(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_status, len);
    xSemaphoreGive(s_lock);
}
