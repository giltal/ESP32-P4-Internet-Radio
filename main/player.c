#include "player.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "nowplaying.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "board.h"
#include "byte_ring.h"
#include "sdkconfig.h"

static const char *TAG = "player";

#define RING_BYTES        (CONFIG_RADIO_STREAM_BUFFER_KB * 1024)
#define PREBUFFER_BYTES   (CONFIG_RADIO_PREBUFFER_KB * 1024)
#define NET_CHUNK         4096
#define DEC_CHUNK         2048
#define MAX_RETRIES       6
#define MAX_REDIRECTS     6

typedef enum {
    STREAM_LOST = 0,     /* connection ended or failed: worth retrying */
    STREAM_FATAL,        /* retrying will not help (404, HLS, unknown format) */
    STREAM_CANCELLED,    /* user changed station / stopped */
} stream_result_t;

typedef struct {
    int metaint;
    int bitrate;
    char content_type[64];
} http_hdrs_t;

static byte_ring_t *s_ring;
static SemaphoreHandle_t s_lock;
static player_status_t s_st;
static station_t s_req;
static volatile bool s_req_play;
static volatile uint32_t s_session;         /* bumped on every play/stop */
static volatile uint32_t s_data_session;    /* session whose data the ring holds */
static volatile esp_audio_simple_dec_type_t s_dec_type;
static TaskHandle_t s_reader;
static int s_volume = 60;
static volatile bool s_icy_has_title;      /* the stream sends its own (non-empty) titles */
static volatile bool s_paused;
static volatile bool s_go_live;             /* decoder: drop the backlog, resync */

/* Titles arrive in real time but the audio may be played minutes later (pause buffer):
 * each title waits until playback reaches the stream position it arrived at. */
#define TITLE_QUEUE 16
static struct {
    uint64_t at;            /* ring total_in when the title arrived */
    char title[sizeof(((player_status_t *)0)->title)];
} s_titles[TITLE_QUEUE];
static int s_title_count;

/* ------------------------------------------------------------------ */
/* Status helpers                                                      */
/* ------------------------------------------------------------------ */

static void status_set_state(uint32_t sess, player_state_t state, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* the decoder may still be draining audio after the reader gave up:
     * don't let its BUFFERING/PLAYING hide the error */
    bool keep_error = s_st.state == PLAYER_ERROR && (state == PLAYER_BUFFERING || state == PLAYER_PLAYING);
    if (sess == s_session && !keep_error) {
        s_st.state = state;
        strlcpy(s_st.error, err ? err : "", sizeof(s_st.error));
        s_st.revision++;
    }
    xSemaphoreGive(s_lock);
}

static void status_set_title(uint32_t sess, const char *title)
{
    uint64_t in, out;
    byte_ring_totals(s_ring, &in, &out);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *last = s_title_count ? s_titles[s_title_count - 1].title : s_st.title;
    if (sess == s_session && strcmp(last, title) != 0) {
        if (s_title_count == TITLE_QUEUE) {     /* a very long pause: forget the oldest */
            memmove(&s_titles[0], &s_titles[1], sizeof(s_titles[0]) * (TITLE_QUEUE - 1));
            s_title_count--;
        }
        s_titles[s_title_count].at = in;
        strlcpy(s_titles[s_title_count].title, title, sizeof(s_titles[0].title));
        s_title_count++;
    }
    xSemaphoreGive(s_lock);
}

/* Show the newest queued title whose audio is now playing. Call with s_lock held. */
static void titles_apply_locked(uint64_t played)
{
    int due = 0;
    while (due < s_title_count && s_titles[due].at <= played) {
        due++;
    }
    if (due == 0) {
        return;
    }
    if (strcmp(s_st.title, s_titles[due - 1].title) != 0) {
        strlcpy(s_st.title, s_titles[due - 1].title, sizeof(s_st.title));
        s_st.revision++;
    }
    memmove(&s_titles[0], &s_titles[due], sizeof(s_titles[0]) * (s_title_count - due));
    s_title_count -= due;
}

