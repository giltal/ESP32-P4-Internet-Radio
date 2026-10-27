#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdcard.h"

static const char *TAG = "weather";

#define REFRESH_MS   (30 * 60 * 1000)
#define RETRY_MS     (2 * 60 * 1000)
#define BODY_MAX     8192

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static weather_t s_wx;
static volatile uint32_t s_rev;
static char s_place[64] = "תל אביב";
static double s_lat = 32.0853, s_lon = 34.7818;
static char s_query[64];            /* pending city search, "" = none */
static char s_status[96];

/* ------------------------------------------------------------------ */

const char *weather_text(int code)
{
    switch (code) {
    case 0: return "בהיר";
    case 1: return "בהיר ברובו";
    case 2: return "מעונן חלקית";
    case 3: return "מעונן";
    case 45: case 48: return "ערפל";
    case 51: case 53: case 55: case 56: case 57: return "טפטוף";
    case 61: return "גשם קל";
    case 63: return "גשם";
    case 65: return "גשם חזק";
    case 66: case 67: return "גשם קופא";
    case 71: case 73: case 75: case 77: return "שלג";
    case 80: case 81: return "ממטרים";
    case 82: return "ממטרים חזקים";
    case 85: case 86: return "ממטרי שלג";
    case 95: return "סופת רעמים";
    case 96: case 99: return "סופת רעמים וברד";
    default: return "";
    }
}

/* FontAwesome 5 glyphs in font_wx_* (see tools/gen_fonts_wx.sh) */
#define FA_SUN            "\xEF\x86\x85"   /* F185 */
#define FA_MOON           "\xEF\x86\x86"   /* F186 */
#define FA_CLOUD_SUN      "\xEF\x9B\x84"   /* F6C4 */
#define FA_CLOUD_MOON     "\xEF\x9B\x83"   /* F6C3 */
#define FA_CLOUD          "\xEF\x83\x82"   /* F0C2 */
#define FA_SMOG           "\xEF\x9D\x9F"   /* F75F */
#define FA_CLOUD_RAIN     "\xEF\x9C\xBD"   /* F73D */
#define FA_CLOUD_SHOWERS  "\xEF\x9D\x80"   /* F740 */
#define FA_CLOUD_SUN_RAIN "\xEF\x9D\x83"   /* F743 */
#define FA_SNOWFLAKE      "\xEF\x8B\x9C"   /* F2DC */
#define FA_BOLT           "\xEF\x83\xA7"   /* F0E7 */

const char *weather_icon(int code, bool is_day)
{
    if (code == 0) return is_day ? FA_SUN : FA_MOON;
    if (code == 1 || code == 2) return is_day ? FA_CLOUD_SUN : FA_CLOUD_MOON;
    if (code == 3) return FA_CLOUD;
    if (code == 45 || code == 48) return FA_SMOG;
    if (code >= 51 && code <= 61) return FA_CLOUD_RAIN;
    if (code >= 63 && code <= 67) return FA_CLOUD_SHOWERS;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return FA_SNOWFLAKE;
    if (code >= 80 && code <= 82) return is_day ? FA_CLOUD_SUN_RAIN : FA_CLOUD_RAIN;
    if (code >= 95) return FA_BOLT;
    return FA_CLOUD;
}

uint32_t weather_icon_color(int code, bool is_day)
{
    if (code == 0) return is_day ? 0xFFB52E : 0xF2E6B8;                 /* warm sun / pale moon */
    if (code == 1 || code == 2) return is_day ? 0xFFCF6B : 0xB8C4E0;    /* sun through clouds / moonlit */
    if (code == 3) return 0xC9D1DC;                                     /* overcast grey */
    if (code == 45 || code == 48) return 0x9AA4B1;                      /* fog */
    if (code >= 51 && code <= 61) return 0x6CB4EE;                      /* drizzle, light rain */
    if (code >= 63 && code <= 67) return 0x3D84F5;                      /* heavy rain */
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 0xBFE6FF;   /* icy snow */
    if (code >= 80 && code <= 82) return is_day ? 0x5FC6D8 : 0x6CB4EE;  /* sunny showers */
    if (code >= 95) return 0xFFE14D;                                    /* lightning */
    return 0xC9D1DC;
}

/* ------------------------------------------------------------------ */

static void set_status(const char *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status, s ? s : "", sizeof(s_status));
    xSemaphoreGive(s_lock);
    s_rev++;
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

