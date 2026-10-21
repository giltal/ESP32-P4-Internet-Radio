#include "logo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"
#include "lvgl.h"
#include "libs/lodepng/lodepng.h"
#include "sdcard.h"

static const char *TAG = "logo";

#define URL_LEN       224
#define MAX_DOWNLOAD  (320 * 1024)
#define MIN_SOURCE    32                    /* smaller favicons look awful scaled up */
#define THUMB_BYTES   (LOGO_SIZE * LOGO_SIZE * 4)

typedef struct {
    char magic[4];       /* "LG1" */
    uint16_t w, h;
} thumb_hdr_t;

/*
 * Stations that only publish SVG logos: Wikimedia Commons renders those to PNG
 * (250 px thumbnails of the logo files on commons.wikimedia.org).
 */
static const struct {
    const char *stream;         /* substring of the stream URL */
    const char *png;
} s_overrides[] = {
    { "KAN_GIMMEL", "https://upload.wikimedia.org/wikipedia/commons/thumb/3/39/KanGimel.svg/250px-KanGimel.svg.png" },
    { "KAN_88", "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0b/Kan88.svg/250px-Kan88.svg.png" },
    { "KAN_BET", "https://upload.wikimedia.org/wikipedia/commons/thumb/4/49/KanReshetBetLogo.svg/250px-KanReshetBetLogo.svg.png" },
    { "kan_reshet_bet", "https://upload.wikimedia.org/wikipedia/commons/thumb/4/49/KanReshetBetLogo.svg/250px-KanReshetBetLogo.svg.png" },
    { "KAN_TARBUT", "https://upload.wikimedia.org/wikipedia/commons/thumb/e/ed/KanTarbutNewLogo.svg/250px-KanTarbutNewLogo.svg.png" },
    { "glglz", "https://upload.wikimedia.org/wikipedia/commons/thumb/e/e2/GLGLZ.svg/250px-GLGLZ.svg.png" },
};

const char *logo_url_for(const char *stream_url, const char *favicon)
{
    for (size_t i = 0; stream_url && i < sizeof(s_overrides) / sizeof(s_overrides[0]); i++) {
        if (strstr(stream_url, s_overrides[i].stream)) {
            return s_overrides[i].png;
        }
    }
    return favicon ? favicon : "";
}

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock;
static uint8_t *s_thumb;            /* PSRAM, THUMB_BYTES */
static bool s_have;
static char s_have_url[URL_LEN];
static volatile uint32_t s_rev;
static jpeg_decoder_handle_t s_jpeg;

/* ------------------------------------------------------------------ */
/* Source image -> fitted thumbnail                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *px;
    int w, h, bpp;       /* 4 = R,G,B,A (LodePNG), 3 = B,G,R (JPEG engine, BGR order) */
    int stride;          /* bytes per row */
} src_img_t;

static inline void src_px(const src_img_t *s, int x, int y, int *r, int *g, int *b, int *a)
{
    const uint8_t *p = s->px + (size_t)y * s->stride + (size_t)x * s->bpp;
    if (s->bpp == 4) {
        *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    } else {
        *b = p[0]; *g = p[1]; *r = p[2]; *a = 255;
    }
}

/* Fit (contain) into LOGO_SIZE x LOGO_SIZE, transparent margins, box-filter sampling */
static void make_thumb(const src_img_t *s, uint8_t *out)
{
    memset(out, 0, THUMB_BYTES);
    int fw, fh;
    if (s->w >= s->h) {
        fw = LOGO_SIZE;
        fh = s->h * LOGO_SIZE / s->w;
    } else {
        fh = LOGO_SIZE;
        fw = s->w * LOGO_SIZE / s->h;
    }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    int ox = (LOGO_SIZE - fw) / 2, oy = (LOGO_SIZE - fh) / 2;
    for (int dy = 0; dy < fh; dy++) {
        int y0 = dy * s->h / fh, y1 = (dy + 1) * s->h / fh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < fw; dx++) {
            int x0 = dx * s->w / fw, x1 = (dx + 1) * s->w / fw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int r, g, b, a;
                    src_px(s, x, y, &r, &g, &b, &a);
                    sr += r * a; sg += g * a; sb += b * a; sa += a;   /* alpha-weighted */
                    n++;
                }
            }
            uint8_t *d = out + ((size_t)(oy + dy) * LOGO_SIZE + ox + dx) * 4;
            if (sa) {
                d[0] = sb / sa;   /* LVGL ARGB8888 is B,G,R,A in memory */
                d[1] = sg / sa;
                d[2] = sr / sa;
                d[3] = sa / n;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Decoders                                                            */
/* ------------------------------------------------------------------ */

static bool decode_png(const uint8_t *data, size_t len, uint8_t *thumb)
{
    /* LVGL's bundled LodePNG hands back an lv_draw_buf_t (R,G,B,A rows), not a raw pixel array */
    unsigned char *out = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&out, &w, &h, data, len);
    lv_draw_buf_t *db = (lv_draw_buf_t *)out;
    bool ok = err == 0 && db && w >= MIN_SOURCE && h >= MIN_SOURCE;
    if (ok) {
        src_img_t s = { db->data, (int)w, (int)h, 4, (int)db->header.stride };
        make_thumb(&s, thumb);
    } else if (err) {
        ESP_LOGW(TAG, "PNG decode error %u", err);
    }
    if (db) {
        lv_draw_buf_destroy(db);
    }
    return ok;
}