static void status_set_format(uint32_t sess, const char *codec, int kbps, uint32_t rate, int ch)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (sess == s_session) {
        if (codec) {
            strlcpy(s_st.codec, codec, sizeof(s_st.codec));
        }
        if (kbps > 0) {
            s_st.bitrate_kbps = kbps;
        }
        if (rate) {
            s_st.sample_rate = rate;
            s_st.channels = ch;
        }
        s_st.revision++;
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* ICY metadata                                                        */
/* ------------------------------------------------------------------ */

static bool utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) {
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

/*
 * Many Israeli Shoutcast servers still send titles in Windows-1255 (Hebrew)
 * and Russian ones in Windows-1251. Convert those to UTF-8 so LVGL can draw them.
 */
static void legacy_to_utf8(const uint8_t *in, size_t n, char *out, size_t cap)
{
    bool cyrillic = false;
    for (size_t i = 0; i < n; i++) {
        if (in[i] >= 0xC0 && in[i] <= 0xDF) {
            cyrillic = true;   /* cp1255 has no letters there, cp1251 has А..Я */
            break;
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < cap; i++) {
        uint8_t c = in[i];
        uint32_t cp;
        if (c < 0x80) {
            cp = c;
        } else if (cyrillic && c >= 0xC0) {
            cp = 0x0410 + (c - 0xC0);
        } else if (!cyrillic && c >= 0xE0 && c <= 0xFA) {
            cp = 0x05D0 + (c - 0xE0);
        } else {
            cp = '?';
        }
        if (cp < 0x80) {
            out[o++] = cp;
        } else {
            out[o++] = 0xC0 | (cp >> 6);
            out[o++] = 0x80 | (cp & 0x3F);
        }
    }
    out[o] = 0;
}

static void handle_icy_metadata(uint32_t sess, const char *meta, size_t len)
{
    const char *key = "StreamTitle='";
    const char *p = strstr(meta, key);
    if (!p) {
        return;
    }
    p += strlen(key);
    const char *end = strstr(p, "';");
    if (!end) {
        end = strrchr(p, '\'');
    }
    if (!end || end < p) {
        end = meta + len;
    }
    size_t n = end - p;
    char title[sizeof(s_st.title)];
    if (utf8_valid((const uint8_t *)p, n)) {
        if (n >= sizeof(title)) {
            n = sizeof(title) - 1;
            while (n && ((uint8_t)p[n] & 0xC0) == 0x80) {
                n--;
            }
        }
        memcpy(title, p, n);
        title[n] = 0;
    } else {
        legacy_to_utf8((const uint8_t *)p, n, title, sizeof(title));
    }
    /* trim */
    char *t = title;
    while (*t == ' ' || *t == '-') {
        t++;
    }
    size_t tl = strlen(t);
    while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '-')) {
        t[--tl] = 0;
    }
    ESP_LOGI(TAG, "title: %s", t);
    s_icy_has_title = t[0] != 0;
    if (!t[0] && s_st.title[0]) {
        return;     /* empty StreamTitle: keep a title from the station's website */
    }
    status_set_title(sess, t);
}

/* ------------------------------------------------------------------ */
/* HTTP reader                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_hdrs_t *h = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && h && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            h->metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            h->bitrate = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "content-type") == 0) {
            strlcpy(h->content_type, evt->header_value, sizeof(h->content_type));
            for (char *c = h->content_type; *c; c++) {
                *c = tolower((unsigned char)*c);
            }
        }
    }
    return ESP_OK;
}

static bool is_playlist(const char *url, const char *ctype)
{
    if (strstr(ctype, "mpegurl") || strstr(ctype, "scpls") || strstr(ctype, "x-ms-asf")) {
        return true;
    }
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);
    return (len > 4 && (strncasecmp(url + len - 4, ".m3u", 4) == 0 || strncasecmp(url + len - 4, ".pls", 4) == 0));
}

/* Find the first stream URL in an .m3u / .pls body. */
static bool playlist_first_url(const char *body, char *out, size_t cap)
{
    const char *p = body;
    while ((p = strstr(p, "http")) != NULL) {
        if (p[4] == ':' || (p[4] == 's' && p[5] == ':')) {
            size_t n = strcspn(p, "\r\n\"' <");
            if (n < cap) {
                memcpy(out, p, n);
                out[n] = 0;
                return true;
            }
        }
        p += 4;
    }
    return false;
}