/* GET url -> parsed JSON (caller deletes), NULL on failure */
static cJSON *get_json(const char *url)
{
    char *buf = malloc(BODY_MAX);
    if (!buf) {
        return NULL;
    }
    body_t body = { buf, 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Nominatim's usage policy asks for an identifying User-Agent */
        .user_agent = "ESP32-P4-Internet-Radio/1.0 (github.com/giltal/ESP32-P4-Internet-Radio)",
        .event_handler = http_event,
        .user_data = &body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    cJSON *root = NULL;
    if (err == ESP_OK && status == 200) {
        buf[body.len] = 0;
        root = cJSON_Parse(buf);
    } else {
        ESP_LOGW(TAG, "%s -> %s, HTTP %d", url, esp_err_to_name(err), status);
    }
    free(buf);
    return root;
}

static double num(const cJSON *o, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static double arr_num(const cJSON *o, const char *key, int i, double def)
{
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(o, key), i);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool fetch_forecast(void)
{
    char url[400];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    double lat = s_lat, lon = s_lon;
    xSemaphoreGive(s_lock);
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=%d", lat, lon, WEATHER_DAYS);
    cJSON *root = get_json(url);
    if (!root) {
        return false;
    }
    weather_t w = { 0 };
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (cur && daily) {
        w.temp = num(cur, "temperature_2m", 0);
        w.code = (int)num(cur, "weather_code", 0);
        w.is_day = num(cur, "is_day", 1) != 0;
        w.humidity = (int)num(cur, "relative_humidity_2m", 0);
        w.wind_kmh = num(cur, "wind_speed_10m", 0);
        const cJSON *dates = cJSON_GetObjectItem(daily, "time");
        int n = cJSON_GetArraySize(dates);
        for (int i = 0; i < n && i < WEATHER_DAYS; i++) {
            weather_day_t *d = &w.days[i];
            const cJSON *t = cJSON_GetArrayItem(dates, i);
            if (!cJSON_IsString(t) || sscanf(t->valuestring, "%d-%d-%d", &d->year, &d->month, &d->day) != 3) {
                break;
            }
            d->code = (int)arr_num(daily, "weather_code", i, 0);
            d->tmax = arr_num(daily, "temperature_2m_max", i, 0);
            d->tmin = arr_num(daily, "temperature_2m_min", i, 0);
            d->rain_pct = (int)arr_num(daily, "precipitation_probability_max", i, -1);
            w.ndays = i + 1;
        }
        w.valid = w.ndays > 0;
    }
    cJSON_Delete(root);
    if (!w.valid) {
        return false;
    }
    w.updated = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(w.place, s_place, sizeof(w.place));
    s_wx = w;
    xSemaphoreGive(s_lock);
    s_rev++;
    ESP_LOGI(TAG, "%s: %.1f C, %s, %d days", w.place, w.temp, weather_text(w.code), w.ndays);
    return true;
}

static void save_location(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n%.5f\n%.5f\n", s_place, s_lat, s_lon);
    sdcard_save("weather.txt", buf, n);
}

static void load_location(void)
{
    char *txt = sdcard_load("weather.txt", NULL);
    if (!txt) {
        return;
    }
    char *name = strtok(txt, "\r\n");
    char *lat = strtok(NULL, "\r\n");
    char *lon = strtok(NULL, "\r\n");
    if (name && lat && lon) {
        strlcpy(s_place, name, sizeof(s_place));
        s_lat = atof(lat);
        s_lon = atof(lon);
    }
    free(txt);
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                *p == '-' || *p == '_' || *p == '.') {
            out[o++] = *p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

/* City search result: 1 = found, 0 = no such place, -1 = service unreachable */
typedef struct {
    char name[64];
    double lat, lon;
} place_t;

/* OpenStreetMap Nominatim: best coverage of (Israeli) towns; settlements only, so a query
 * like "Even Yehuda" can't land on a street called "Ben Yehuda". Policy: identify the app,
 * at most one request per second (a manual search from the settings is far below that). */
static int search_nominatim(const char *enc, place_t *out)
{
    char url[360];
    snprintf(url, sizeof(url), "https://nominatim.openstreetmap.org/search?q=%s"
             "&format=jsonv2&limit=1&accept-language=he&featureType=settlement", enc);
    cJSON *root = get_json(url);
    if (!root) {
        return -1;
    }
    const cJSON *r = cJSON_GetArrayItem(root, 0);
    const cJSON *name = cJSON_GetObjectItem(r, "name");
    const cJSON *lat = cJSON_GetObjectItem(r, "lat");
    const cJSON *lon = cJSON_GetObjectItem(r, "lon");
    int found = cJSON_IsString(name) && name->valuestring[0] && cJSON_IsString(lat) && cJSON_IsString(lon);
    if (found) {
        strlcpy(out->name, name->valuestring, sizeof(out->name));
        out->lat = atof(lat->valuestring);
        out->lon = atof(lon->valuestring);
    }
    cJSON_Delete(root);
    return found;
}

/* Open-Meteo geocoding (GeoNames): fallback, some Hebrew names are missing there */
static int search_open_meteo(const char *enc, const char *lang, place_t *out)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=%s&format=json", enc, lang);
    cJSON *root = get_json(url);
    if (!root) {
        return -1;
    }
    const cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "results"), 0);
    const cJSON *name = cJSON_GetObjectItem(r, "name");
    int found = cJSON_IsString(name) && name->valuestring[0];
    if (found) {
        strlcpy(out->name, name->valuestring, sizeof(out->name));
        out->lat = num(r, "latitude", 0);
        out->lon = num(r, "longitude", 0);
    }
    cJSON_Delete(root);
    return found;
}