/* Modern .ico files usually wrap a PNG; pick the largest PNG entry */
static bool decode_ico(const uint8_t *data, size_t len, uint8_t *thumb)
{
    if (len < 6) {
        return false;
    }
    int count = data[4] | (data[5] << 8);
    int best = -1;
    uint32_t best_area = 0;
    for (int i = 0; i < count && 6 + (i + 1) * 16 <= (int)len; i++) {
        const uint8_t *e = data + 6 + i * 16;
        uint32_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        uint32_t size = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t off = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        if (off + size <= len && size > 8 && memcmp(data + off, "\x89PNG", 4) == 0 && w * h > best_area) {
            best = i;
            best_area = w * h;
        }
    }
    if (best < 0) {
        return false;   /* BMP-only icon */
    }
    const uint8_t *e = data + 6 + best * 16;
    uint32_t size = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
    uint32_t off = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
    return decode_png(data + off, size, thumb);
}

static bool jpeg_is_baseline(const uint8_t *d, size_t len)
{
    /* walk the markers until a start-of-frame: SOF0/SOF1 are baseline, SOF2 is progressive */
    size_t i = 2;
    while (i + 4 <= len && d[i] == 0xFF) {
        uint8_t m = d[i + 1];
        if (m == 0xC0 || m == 0xC1) {
            return true;
        }
        if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return false;
        }
        i += 2 + ((d[i + 2] << 8) | d[i + 3]);
    }
    return false;
}

static bool decode_jpeg(const uint8_t *data, size_t len, uint8_t *thumb)
{
    if (!jpeg_is_baseline(data, len)) {
        ESP_LOGI(TAG, "progressive/unsupported JPEG");
        return false;
    }
    if (!s_jpeg) {
        jpeg_decode_engine_cfg_t ecfg = { .timeout_ms = 500 };
        if (jpeg_new_decoder_engine(&ecfg, &s_jpeg) != ESP_OK) {
            return false;
        }
    }
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, len, &info) != ESP_OK || info.width < MIN_SOURCE || info.height < MIN_SOURCE ||
            info.width > 2048 || info.height > 2048) {
        return false;
    }
    /* the engine writes whole MCUs: pad to 16 */
    uint32_t pw = (info.width + 15) & ~15u, ph = (info.height + 15) & ~15u;
    size_t in_cap = 0, out_cap = 0;
    jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint8_t *in = jpeg_alloc_decoder_mem(len, &in_cfg, &in_cap);
    uint8_t *out = jpeg_alloc_decoder_mem(pw * ph * 3, &out_cfg, &out_cap);
    bool ok = false;
    if (in && out) {
        memcpy(in, data, len);
        jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        if (jpeg_decoder_process(s_jpeg, &cfg, in, len, out, out_cap, &out_size) == ESP_OK) {
            /* rows are pw pixels wide; crop to the real picture */
            src_img_t cropped = { out, (int)info.width, (int)info.height, 3, (int)info.width * 3 };
            if (pw != info.width) {
                for (uint32_t y = 1; y < info.height; y++) {
                    memmove(out + y * info.width * 3, out + y * pw * 3, info.width * 3);
                }
            }
            make_thumb(&cropped, thumb);
            ok = true;
        }
    }
    free(in);
    free(out);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Download                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t len;
    bool too_big;
} dl_t;

static esp_err_t dl_event(esp_http_client_event_t *e)
{
    dl_t *d = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED || e->event_id == HTTP_EVENT_REDIRECT) {
        d->len = 0;             /* only keep the body of the final response */
        d->too_big = false;
    } else if (e->event_id == HTTP_EVENT_ON_DATA && !d->too_big) {
        if (d->len + e->data_len > MAX_DOWNLOAD) {
            d->too_big = true;
        } else {
            memcpy(d->buf + d->len, e->data, e->data_len);
            d->len += e->data_len;
        }
    }
    return ESP_OK;
}