/*
 * StreamTheWorld (Kan, many US stations) serves each mount from a pool of numbered
 * edge nodes (NNNNN.live.streamtheworld.com) that come and go. Directory URLs pin one
 * node, which may be long dead, so ask the provisioning API for the current pool and
 * swap the host. Successive calls rotate through the pool so a reconnect tries another node.
 */
#define STW_HOST_SUFFIX ".live.streamtheworld.com"
#define STW_MAX_NODES   8

static bool stw_resolve(char *url, size_t cap)
{
    static unsigned s_rot;
    const char *host = strstr(url, "://");
    if (!host) {
        return false;
    }
    host += 3;
    const char *path = strchr(host, '/');
    size_t sl = strlen(STW_HOST_SUFFIX);
    if (!path || (size_t)(path - host) <= sl || strncasecmp(path - sl, STW_HOST_SUFFIX, sl) != 0) {
        return false;
    }
    char mount[64];
    size_t ml = strcspn(path + 1, "./?");
    if (ml == 0 || ml >= sizeof(mount)) {
        return false;
    }
    memcpy(mount, path + 1, ml);
    mount[ml] = 0;

    char api[160];
    snprintf(api, sizeof(api),
             "https://playerservices.streamtheworld.com/api/livestream?version=1.9&mount=%s&lang=en", mount);
    esp_http_client_config_t cfg = {
        .url = api,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-P4-Internet-Radio/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    const size_t body_cap = 12 * 1024;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t len = 0;
    if (body && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            int r;
            while (len < body_cap - 1 && (r = esp_http_client_read(c, body + len, body_cap - 1 - len)) > 0) {
                len += r;
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!body) {
        return false;
    }
    body[len] = 0;

    /* collect <ip>node</ip> entries in the API's preference order */
    char nodes[STW_MAX_NODES][64];
    int count = 0;
    for (const char *p = body; count < STW_MAX_NODES && (p = strstr(p, "<ip>")) != NULL;) {
        p += 4;
        const char *e = strstr(p, "</ip>");
        if (!e) {
            break;
        }
        if (e > p && (size_t)(e - p) < sizeof(nodes[0])) {
            memcpy(nodes[count], p, e - p);
            nodes[count][e - p] = 0;
            count++;
        }
        p = e;
    }
    free(body);
    if (count == 0) {
        ESP_LOGW(TAG, "StreamTheWorld: no servers listed for %s", mount);
        return false;
    }

    char resolved[sizeof(((station_t *)0)->url)];
    int n = snprintf(resolved, sizeof(resolved), "https://%s%s", nodes[s_rot++ % count], path);
    if (n <= 0 || (size_t)n >= sizeof(resolved) || (size_t)n >= cap) {
        return false;
    }
    strlcpy(url, resolved, cap);
    ESP_LOGI(TAG, "StreamTheWorld %s: %d servers, using %s", mount, count, url);
    return true;
}

static esp_audio_simple_dec_type_t pick_decoder(const char *ctype, const char *station_codec,
                                                const uint8_t *peek, size_t peek_len)
{
    if (strstr(ctype, "aac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (strstr(ctype, "mpeg") || strstr(ctype, "mp3")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    /* Content-Type missing or generic: sniff the first bytes */
    if (peek_len >= 3 && memcmp(peek, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    for (size_t i = 0; i + 1 < peek_len && i < 1024; i++) {
        if (peek[i] == 0xFF && (peek[i + 1] & 0xF6) == 0xF0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;   /* ADTS sync */
        }
        if (peek[i] == 0xFF && (peek[i + 1] & 0xE6) == 0xE2) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;   /* MPEG layer III sync */
        }
    }
    if (strcasecmp(station_codec, "AAC") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (strcasecmp(station_codec, "MP3") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* Push audio bytes into the ring, giving up if the user switched station. */
static bool push_audio(uint32_t sess, const uint8_t *p, size_t n)
{
    while (n) {
        if (sess != s_session) {
            return false;
        }
        if (s_paused) {
            /* pause buffer full: it becomes a FIFO, the oldest audio makes room */
            size_t space = byte_ring_size(s_ring) - byte_ring_filled(s_ring);
            if (space < n) {
                byte_ring_drop(s_ring, n - space);
            }
        }
        size_t w = byte_ring_write(s_ring, p, n, pdMS_TO_TICKS(100));
        p += w;
        n -= w;
    }
    return true;
}

static stream_result_t stream_once(uint32_t sess, const station_t *st, bool *reached_audio)
{
    static uint8_t buf[NET_CHUNK];
    static char meta[16 * 255 + 1];
    char url[sizeof(st->url)];
    strlcpy(url, st->url, sizeof(url));
    stw_resolve(url, sizeof(url));   /* no-op for anything but StreamTheWorld */
    http_hdrs_t hdrs;
    stream_result_t result = STREAM_LOST;

    for (int hop = 0; hop < 3; hop++) {   /* playlist -> stream hops */
        memset(&hdrs, 0, sizeof(hdrs));
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .user_agent = "ESP32-P4-Internet-Radio/1.0",
            .buffer_size = 8192,
            .buffer_size_tx = 1024,
            .disable_auto_redirect = true,
            .event_handler = http_event,
            .user_data = &hdrs,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            return STREAM_LOST;
        }
        esp_http_client_set_header(c, "Icy-MetaData", "1");
        esp_http_client_set_header(c, "Accept", "*/*");

        int status = 0;
        for (int redir = 0; redir <= MAX_REDIRECTS; redir++) {
            memset(&hdrs, 0, sizeof(hdrs));
            if (esp_http_client_open(c, 0) != ESP_OK) {
                status = -1;
                break;
            }
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                /* StreamTheWorld & co. bounce us to a rotating edge server */
                esp_http_client_set_redirection(c);
                esp_http_client_close(c);
                continue;
            }
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
            esp_http_client_cleanup(c);
            if (status == 404 || status == 403 || status == 410) {
                status_set_state(sess, PLAYER_ERROR, status == 404 ? "השידור לא נמצא (404)" : "השרת חסם את הגישה");
                return STREAM_FATAL;
            }
            return STREAM_LOST;
        }

        if (is_playlist(url, hdrs.content_type)) {
            int r = esp_http_client_read(c, (char *)buf, sizeof(buf) - 1);
            buf[r > 0 ? r : 0] = 0;
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            if (strstr((char *)buf, "#EXT-X-")) {
                status_set_state(sess, PLAYER_ERROR, "שידור HLS אינו נתמך");
                return STREAM_FATAL;
            }
            if (!playlist_first_url((char *)buf, url, sizeof(url))) {
                status_set_state(sess, PLAYER_ERROR, "רשימת השמעה ריקה");
                return STREAM_FATAL;
            }
            ESP_LOGI(TAG, "playlist -> %s", url);
            continue;
        }

        /* Peek at the first bytes (needed when Content-Type doesn't say) */
        int metaint = hdrs.metaint;
        int peek = esp_http_client_read(c, (char *)buf, metaint > 0 && metaint < NET_CHUNK ? metaint : NET_CHUNK);
        if (peek <= 0) {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return STREAM_LOST;
        }
        esp_audio_simple_dec_type_t type = pick_decoder(hdrs.content_type, st->codec, buf, peek);
        if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
            ESP_LOGW(TAG, "unsupported content-type '%s'", hdrs.content_type);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            status_set_state(sess, PLAYER_ERROR, "פורמט שמע לא נתמך");
            return STREAM_FATAL;
        }
        const char *codec_name = type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? "AAC" : "MP3";
        ESP_LOGI(TAG, "streaming %s (%s, %d kbps, metaint %d)", url, codec_name, hdrs.bitrate, metaint);
        status_set_format(sess, codec_name, hdrs.bitrate > 0 ? hdrs.bitrate : st->bitrate, 0, 0);

        /* Hand the ring to this session: new station -> drop old audio.
         * A reconnect within the same session keeps what is still buffered. */
        if (s_data_session != sess || s_dec_type != type) {
            byte_ring_reset(s_ring);
            s_dec_type = type;
            s_data_session = sess;
        }
        *reached_audio = true;

        int until_meta = metaint;   /* audio bytes left before the next metadata block */
        int meta_len = 0, meta_got = 0;
        bool meta_len_pending = false;
        int timeouts = 0;
        int n = peek;
        result = STREAM_LOST;
        for (;;) {
            /* walk the chunk, splitting audio from ICY metadata */
            int i = 0;
            while (i < n) {
                if (metaint <= 0) {
                    if (!push_audio(sess, buf + i, n - i)) {
                        result = STREAM_CANCELLED;
                    }
                    i = n;
                } else if (meta_len_pending) {
                    meta_len = buf[i++] * 16;
                    meta_got = 0;
                    meta_len_pending = false;
                    if (meta_len == 0) {
                        until_meta = metaint;
                    }
                } else if (meta_len > 0) {
                    int take = n - i < meta_len - meta_got ? n - i : meta_len - meta_got;
                    memcpy(meta + meta_got, buf + i, take);
                    meta_got += take;
                    i += take;
                    if (meta_got == meta_len) {
                        meta[meta_len] = 0;
                        handle_icy_metadata(sess, meta, meta_len);
                        meta_len = 0;
                        until_meta = metaint;
                    }
                } else {
                    int take = n - i < until_meta ? n - i : until_meta;
                    if (!push_audio(sess, buf + i, take)) {
                        result = STREAM_CANCELLED;
                        i = n;
                        break;
                    }
                    i += take;
                    until_meta -= take;
                    if (until_meta == 0) {
                        meta_len_pending = true;
                    }
                }
            }
            if (result == STREAM_CANCELLED || sess != s_session) {
                result = STREAM_CANCELLED;
                break;
            }
            n = esp_http_client_read(c, (char *)buf, sizeof(buf));
            if (n == -ESP_ERR_HTTP_EAGAIN) {
                if (++timeouts > 3) {
                    ESP_LOGW(TAG, "stream stalled");
                    break;
                }
                n = 0;
                continue;
            }
            if (n <= 0) {
                ESP_LOGW(TAG, "stream ended (%d)", n);
                break;
            }
            timeouts = 0;
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return result;
    }
    status_set_state(sess, PLAYER_ERROR, "רשימת השמעה מקוננת מדי");
    return STREAM_FATAL;
}

static void reader_task(void *arg)
{
    uint32_t handled = 0;
    for (;;) {
        uint32_t sess = s_session;
        if (sess == handled) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        handled = sess;
        station_t st;
        bool play;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        st = s_req;
        play = s_req_play;
        xSemaphoreGive(s_lock);
        if (!play) {
            continue;
        }

        int attempt = 0;
        while (sess == s_session) {
            status_set_state(sess, PLAYER_CONNECTING, attempt ? "מתחבר מחדש…" : NULL);
            bool reached_audio = false;
            stream_result_t r = stream_once(sess, &st, &reached_audio);
            if (r == STREAM_CANCELLED || r == STREAM_FATAL || sess != s_session) {
                break;
            }
            attempt = reached_audio ? 1 : attempt + 1;
            if (attempt > MAX_RETRIES) {
                status_set_state(sess, PLAYER_ERROR, "אין חיבור לתחנה הזו");
                break;
            }
            /* back off, but wake immediately if the user picks another station */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500 * attempt * attempt));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Decoder -> codec                                                    */
/* ------------------------------------------------------------------ */

static uint8_t *s_pcm;
static size_t s_pcm_cap;
static int16_t *s_out;
static size_t s_out_cap;

static void output_pcm(uint32_t sess, const uint8_t *pcm, size_t bytes, const esp_audio_simple_dec_info_t *info)
{
    if (info->bits_per_sample != 16 || info->channel < 1 || info->channel > 2) {
        return;
    }
    size_t frames = bytes / (2 * info->channel);
    size_t need = frames * 4;
    if (need > s_out_cap) {
        int16_t *n = heap_caps_realloc(s_out, need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!n) {
            return;
        }
        s_out = n;
        s_out_cap = need;
    }
    /* The board has a single speaker: fold stereo to mono and send it on both slots */
    const int16_t *in = (const int16_t *)pcm;
    int peak = 0;
    for (size_t f = 0; f < frames; f++) {
        int v = info->channel == 2 ? (in[2 * f] + in[2 * f + 1]) / 2 : in[f];
        s_out[2 * f] = s_out[2 * f + 1] = (int16_t)v;
        int a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    s_st.level = (uint8_t)(peak * 100 / 32768);
    if (sess == s_session) {
        board_audio_write(s_out, need);
    }
}

static void decode_session(uint32_t sess)
{
    status_set_state(sess, PLAYER_BUFFERING, NULL);
    while (sess == s_session && byte_ring_filled(s_ring) < PREBUFFER_BYTES) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (sess != s_session) {
        return;
    }

    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.aac_plus_enable = true;   /* HE-AAC (SBR/PS), common on low-bitrate stations */
    esp_audio_simple_dec_cfg_t cfg = { .dec_type = s_dec_type };
    if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        cfg.dec_cfg = &aac_cfg;
        cfg.cfg_size = sizeof(aac_cfg);
    }
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        status_set_state(sess, PLAYER_ERROR, "שגיאה בהפעלת המפענח");
        return;
    }

    static uint8_t in[DEC_CHUNK];
    esp_audio_simple_dec_info_t info = { 0 };
    bool playing = false;
    int errors = 0;
    int empty_reads = 0;
    bool rebuffering = false;
    bool was_paused = false;
    while (sess == s_session) {
        if (s_go_live) {
            /* keep just a prebuffer's worth of the newest audio; the cut lands mid-frame */
            s_go_live = false;
            size_t fill = byte_ring_filled(s_ring);
            if (fill > PREBUFFER_BYTES) {
                byte_ring_drop(s_ring, fill - PREBUFFER_BYTES);
            }
            esp_audio_simple_dec_reset(dec);
        }
        if (s_paused) {
            if (!was_paused) {
                was_paused = true;
                board_audio_close();    /* amp off: no hiss or stale DMA loop while paused */
                s_st.level = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (was_paused) {
            was_paused = false;
            if (info.sample_rate) {
                board_audio_open(info.sample_rate);
            }
        }
        if (rebuffering) {
            /* after an underrun, wait for a cushion before resuming */
            if (byte_ring_filled(s_ring) < PREBUFFER_BYTES / 2) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            rebuffering = false;
        }
        size_t got = byte_ring_read(s_ring, in, sizeof(in), pdMS_TO_TICKS(50));
        if (got == 0) {
            if (++empty_reads >= 4 && playing) {
                status_set_state(sess, PLAYER_BUFFERING, NULL);
                playing = false;
                rebuffering = true;
            }
            continue;
        }
        empty_reads = 0;

        esp_audio_simple_dec_raw_t raw = { .buffer = in, .len = got };
        while (raw.len && sess == s_session) {
            esp_audio_simple_dec_out_t out = { .buffer = s_pcm, .len = s_pcm_cap };
            esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *n = heap_caps_realloc(s_pcm, out.needed_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!n) {
                    break;
                }
                s_pcm = n;
                s_pcm_cap = out.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                /* corrupt data (e.g. right after a reconnect): drop the chunk, resync */
                if (++errors > 50) {
                    ESP_LOGW(TAG, "decoder keeps failing (%d), resetting", ret);
                    esp_audio_simple_dec_reset(dec);
                    errors = 0;
                }
                break;
            }
            if (out.decoded_size) {
                errors = 0;
                esp_audio_simple_dec_info_t ni = { 0 };
                esp_audio_simple_dec_get_info(dec, &ni);
                if (ni.sample_rate != info.sample_rate || ni.channel != info.channel) {
                    info = ni;
                    ESP_LOGI(TAG, "format %lu Hz, %u ch, %u bit, %lu bps", (unsigned long)info.sample_rate,
                             info.channel, info.bits_per_sample, (unsigned long)info.bitrate);
                    board_audio_open(info.sample_rate);
                    status_set_format(sess, NULL, info.bitrate ? (int)(info.bitrate / 1000) : 0,
                                      info.sample_rate, info.channel);
                }
                if (!playing) {
                    playing = true;
                    status_set_state(sess, PLAYER_PLAYING, NULL);
                }
                output_pcm(sess, out.buffer, out.decoded_size, &info);
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (raw.consumed == 0 && out.decoded_size == 0) {
                break;   /* decoder wants more input */
            }
        }
    }
    esp_audio_simple_dec_close(dec);
    s_st.level = 0;
}

static void decoder_task(void *arg)
{
    for (;;) {
        uint32_t sess = s_data_session;
        if (sess == 0 || sess != s_session || !s_req_play) {
            if (!s_req_play) {
                board_audio_close();   /* also switches the speaker amp off */
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        decode_session(sess);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ring = byte_ring_create(RING_BYTES);
    s_pcm_cap = 16 * 1024;
    s_pcm = heap_caps_malloc(s_pcm_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_lock || !s_ring || !s_pcm) {
        return ESP_ERR_NO_MEM;
    }
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    board_audio_set_volume(s_volume);

    /* Networking and decoding on separate cores so a slow TLS read never starves audio */
    xTaskCreatePinnedToCore(reader_task, "radio_net", 8192, NULL, 6, &s_reader, 0);
    xTaskCreatePinnedToCore(decoder_task, "radio_dec", 8192, NULL, 7, NULL, 1);
    return ESP_OK;
}

void player_play(const station_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req = *st;
    s_req_play = true;
    s_session++;
    memset(&s_st, 0, sizeof(s_st));
    s_st.station = *st;
    s_st.state = PLAYER_CONNECTING;
    strlcpy(s_st.codec, st->codec, sizeof(s_st.codec));
    s_st.bitrate_kbps = st->bitrate;
    s_st.revision = s_session * 1000;
    s_icy_has_title = false;
    s_paused = false;
    s_go_live = false;
    s_title_count = 0;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_reader);
    nowplaying_track(st);

    sdcard_save("last.bin", st, sizeof(*st));
}

void player_set_web_title(const char *station_url, const char *title)
{
    if (s_icy_has_title) {
        return;     /* in-stream metadata is more precise */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_req_play && strcmp(s_req.url, station_url) == 0;
    xSemaphoreGive(s_lock);
    if (same) {
        status_set_title(s_session, title);
    }
}

void player_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req_play = false;
    s_session++;
    s_st.state = PLAYER_STOPPED;
    s_st.title[0] = 0;
    s_st.error[0] = 0;
    s_st.level = 0;
    s_st.revision++;
    s_paused = false;
    s_go_live = false;
    s_title_count = 0;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_reader);
    nowplaying_track(NULL);
}

void player_pause(bool pause)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_req_play && s_paused != pause) {
        s_paused = pause;
        s_st.revision++;
    }
    xSemaphoreGive(s_lock);
}

void player_go_live(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_req_play) {
        s_go_live = true;
        s_paused = false;
        s_st.revision++;
    }
    xSemaphoreGive(s_lock);
}

void player_set_volume(int volume)
{
    s_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    board_audio_set_volume(s_volume);
}

int player_get_volume(void)
{
    return s_volume;
}

void player_load_settings(void)
{
    player_set_volume(sdcard_load_int("volume.txt", s_volume));
}

void player_save_volume(void)
{
    sdcard_save_int("volume.txt", s_volume);
}

void player_get_status(player_status_t *out)
{
    uint64_t in, played;
    byte_ring_totals(s_ring, &in, &played);
    size_t fill = (size_t)(in - played);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_go_live) {
        titles_apply_locked(played);
    }
    *out = s_st;
    out->paused = s_paused;
    xSemaphoreGive(s_lock);
    out->buffer_pct = (uint8_t)(fill * 100 / byte_ring_size(s_ring));
    /* the normal prebuffer cushion is not "behind live" */
    uint32_t bytes_per_s = out->bitrate_kbps ? out->bitrate_kbps * 125u : 16000u;
    out->behind_s = fill > PREBUFFER_BYTES * 2 ? (fill - PREBUFFER_BYTES) / bytes_per_s : 0;
}

bool player_last_station(station_t *out)
{
    size_t sz = 0;
    station_t *st = sdcard_load("last.bin", &sz);
    bool ok = st && sz == sizeof(*out);
    if (ok) {
        *out = *st;
        out->name[sizeof(out->name) - 1] = 0;
        out->url[sizeof(out->url) - 1] = 0;
        out->codec[sizeof(out->codec) - 1] = 0;
    }
    free(st);
    return ok;
}