static void do_search(const char *query)
{
    /* the on-screen keyboard can leave stray spaces around the name */
    char q[sizeof(s_query)];
    while (*query == ' ') {
        query++;
    }
    strlcpy(q, query, sizeof(q));
    for (size_t n = strlen(q); n && q[n - 1] == ' '; n--) {
        q[n - 1] = 0;
    }
    if (!q[0]) {
        return;
    }
    char enc[200];
    url_encode(q, enc, sizeof(enc));
    set_status("מחפש…");

    place_t p = { 0 };
    int r = search_nominatim(enc, &p);
    const char *src = "OpenStreetMap";
    if (r != 1) {
        int r2 = search_open_meteo(enc, "he", &p);
        if (r2 != 1) {
            r2 = search_open_meteo(enc, "en", &p);
        }
        src = "Open-Meteo";
        r = (r2 == 1) ? 1 : (r == -1 && r2 == -1) ? -1 : 0;
    }
    if (r != 1) {
        set_status(r == -1 ? "אין חיבור לשירות החיפוש" : "העיר לא נמצאה");
        ESP_LOGW(TAG, "city search '%s': %s", q, r == -1 ? "unreachable" : "not found");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_place, p.name, sizeof(s_place));
    s_lat = p.lat;
    s_lon = p.lon;
    xSemaphoreGive(s_lock);
    save_location();
    ESP_LOGI(TAG, "location (%s): %s (%.4f, %.4f)", src, s_place, s_lat, s_lon);
    set_status("");
    fetch_forecast();
}

static void weather_task(void *arg)
{
    uint32_t wait = portMAX_DELAY;      /* until weather_refresh() (Wi-Fi up) */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(wait));
        char q[sizeof(s_query)];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(q, s_query, sizeof(q));
        s_query[0] = 0;
        xSemaphoreGive(s_lock);
        if (q[0]) {
            do_search(q);
            wait = REFRESH_MS;
            continue;
        }
        wait = fetch_forecast() ? REFRESH_MS : RETRY_MS;
    }
}

void weather_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_location();
    strlcpy(s_wx.place, s_place, sizeof(s_wx.place));
    /* internal-RAM stack for the TLS handshake */
    xTaskCreatePinnedToCore(weather_task, "weather", 8192, NULL, 2, &s_task, 0);
}

void weather_refresh(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

uint32_t weather_revision(void)
{
    return s_rev;
}

void weather_get(weather_t *out)
{
    if (!s_lock) {                  /* UI polls before weather_init(): nothing yet */
        memset(out, 0, sizeof(*out));
        strlcpy(out->place, s_place, sizeof(out->place));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_wx;
    if (!out->place[0]) {
        strlcpy(out->place, s_place, sizeof(out->place));
    }
    xSemaphoreGive(s_lock);
}

void weather_search_city(const char *query)
{
    if (!s_task || !query || !query[0]) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_query, query, sizeof(s_query));
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void weather_status(char *buf, size_t len)
{
    if (!s_lock) {
        buf[0] = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_status, len);
    xSemaphoreGive(s_lock);
}
