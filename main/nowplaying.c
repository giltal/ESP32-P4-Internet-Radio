#include "nowplaying.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "clock.h"
#include "player.h"

static const char *TAG = "nowplaying";

#define MIN_POLL_MS   45000     /* Cloudflare guards kan.org.il: keep requests sparse */
#define MAX_POLL_MS   120000
#define BLOCKED_MIN_MS 60000             /* after a Cloudflare 403: back off 1, 2, 4, 8 .. 15 min */
#define BLOCKED_MAX_MS (15 * 60 * 1000)
#define KEEP_AFTER_END 30                 /* s: keep the last title this long past its end time on errors */
#define MIN_GAP_MS    30000              /* between any two requests, even when stations change fast */
#define BODY_MAX      4096

/* Kan stream mount -> kan.org.il live-track channel id */
static const struct {
    const char *mount;
    int channel;
} s_kan[] = {
    { "KAN_KOL_HAMUSICA", 5 },
    { "KAN_GIMMEL", 9 },
    { "KAN_TARBUT", 7 },
    { "KAN_BET", 8 },
    { "KAN_88", 4 },
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_url[sizeof(((station_t *)0)->url)];
static int s_channel;               /* 0 = nothing to follow */
static uint32_t s_gen;              /* bumped on every station change */
static uint32_t s_blocked_ms;       /* current 403 back-off, 0 = not blocked */
static time_t s_title_end;          /* end time of the title currently shown (0 = unknown / none) */

static int kan_channel(const char *url)
{
    for (size_t i = 0; i < sizeof(s_kan) / sizeof(s_kan[0]); i++) {
        const char *m = strcasestr(url, s_kan[i].mount);
        /* KAN_BET must not match inside a longer mount name */
        if (m && (m[strlen(s_kan[i].mount)] == '.' || m[strlen(s_kan[i].mount)] == '_' ||
                  m[strlen(s_kan[i].mount)] == '?' || m[strlen(s_kan[i].mount)] == 0)) {
            return s_kan[i].channel;
        }
    }
    return 0;
}

typedef struct {
    char *buf;
    size_t len;
} body_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b->len + e->data_len < BODY_MAX) {
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += e->data_len;
    }
    return ESP_OK;
}

/* "2026-10-20T20:53:26.653Z" -> epoch, 0 if unparsable */
static time_t parse_iso_utc(const char *s)
{
    struct tm t = { 0 };
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
        return 0;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    return clock_utc_to_epoch(&t);
}

/* Fetch the current Kan track; returns the delay until the next poll */
static uint32_t poll_kan(int channel, const char *station_url)
{
    char api[96];
    snprintf(api, sizeof(api), "https://www.kan.org.il/api/arc-cloud/get-live-track-data?channelId=%d", channel);
    char *buf = malloc(BODY_MAX);
    if (!buf) {
        return MIN_POLL_MS;
    }
    body_t body = { buf, 0 };
    esp_http_client_config_t cfg = {
        .url = api,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-P4-Internet-Radio/1.0",
        .event_handler = http_event,
        .user_data = &body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    buf[body.len] = 0;

    uint32_t next = MAX_POLL_MS / 2;
    if (err != ESP_OK || status != 200) {
        /* 204 = no song info right now (talk show); anything else = try again later */
        free(buf);
        if (status == 204) {
            s_blocked_ms = 0;
            s_title_end = 0;
            player_set_web_title(station_url, "");      /* talk / no song: really nothing to show */
            return MAX_POLL_MS / 2;
        }
        /* blocked or network trouble: keep the last song until it should have ended */
        if (!s_title_end || !clock_is_valid() || time(NULL) > s_title_end + KEEP_AFTER_END) {
            s_title_end = 0;
            player_set_web_title(station_url, "");
        }
        if (status == 403) {
            s_blocked_ms = s_blocked_ms ? s_blocked_ms * 2 : BLOCKED_MIN_MS;
            if (s_blocked_ms > BLOCKED_MAX_MS) {
                s_blocked_ms = BLOCKED_MAX_MS;
            }
            ESP_LOGW(TAG, "Kan channel %d: blocked by Cloudflare (403), retrying in %lu s",
                     channel, (unsigned long)(s_blocked_ms / 1000));
            return s_blocked_ms;
        }
        ESP_LOGW(TAG, "Kan channel %d: %s, HTTP %d", channel, esp_err_to_name(err), status);
        return MIN_POLL_MS * 2;
    }

    s_blocked_ms = 0;
    cJSON *root = cJSON_Parse(buf);
    const cJSON *title = cJSON_GetObjectItem(root, "title");
    const cJSON *artists = cJSON_GetObjectItem(root, "artists");
    if (cJSON_IsString(title) && title->valuestring[0]) {
        char text[160] = "";
        const cJSON *a;
        cJSON_ArrayForEach(a, artists) {
            if (cJSON_IsString(a) && a->valuestring[0]) {
                if (text[0]) {
                    strlcat(text, ", ", sizeof(text));
                }
                strlcat(text, a->valuestring, sizeof(text));
            }
        }
        if (text[0]) {
            strlcat(text, " - ", sizeof(text));   /* the UI splits "Artist - Title" */
        }
        strlcat(text, title->valuestring, sizeof(text));
        ESP_LOGI(TAG, "Kan %d: %s", channel, text);
        player_set_web_title(station_url, text);

        /* ask again shortly after this track should end */
        const cJSON *end = cJSON_GetObjectItem(root, "endTime");
        const cJSON *delay = cJSON_GetObjectItem(root, "delayTime");
        time_t end_t = parse_iso_utc(cJSON_IsString(end) ? end->valuestring : NULL);
        s_title_end = end_t;
        if (end_t && clock_is_valid()) {
            long ms = (long)(end_t - time(NULL)) * 1000 + (cJSON_IsNumber(delay) ? delay->valueint : 5000);
            next = ms < MIN_POLL_MS ? MIN_POLL_MS : ms > MAX_POLL_MS ? MAX_POLL_MS : (uint32_t)ms;
        }
    } else {
        s_title_end = 0;
        player_set_web_title(station_url, "");
    }
    cJSON_Delete(root);
    free(buf);
    return next;
}

static void nowplaying_task(void *arg)
{
    uint32_t wait_ms = portMAX_DELAY;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms));
        char url[sizeof(s_url)];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int channel = s_channel;
        uint32_t gen = s_gen;
        strlcpy(url, s_url, sizeof(url));
        xSemaphoreGive(s_lock);
        if (!channel) {
            wait_ms = portMAX_DELAY;
            continue;
        }
        /* quick station hopping must not turn into a burst of requests */
        static int64_t s_last_req_us;
        int64_t since_ms = (esp_timer_get_time() - s_last_req_us) / 1000;
        if (s_last_req_us && since_ms < MIN_GAP_MS) {
            wait_ms = MIN_GAP_MS - since_ms;    /* a newer station change wakes us early and re-checks */
            continue;
        }
        s_last_req_us = esp_timer_get_time();
        uint32_t next = poll_kan(channel, url);
        /* a station change during the request restarts the cycle immediately */
        wait_ms = gen == s_gen ? next : 0;
    }
}

void nowplaying_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    /* internal-RAM stack for the TLS handshake */
    xTaskCreatePinnedToCore(nowplaying_task, "nowplaying", 8192, NULL, 2, &s_task, 0);
}

void nowplaying_track(const station_t *st)
{
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_channel = st ? kan_channel(st->url) : 0;
    s_title_end = 0;
    strlcpy(s_url, st ? st->url : "", sizeof(s_url));
    s_gen++;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}