static uint8_t *download(const char *url, size_t *len)
{
    dl_t d = { .buf = heap_caps_malloc(MAX_DOWNLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) };
    if (!d.buf) {
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-P4-Internet-Radio/1.0 (logo fetch; https://github.com/giltal/ESP32-P4-Internet-Radio)",
        .event_handler = dl_event,
        .user_data = &d,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = c ? esp_http_client_perform(c) : ESP_FAIL;
    int status = c ? esp_http_client_get_status_code(c) : 0;
    if (c) {
        esp_http_client_cleanup(c);
    }
    if (err != ESP_OK || status != 200 || d.too_big || d.len < 16) {
        ESP_LOGW(TAG, "%s -> %s, HTTP %d, %u bytes%s", url, esp_err_to_name(err), status, (unsigned)d.len,
                 d.too_big ? " (too big)" : "");
        free(d.buf);
        return NULL;
    }
    *len = d.len;
    return d.buf;
}

/* ------------------------------------------------------------------ */
/* Cache + worker                                                      */
/* ------------------------------------------------------------------ */

static uint32_t url_hash(const char *s)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

static bool load_cached(const char *name, uint8_t *thumb)
{
    size_t len = 0;
    uint8_t *f = sdcard_load(name, &len);
    const thumb_hdr_t *h = (const thumb_hdr_t *)f;
    bool ok = f && len == sizeof(*h) + THUMB_BYTES && memcmp(h->magic, "LG1", 4) == 0 &&
              h->w == LOGO_SIZE && h->h == LOGO_SIZE;
    if (ok) {
        memcpy(thumb, f + sizeof(*h), THUMB_BYTES);
    }
    free(f);
    return ok;
}

static bool fetch_logo(const char *url, uint8_t *thumb)
{
    char name[48], none[48];
    uint32_t h = url_hash(url);
    snprintf(name, sizeof(name), "logos/%08lx.bin", (unsigned long)h);
    snprintf(none, sizeof(none), "logos/%08lx.no", (unsigned long)h);

    if (load_cached(name, thumb)) {
        return true;
    }
    size_t nlen;
    void *marker = sdcard_load(none, &nlen);
    if (marker) {
        free(marker);
        return false;           /* known to have no usable logo */
    }

    size_t len = 0;
    uint8_t *data = download(url, &len);
    if (!data) {
        return false;           /* network trouble: try again next time */
    }
    bool ok;
    if (len >= 8 && memcmp(data, "\x89PNG", 4) == 0) {
        ok = decode_png(data, len, thumb);
    } else if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        ok = decode_jpeg(data, len, thumb);
    } else if (len >= 6 && memcmp(data, "\0\0\1\0", 4) == 0) {
        ok = decode_ico(data, len, thumb);
    } else {
        ESP_LOGI(TAG, "unsupported logo format: %s", url);
        ok = false;
    }
    free(data);

    if (ok) {
        uint8_t *file = heap_caps_malloc(sizeof(thumb_hdr_t) + THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (file) {
            thumb_hdr_t hdr = { .magic = "LG1", .w = LOGO_SIZE, .h = LOGO_SIZE };
            memcpy(file, &hdr, sizeof(hdr));
            memcpy(file + sizeof(hdr), thumb, THUMB_BYTES);
            sdcard_save(name, file, sizeof(hdr) + THUMB_BYTES);
            free(file);
        }
        ESP_LOGI(TAG, "logo cached: %s", url);
    } else {
        sdcard_save(none, "x", 1);   /* don't download it again */
    }
    return ok;
}

static void logo_task(void *arg)
{
    static char url[URL_LEN];
    uint8_t *work = heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        xQueueReceive(s_q, url, portMAX_DELAY);
        bool ok = url[0] && work && fetch_logo(url, work);
        if (uxQueueMessagesWaiting(s_q)) {
            continue;           /* the user already moved on to another station */
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_have = ok;
        strlcpy(s_have_url, url, sizeof(s_have_url));
        if (ok) {
            memcpy(s_thumb, work, THUMB_BYTES);
        }
        xSemaphoreGive(s_lock);
        s_rev++;
    }
}

void logo_init(void)
{
    s_q = xQueueCreate(1, URL_LEN);
    s_lock = xSemaphoreCreateMutex();
    s_thumb = heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* internal-RAM stack for the TLS handshake */
    xTaskCreatePinnedToCore(logo_task, "logo", 10240, NULL, 2, NULL, 0);
}

void logo_request(const char *url)
{
    char buf[URL_LEN];
    strlcpy(buf, url ? url : "", sizeof(buf));
    xQueueOverwrite(s_q, buf);
}

uint32_t logo_revision(void)
{
    return s_rev;
}

bool logo_copy(const char *url, uint8_t *dst)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_have && url && strcmp(url, s_have_url) == 0;
    if (ok) {
        memcpy(dst, s_thumb, THUMB_BYTES);
    }
    xSemaphoreGive(s_lock);
    return ok;
}
