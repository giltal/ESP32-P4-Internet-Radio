#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board.h"
#include "clock.h"
#include "player.h"
#include "sdcard.h"
#include "station_db.h"
#include "ui_common.h"
#include "wifi_mgr.h"

static const char *TAG = "ui";

#define YEAR_MIN 2024

static const char *const s_weekdays[] = { "ראשון", "שני", "שלישי", "רביעי", "חמישי", "שישי", "שבת" };
static const char *const s_months[] = {
    "ינואר", "פברואר", "מרץ", "אפריל", "מאי", "יוני",
    "יולי", "אוגוסט", "ספטמבר", "אוקטובר", "נובמבר", "דצמבר",
};

/* ---- widgets ---- */
static lv_obj_t *s_clock;
static lv_obj_t *s_weekday_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_tabview;
static lv_obj_t *s_list[2];          /* [STATION_LIST_ALL], [STATION_LIST_FAVORITES] */
static lv_obj_t *s_empty_lbl[2];
static lv_obj_t *s_db_status;
static lv_obj_t *s_refresh_btn;

static lv_obj_t *s_avatar;
static lv_obj_t *s_avatar_lbl;
static lv_obj_t *s_station_lbl;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_song_lbl;
static lv_obj_t *s_artist_lbl;
static lv_obj_t *s_format_lbl;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_buffer_bar;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_fav_btn;
static lv_obj_t *s_vol_slider;
static lv_obj_t *s_vol_lbl;

static lv_obj_t *s_settings;
static lv_obj_t *s_ssid_dd;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_scan_btn_lbl;
static lv_obj_t *s_wifi_status_lbl;
static lv_obj_t *s_bl_slider;
static lv_obj_t *s_dd_day, *s_dd_mon, *s_dd_year, *s_dd_hour, *s_dd_min;
static lv_obj_t *s_sw_ntp;

/* ---- state ---- */
static station_t s_current;
static bool s_have_current;
static station_list_t s_current_list = STATION_LIST_ALL;
static int s_current_index = -1;
static uint32_t s_list_rev_seen[2] = { UINT32_MAX, UINT32_MAX };
static uint32_t s_status_rev_seen = UINT32_MAX;
static uint32_t s_player_rev_seen = UINT32_MAX;
static int s_last_minute = -1;
static int s_last_day = -1;

static wifi_ap_info_t s_scan_res[24];
static volatile size_t s_scan_count;
static volatile bool s_scan_running;
static volatile bool s_scan_done;

/* ------------------------------------------------------------------ */

static lv_obj_t *make_card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_icon_button(lv_obj_t *parent, const char *sym, int size, const lv_font_t *font)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COL_CARD2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, font, COL_TEXT, sym);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_text_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, &g_font22, fg, text);
    lv_obj_center(l);
    return b;
}

/* First UTF-8 character of a name, upper-cased if ASCII */
static void initial_of(const char *name, char *out)
{
    size_t n = 1;
    unsigned char c = (unsigned char)name[0];
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    memcpy(out, name, n);
    out[n] = 0;
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z') {
        out[0] -= 32;
    }
}

/* Stable pleasant color per station */
static lv_color_t color_for(const char *name)
{
    static const uint32_t palette[] = {
        0xE5484D, 0xF5A524, 0x30A46C, 0x0090FF, 0x8E4EC6, 0xD6409F, 0x12A594, 0x3E63DD,
    };
    uint32_t h = 5381;
    for (const char *p = name; *p; p++) {
        h = h * 33 + (unsigned char)*p;
    }
    return lv_color_hex(palette[h % (sizeof(palette) / sizeof(palette[0]))]);
}

/* ------------------------------------------------------------------ */
/* Station lists                                                       */
/* ------------------------------------------------------------------ */

static void update_now_playing_header(void);

static void start_station(station_list_t list, int index)
{
    station_t st;
    if (!station_db_get(list, index, &st)) {
        return;
    }
    s_current = st;
    s_have_current = true;
    s_current_list = list;
    s_current_index = index;
    player_play(&st);
    update_now_playing_header();
}

static void list_item_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    station_list_t list = (station_list_t)(intptr_t)lv_obj_get_user_data(lv_obj_get_parent(btn));
    int index = (int)(intptr_t)lv_obj_get_user_data(btn);
    start_station(list, index);
}

static void mark_current_in_list(station_list_t which)
{
    lv_obj_t *list = s_list[which];
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(list, i);
        int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
        station_t st;
        bool cur = s_have_current && station_db_get(which, idx, &st) && strcmp(st.url, s_current.url) == 0;
        if (cur) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btn, LV_STATE_CHECKED);
        }
    }
}

static void rebuild_list(station_list_t which)
{
    lv_obj_t *list = s_list[which];
    lv_obj_clean(list);
    size_t n = station_db_count(which);
    for (size_t i = 0; i < n; i++) {
        station_t st;
        if (!station_db_get(which, i, &st)) {
            break;
        }
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_bg_color(btn, COL_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2416), LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_RIGHT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 4, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_ver(btn, 8, 0);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(btn, 0, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *name = ui_label(btn, &g_font22, COL_TEXT, st.name);
        lv_obj_set_size(name, lv_pct(100), 31);   /* fixed height: DOTS would otherwise wrap */
        lv_obj_set_style_base_dir(name, LV_BASE_DIR_AUTO, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

        char sub[48];
        if (st.bitrate) {
            snprintf(sub, sizeof(sub), "%s  ·  %u kbps", st.codec, st.bitrate);
        } else {
            snprintf(sub, sizeof(sub), "%s", st.codec);
        }
        lv_obj_t *meta = ui_label(btn, &g_font22, COL_DIM, sub);
        lv_obj_set_width(meta, lv_pct(100));
        lv_obj_set_style_base_dir(meta, LV_BASE_DIR_LTR, 0);
        lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (n == 0) {
        lv_obj_remove_flag(s_empty_lbl[which], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_lbl[which], LV_OBJ_FLAG_HIDDEN);
    }
    mark_current_in_list(which);
}

static void refresh_btn_cb(lv_event_t *e)
{
    if (wifi_mgr_state() != WIFI_STATE_CONNECTED) {
        lv_label_set_text(s_db_status, "יש להתחבר לרשת WiFi קודם");
        return;
    }
    station_db_refresh_async();
}

static lv_obj_t *build_list_tab(lv_obj_t *tab, station_list_t which, const char *empty_text)
{
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *list = lv_obj_create(tab);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_user_data(list, (void *)(intptr_t)which);
    s_list[which] = list;

    s_empty_lbl[which] = ui_label(tab, &g_font22, COL_DIM, empty_text);
    lv_obj_set_width(s_empty_lbl[which], lv_pct(100));
    lv_label_set_long_mode(s_empty_lbl[which], LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(s_empty_lbl[which], LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_empty_lbl[which], LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_align(s_empty_lbl[which], LV_TEXT_ALIGN_CENTER, 0);
    return list;
}

/* ------------------------------------------------------------------ */
/* Now playing                                                         */
/* ------------------------------------------------------------------ */

static void update_fav_button(void)
{
    bool fav = s_have_current && station_db_is_favorite(&s_current);
    lv_obj_t *icon = lv_obj_get_child(s_fav_btn, 0);
    lv_obj_set_style_text_color(icon, fav ? COL_ACCENT : COL_DIM, 0);
}

static void update_now_playing_header(void)
{
    if (!s_have_current) {
        lv_label_set_text(s_station_lbl, "בחרו תחנה");
        lv_label_set_text(s_avatar_lbl, LV_SYMBOL_AUDIO);
        lv_obj_set_style_bg_color(s_avatar, COL_CARD2, 0);
        update_fav_button();
        return;
    }
    char ini[5];
    initial_of(s_current.name, ini);
    lv_label_set_text(s_avatar_lbl, ini);
    lv_obj_set_style_bg_color(s_avatar, color_for(s_current.name), 0);
    lv_label_set_text(s_station_lbl, s_current.name);
    update_fav_button();
    mark_current_in_list(STATION_LIST_ALL);
    mark_current_in_list(STATION_LIST_FAVORITES);
}

static void play_btn_cb(lv_event_t *e)
{
    player_status_t st;
    player_get_status(&st);
    if (st.state == PLAYER_STOPPED || st.state == PLAYER_ERROR) {
        if (s_have_current) {
            player_play(&s_current);
        } else if (station_db_count(STATION_LIST_FAVORITES)) {
            start_station(STATION_LIST_FAVORITES, 0);
        } else if (station_db_count(STATION_LIST_ALL)) {
            start_station(STATION_LIST_ALL, 0);
        }
    } else {
        player_stop();
    }
}

static void step_station(int dir)
{
    station_list_t list = s_current_list;
    size_t n = station_db_count(list);
    if (n == 0) {
        list = STATION_LIST_ALL;
        n = station_db_count(list);
        if (n == 0) {
            return;
        }
    }
    int idx = s_current_index < 0 ? 0 : (int)((s_current_index + dir + (int)n) % (int)n);
    start_station(list, idx);
    lv_obj_t *btn = lv_obj_get_child(s_list[list], idx);
    if (btn) {
        lv_obj_scroll_to_view(btn, LV_ANIM_ON);
    }
}

static void prev_btn_cb(lv_event_t *e)
{
    step_station(-1);
}

static void next_btn_cb(lv_event_t *e)
{
    step_station(1);
}

static void fav_btn_cb(lv_event_t *e)
{
    if (!s_have_current) {
        return;
    }
    station_db_set_favorite(&s_current, !station_db_is_favorite(&s_current));
    update_fav_button();
}

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_vol_slider);
    player_set_volume(v);
    lv_label_set_text_fmt(s_vol_lbl, "%d", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        player_save_volume();
    }
}

static const char *state_text(const player_status_t *st, lv_color_t *color)
{
    *color = COL_DIM;
    switch (st->state) {
    case PLAYER_CONNECTING:
        *color = COL_ACCENT;
        return st->error[0] ? st->error : "מתחבר…";
    case PLAYER_BUFFERING:
        *color = COL_ACCENT;
        return "טוען…";
    case PLAYER_PLAYING:
        *color = COL_OK;
        return "שידור חי";
    case PLAYER_ERROR:
        *color = COL_ERR;
        return st->error[0] ? st->error : "שגיאה";
    default:
        return "עצור";
    }
}

/*
 * ICY StreamTitle is usually "Artist - Song". Show the song big and the artist
 * under it; titles without a separator (shows, slogans) go in the song line.
 */
static void show_song(const char *title)
{
    static const char *const seps[] = { " - ", " – ", " — " };
    const char *sep = NULL;
    size_t sep_len = 0;
    for (size_t i = 0; i < sizeof(seps) / sizeof(seps[0]) && !sep; i++) {
        sep = strstr(title, seps[i]);
        sep_len = strlen(seps[i]);
    }
    if (sep && sep != title && sep[sep_len]) {
        char artist[160];
        size_t n = sep - title < (int)sizeof(artist) - 1 ? (size_t)(sep - title) : sizeof(artist) - 1;
        memcpy(artist, title, n);
        artist[n] = 0;
        lv_label_set_text(s_song_lbl, sep + sep_len);
        lv_label_set_text(s_artist_lbl, artist);
    } else {
        lv_label_set_text(s_song_lbl, title);
        lv_label_set_text(s_artist_lbl, "");
    }
}

static void update_player(void)
{
    player_status_t st;
    player_get_status(&st);

    lv_bar_set_value(s_buffer_bar, st.buffer_pct, LV_ANIM_OFF);
    lv_bar_set_value(s_level_bar, st.state == PLAYER_PLAYING ? st.level : 0, LV_ANIM_ON);

    if (st.revision == s_player_rev_seen) {
        return;
    }
    s_player_rev_seen = st.revision;

    lv_color_t c;
    lv_label_set_text(s_state_lbl, state_text(&st, &c));
    lv_obj_set_style_text_color(s_state_lbl, c, 0);

    show_song(st.state == PLAYER_STOPPED ? "" : st.title);

    char fmt[64] = "";
    if (st.state != PLAYER_STOPPED && st.codec[0]) {
        int o = snprintf(fmt, sizeof(fmt), "%s", st.codec);
        if (st.bitrate_kbps) {
            o += snprintf(fmt + o, sizeof(fmt) - o, "  ·  %u kbps", st.bitrate_kbps);
        }
        if (st.sample_rate) {
            o += snprintf(fmt + o, sizeof(fmt) - o, "  ·  %.1f kHz", st.sample_rate / 1000.0);
        }
    }
    lv_label_set_text(s_format_lbl, fmt);

    bool active = st.state == PLAYER_CONNECTING || st.state == PLAYER_BUFFERING || st.state == PLAYER_PLAYING;
    lv_label_set_text(s_play_icon, active ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
}

/* ------------------------------------------------------------------ */
/* Clock + date header                                                 */
/* ------------------------------------------------------------------ */

static void update_clock(bool force)
{
    if (!clock_is_valid()) {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_weekday_lbl, "");
        lv_label_set_text(s_date_lbl, "השעון לא מכוון");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    /* Re-render the 120 px digits only when the minute changes */
    if (force || tm.tm_min != s_last_minute) {
        s_last_minute = tm.tm_min;
        lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    if (force || tm.tm_mday != s_last_day) {
        s_last_day = tm.tm_mday;
        lv_label_set_text_fmt(s_weekday_lbl, "יום %s", s_weekdays[tm.tm_wday % 7]);
        lv_label_set_text_fmt(s_date_lbl, "%d ב%s %d", tm.tm_mday, s_months[tm.tm_mon % 12], tm.tm_year + 1900);
    }
}

static void update_wifi_icon(void)
{
    lv_color_t c;
    switch (wifi_mgr_state()) {
    case WIFI_STATE_CONNECTED:  c = COL_OK;     break;
    case WIFI_STATE_CONNECTING: c = COL_ACCENT; break;
    default:                    c = COL_ERR;    break;
    }
    lv_obj_set_style_text_color(s_wifi_icon, c, 0);
}

/* ------------------------------------------------------------------ */
/* Settings overlay                                                    */
/* ------------------------------------------------------------------ */

static void scan_task(void *arg)
{
    s_scan_count = wifi_mgr_scan(s_scan_res, sizeof(s_scan_res) / sizeof(s_scan_res[0]));
    s_scan_done = true;
    s_scan_running = false;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scan_running) {
        return;
    }
    s_scan_running = true;
    s_scan_done = false;
    lv_label_set_text(s_scan_btn_lbl, "סורק…");
    xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, NULL, 3, NULL, 0);
}

static void scan_btn_cb(lv_event_t *e)
{
    start_scan();
}

static void ta_focus_cb(lv_event_t *e)
{
    ui_keyboard_attach(s_kb, s_pass_ta, false);
}

static void kb_done_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_pass_ta, LV_STATE_FOCUSED);
}

static void connect_btn_cb(lv_event_t *e)
{
    char ssid[33];
    lv_dropdown_get_selected_str(s_ssid_dd, ssid, sizeof(ssid));
    if (!ssid[0] || s_scan_count == 0) {
        return;
    }
    wifi_mgr_connect(ssid, lv_textarea_get_text(s_pass_ta));
    if (sdcard_write_wifi(ssid, lv_textarea_get_text(s_pass_ta)) != ESP_OK) {   /* remembered in WiFi.txt */
        ui_toast("אין כרטיס SD - הרשת לא תישמר", COL_ERR);
    }
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void bl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bl_slider);
    board_backlight_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        sdcard_save_int("brightness.txt", v);
    }
}

static void load_clock_pickers(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (!clock_is_valid()) {
        memset(&tm, 0, sizeof(tm));
        tm.tm_mday = 1;
        tm.tm_year = YEAR_MIN - 1900 + 2;
    }
    lv_dropdown_set_selected(s_dd_day, tm.tm_mday - 1);
    lv_dropdown_set_selected(s_dd_mon, tm.tm_mon);
    int y = tm.tm_year + 1900 - YEAR_MIN;
    lv_dropdown_set_selected(s_dd_year, y < 0 ? 0 : y);
    lv_dropdown_set_selected(s_dd_hour, tm.tm_hour);
    lv_dropdown_set_selected(s_dd_min, tm.tm_min);
    if (clock_auto_ntp()) {
        lv_obj_add_state(s_sw_ntp, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_sw_ntp, LV_STATE_CHECKED);
    }
}

static void clock_set_cb(lv_event_t *e)
{
    struct tm t = { 0 };
    t.tm_mday = lv_dropdown_get_selected(s_dd_day) + 1;
    t.tm_mon = lv_dropdown_get_selected(s_dd_mon);
    t.tm_year = lv_dropdown_get_selected(s_dd_year) + YEAR_MIN - 1900;
    t.tm_hour = lv_dropdown_get_selected(s_dd_hour);
    t.tm_min = lv_dropdown_get_selected(s_dd_min);
    esp_err_t err = clock_set_local(&t);
    update_clock(true);
    if (err == ESP_OK) {
        ui_toast("השעון עודכן", COL_OK);
    } else if (clock_is_valid()) {
        ui_toast("השעון עודכן (ללא שעון גיבוי)", COL_ACCENT);
    } else {
        ui_toast("שגיאה בעדכון השעון", COL_ERR);
    }
}

static void clock_ntp_cb(lv_event_t *e)
{
    if (wifi_mgr_state() != WIFI_STATE_CONNECTED) {
        ui_toast("אין חיבור לרשת WiFi", COL_ERR);
        return;
    }
    clock_sync_now();
    ui_toast("מסנכרן שעון מהרשת…", COL_ACCENT);
}

static void ntp_sw_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_sw_ntp, LV_STATE_CHECKED);
    clock_set_auto_ntp(on);
    if (on && wifi_mgr_state() == WIFI_STATE_CONNECTED) {
        clock_sync_now();
    }
}

static void settings_open_cb(lv_event_t *e)
{
    load_clock_pickers();
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings);
    if (s_scan_count == 0) {
        start_scan();
    }
}

static void settings_close_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *style_dropdown(lv_obj_t *dd, int w)
{
    lv_obj_set_width(dd, w);
    lv_obj_set_style_text_font(dd, &g_font32, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &g_font32, 0);
    lv_obj_set_style_bg_color(dd, COL_CARD2, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    return dd;
}

static lv_obj_t *number_dropdown(lv_obj_t *parent, int from, int to, const char *fmt, int w)
{
    static char opts[60 * 6];
    size_t o = 0;
    for (int v = from; v <= to && o < sizeof(opts) - 8; v++) {
        if (v != from) {
            opts[o++] = '\n';
        }
        o += snprintf(opts + o, sizeof(opts) - o, fmt, v);
    }
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    return style_dropdown(dd, w);
}

/* A settings row: label on the (physical) right, controls to its left */
static lv_obj_t *setting_row(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 72);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui_label(row, &g_font32, COL_TEXT, title);
    ui_align(l, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_update_layout(l);
    int32_t ctl_x = lv_obj_get_width(l) + 32;
    if (ctl_x < 260) {
        ctl_x = 260;   /* keep short-label rows in one column */
    }
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 12, 0);
    lv_obj_set_style_base_dir(box, LV_BASE_DIR_LTR, 0);   /* dates/times read left to right */
    ui_align(box, LV_ALIGN_RIGHT_MID, -ctl_x, 0);
    return box;
}

static void build_wifi_tab(lv_obj_t *tab)
{
    lv_obj_t *r = setting_row(tab, "רשת");
    s_ssid_dd = lv_dropdown_create(r);
    lv_dropdown_set_options(s_ssid_dd, " ");
    style_dropdown(s_ssid_dd, 400);
    lv_obj_t *scan = make_text_button(r, LV_SYMBOL_REFRESH "  סריקה", COL_CARD2, COL_TEXT, 150, 56);
    s_scan_btn_lbl = lv_obj_get_child(scan, 0);
    lv_obj_add_event_cb(scan, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    r = setting_row(tab, "סיסמה");
    s_pass_ta = lv_textarea_create(r);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_obj_set_style_text_font(s_pass_ta, &g_font32, 0);
    lv_obj_set_width(s_pass_ta, 400);
    lv_obj_add_event_cb(s_pass_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_pass_ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *conn = make_text_button(r, LV_SYMBOL_OK "  התחברות", COL_ACCENT, lv_color_hex(0x1A1200), 150, 56);
    lv_obj_add_event_cb(conn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_status_lbl = ui_label(tab, &g_font22, COL_DIM, "");
    lv_obj_set_width(s_wifi_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_wifi_status_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_wifi_status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_clock_tab(lv_obj_t *tab)
{
    lv_obj_t *r = setting_row(tab, "תאריך");
    s_dd_day = number_dropdown(r, 1, 31, "%02d", 110);
    ui_label(r, &g_font32, COL_DIM, ".");
    s_dd_mon = number_dropdown(r, 1, 12, "%02d", 110);
    ui_label(r, &g_font32, COL_DIM, ".");
    s_dd_year = number_dropdown(r, YEAR_MIN, YEAR_MIN + 15, "%d", 140);

    r = setting_row(tab, "שעה");
    s_dd_hour = number_dropdown(r, 0, 23, "%02d", 110);
    ui_label(r, &g_font32, COL_DIM, ":");
    s_dd_min = number_dropdown(r, 0, 59, "%02d", 110);

    r = setting_row(tab, "");
    lv_obj_t *set = make_text_button(r, "קביעת שעון", COL_ACCENT, lv_color_hex(0x1A1200), 220, 60);
    lv_obj_add_event_cb(set, clock_set_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sync = make_text_button(r, "סנכרון מהרשת", COL_CARD2, COL_TEXT, 220, 60);
    lv_obj_add_event_cb(sync, clock_ntp_cb, LV_EVENT_CLICKED, NULL);

    r = setting_row(tab, "עדכון אוטומטי מהרשת");
    s_sw_ntp = lv_switch_create(r);
    lv_obj_set_size(s_sw_ntp, 90, 44);
    lv_obj_set_style_bg_color(s_sw_ntp, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_ntp, ntp_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *note = ui_label(tab, &g_font22, COL_DIM, "אזור זמן: ישראל (שעון קיץ מתעדכן אוטומטית)");
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_display_tab(lv_obj_t *tab)
{
    lv_obj_t *r = setting_row(tab, "בהירות מסך");
    s_bl_slider = lv_slider_create(r);
    lv_obj_set_width(s_bl_slider, 420);
    lv_slider_set_range(s_bl_slider, 5, 100);
    lv_obj_set_style_bg_color(s_bl_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bl_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bl_slider, bl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bl_slider, bl_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *about = ui_label(tab, &g_font22, COL_DIM,
                               "תחנות: radio-browser.info  ·  MP3 / AAC / HE-AAC  ·  Guition JC1060P470C");
    lv_obj_set_width(about, lv_pct(100));
    lv_obj_set_style_text_align(about, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_settings(void)
{
    s_settings = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_settings, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(s_settings, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_radius(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 16, 0);
    lv_obj_set_style_base_dir(s_settings, LV_BASE_DIR_RTL, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(s_settings, &g_font48, COL_TEXT, "הגדרות");
    ui_align(title, LV_ALIGN_TOP_RIGHT, -8, 0);

    lv_obj_t *close = make_icon_button(s_settings, LV_SYMBOL_CLOSE, 60, &g_font32);
    ui_align(close, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tv = lv_tabview_create(s_settings);
    lv_obj_set_size(tv, lv_pct(100), BOARD_LCD_V_RES - 32 - 76);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 76);
    lv_tabview_set_tab_bar_size(tv, 60);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_text_font(bar, &g_font32, 0);
    lv_obj_t *t_wifi = lv_tabview_add_tab(tv, LV_SYMBOL_WIFI "  רשת WiFi");
    lv_obj_t *t_clock = lv_tabview_add_tab(tv, "שעון ותאריך");
    lv_obj_t *t_disp = lv_tabview_add_tab(tv, "תצוגה");
    lv_obj_remove_flag(lv_tabview_get_content(tv), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tabs[] = { t_wifi, t_clock, t_disp };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_flex_flow(tabs[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(tabs[i], 8, 0);
        lv_obj_set_style_pad_all(tabs[i], 16, 0);
    }
    build_wifi_tab(t_wifi);
    build_clock_tab(t_clock);
    build_display_tab(t_disp);

    s_kb = ui_keyboard_create(s_settings);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_CANCEL, NULL);
}

static void update_settings(void)
{
    if (s_scan_done) {
        s_scan_done = false;
        lv_label_set_text(s_scan_btn_lbl, LV_SYMBOL_REFRESH "  סריקה");
        static char opts[24 * 34];
        size_t o = 0;
        char cur[33];
        wifi_mgr_ssid(cur, sizeof(cur));
        int sel = 0;
        for (size_t i = 0; i < s_scan_count; i++) {
            o += snprintf(opts + o, sizeof(opts) - o, "%s%s", i ? "\n" : "", s_scan_res[i].ssid);
            if (strcmp(s_scan_res[i].ssid, cur) == 0) {
                sel = i;
            }
        }
        lv_dropdown_set_options(s_ssid_dd, s_scan_count ? opts : "לא נמצאו רשתות");
        lv_dropdown_set_selected(s_ssid_dd, sel);
    }
    char ssid[33];
    wifi_mgr_ssid(ssid, sizeof(ssid));
    switch (wifi_mgr_state()) {
    case WIFI_STATE_CONNECTED:
        lv_label_set_text_fmt(s_wifi_status_lbl, "מחובר ל-%s  (%d dBm)", ssid, wifi_mgr_rssi());
        break;
    case WIFI_STATE_CONNECTING:
        lv_label_set_text_fmt(s_wifi_status_lbl, "מתחבר ל-%s…", ssid);
        break;
    case WIFI_STATE_FAILED:
        lv_label_set_text_fmt(s_wifi_status_lbl, "אין חיבור ל-%s. בדקו את הסיסמה.", ssid);
        break;
    default:
        lv_label_set_text(s_wifi_status_lbl, "בחרו רשת והקלידו סיסמה.");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Periodic refresh                                                    */
/* ------------------------------------------------------------------ */

static void tick_cb(lv_timer_t *t)
{
    static uint32_t n;
    n++;

    update_player();

    for (int l = 0; l < 2; l++) {
        uint32_t rev = station_db_revision((station_list_t)l);
        if (rev != s_list_rev_seen[l]) {
            s_list_rev_seen[l] = rev;
            rebuild_list((station_list_t)l);
            update_fav_button();
        }
    }
    uint32_t srev = station_db_status_revision();
    if (srev != s_status_rev_seen) {
        s_status_rev_seen = srev;
        char buf[96];
        station_db_status(buf, sizeof(buf));
        lv_label_set_text(s_db_status, buf);
        if (station_db_busy()) {
            lv_obj_add_state(s_refresh_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_refresh_btn, LV_STATE_DISABLED);
        }
    }

    if (n % 5 == 0) {   /* once a second */
        update_clock(false);
        update_wifi_icon();
    }
    if (!lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN) || s_scan_done) {
        update_settings();
    }
}

/* ------------------------------------------------------------------ */

static void build_header(lv_obj_t *scr)
{
    /* Big clock in the middle; weekday + full date on the left; settings + Wi-Fi on the right */
    s_clock = ui_label(scr, &font_clock_120, COL_TEXT, "--:--");
    lv_obj_set_style_base_dir(s_clock, LV_BASE_DIR_LTR, 0);
    ui_align(s_clock, LV_ALIGN_TOP_MID, 0, 6);   /* tall digits (6, 8) reach the top of the line box */

    s_weekday_lbl = ui_label(scr, &g_font48, COL_ACCENT, "");
    ui_align(s_weekday_lbl, LV_ALIGN_TOP_LEFT, 28, 14);

    s_date_lbl = ui_label(scr, &g_font32, COL_TEXT, "");
    ui_align(s_date_lbl, LV_ALIGN_TOP_LEFT, 28, 80);

    lv_obj_t *gear = make_icon_button(scr, LV_SYMBOL_SETTINGS, 60, &g_font32);
    ui_align(gear, LV_ALIGN_TOP_RIGHT, -24, 36);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_icon = ui_label(scr, &g_font32, COL_DIM, LV_SYMBOL_WIFI);
    ui_align(s_wifi_icon, LV_ALIGN_TOP_RIGHT, -108, 50);
}

static void build_lists(lv_obj_t *scr)
{
    lv_obj_t *left = make_card(scr, 420, 436);
    ui_align(left, LV_ALIGN_TOP_LEFT, 16, 148);
    lv_obj_set_style_pad_all(left, 8, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);

    s_tabview = lv_tabview_create(left);
    lv_tabview_set_tab_bar_size(s_tabview, 52);
    lv_obj_set_width(s_tabview, lv_pct(100));
    lv_obj_set_flex_grow(s_tabview, 1);
    lv_obj_set_style_bg_opa(s_tabview, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabview);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_text_font(bar, &g_font22, 0);
    lv_obj_t *tab_fav = lv_tabview_add_tab(s_tabview, SYM_STAR "  מועדפים");
    lv_obj_t *tab_all = lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST "  כל התחנות");
    lv_obj_remove_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);

    build_list_tab(tab_fav, STATION_LIST_FAVORITES, "אין מועדפים עדיין.\nהפעילו תחנה והקישו " SYM_STAR " כדי להוסיף.");
    build_list_tab(tab_all, STATION_LIST_ALL, "אין תחנות עדיין.\nהתחברו לרשת WiFi והקישו רענון.");

    lv_obj_t *foot = lv_obj_create(left);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, lv_pct(100), 52);
    lv_obj_remove_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    s_db_status = ui_label(foot, &g_font22, COL_DIM, "");
    lv_obj_set_width(s_db_status, 250);
    lv_label_set_long_mode(s_db_status, LV_LABEL_LONG_MODE_DOTS);
    ui_align(s_db_status, LV_ALIGN_RIGHT_MID, -4, 0);
    s_refresh_btn = make_text_button(foot, LV_SYMBOL_REFRESH "  רענון", COL_CARD2, COL_TEXT, 130, 44);
    ui_align(s_refresh_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(s_refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void build_now_playing(lv_obj_t *scr)
{
    lv_obj_t *np = make_card(scr, 556, 436);
    ui_align(np, LV_ALIGN_TOP_RIGHT, -16, 148);
    lv_obj_set_style_pad_all(np, 20, 0);

    /* station: avatar on the right, name + state to its left */
    s_avatar = lv_obj_create(np);
    lv_obj_set_size(s_avatar, 96, 96);
    lv_obj_set_style_radius(s_avatar, 24, 0);
    lv_obj_set_style_border_width(s_avatar, 0, 0);
    lv_obj_set_style_bg_color(s_avatar, COL_CARD2, 0);
    lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui_align(s_avatar, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_avatar_lbl = ui_label(s_avatar, &g_font48, lv_color_white(), LV_SYMBOL_AUDIO);
    lv_obj_center(s_avatar_lbl);

    s_station_lbl = ui_label(np, &g_font48, COL_TEXT, "בחרו תחנה");
    lv_obj_set_size(s_station_lbl, 400, 65);   /* one line, long names end with "…" */
    lv_obj_set_style_base_dir(s_station_lbl, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_style_text_align(s_station_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_station_lbl, LV_LABEL_LONG_MODE_DOTS);
    ui_align(s_station_lbl, LV_ALIGN_TOP_RIGHT, -116, -4);

    s_state_lbl = ui_label(np, &g_font22, COL_DIM, "עצור");
    ui_align(s_state_lbl, LV_ALIGN_TOP_RIGHT, -116, 64);

    /* song + artist from the stream's ICY metadata */
    s_song_lbl = ui_label(np, &g_font32, COL_TEXT, "");
    lv_obj_set_width(s_song_lbl, lv_pct(100));
    lv_obj_set_style_base_dir(s_song_lbl, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_style_text_align(s_song_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_song_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    ui_align(s_song_lbl, LV_ALIGN_TOP_RIGHT, 0, 102);

    s_artist_lbl = ui_label(np, &g_font32, COL_ACCENT, "");
    lv_obj_set_width(s_artist_lbl, lv_pct(100));
    lv_obj_set_style_base_dir(s_artist_lbl, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_style_text_align(s_artist_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    ui_align(s_artist_lbl, LV_ALIGN_TOP_RIGHT, 0, 148);

    s_format_lbl = ui_label(np, &g_font22, COL_DIM, "");
    lv_obj_set_style_base_dir(s_format_lbl, LV_BASE_DIR_LTR, 0);
    ui_align(s_format_lbl, LV_ALIGN_TOP_LEFT, 0, 64);   /* same row as the state, other side */

    /* level + buffer meters */
    s_level_bar = lv_bar_create(np);
    lv_obj_set_size(s_level_bar, lv_pct(100), 8);
    ui_align(s_level_bar, LV_ALIGN_TOP_RIGHT, 0, 204);
    lv_obj_set_style_bg_color(s_level_bar, COL_CARD2, 0);
    lv_obj_set_style_bg_color(s_level_bar, COL_OK, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_level_bar, 150, 0);

    s_buffer_bar = lv_bar_create(np);
    lv_obj_set_size(s_buffer_bar, lv_pct(100), 4);
    ui_align(s_buffer_bar, LV_ALIGN_TOP_RIGHT, 0, 218);
    lv_obj_set_style_bg_color(s_buffer_bar, COL_CARD2, 0);
    lv_obj_set_style_bg_color(s_buffer_bar, COL_DIM, LV_PART_INDICATOR);

    /* transport controls: media symbols keep their usual left-to-right order */
    lv_obj_t *ctl = lv_obj_create(np);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, lv_pct(100), 96);
    lv_obj_align(ctl, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_base_dir(ctl, LV_BASE_DIR_LTR, 0);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 28, 0);

    lv_obj_t *prev = make_icon_button(ctl, LV_SYMBOL_PREV, 72, &g_font32);
    lv_obj_add_event_cb(prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *play = make_icon_button(ctl, LV_SYMBOL_PLAY, 96, &g_font48);
    lv_obj_set_style_bg_color(play, COL_ACCENT, 0);
    s_play_icon = lv_obj_get_child(play, 0);
    lv_obj_set_style_text_color(s_play_icon, lv_color_hex(0x1A1200), 0);
    lv_obj_add_event_cb(play, play_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = make_icon_button(ctl, LV_SYMBOL_NEXT, 72, &g_font32);
    lv_obj_add_event_cb(next, next_btn_cb, LV_EVENT_CLICKED, NULL);

    s_fav_btn = make_icon_button(ctl, SYM_STAR, 72, &g_font32);
    lv_obj_add_event_cb(s_fav_btn, fav_btn_cb, LV_EVENT_CLICKED, NULL);

    /* volume */
    lv_obj_t *vol = lv_obj_create(np);
    lv_obj_remove_style_all(vol);
    lv_obj_set_size(vol, lv_pct(100), 40);
    lv_obj_align(vol, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(vol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_base_dir(vol, LV_BASE_DIR_LTR, 0);
    lv_obj_t *vic = ui_label(vol, &g_font32, COL_DIM, LV_SYMBOL_VOLUME_MID);
    lv_obj_align(vic, LV_ALIGN_LEFT_MID, 0, 0);
    s_vol_slider = lv_slider_create(vol);
    lv_obj_set_width(s_vol_slider, 400);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, player_get_volume(), LV_ANIM_OFF);
    lv_obj_align(s_vol_slider, LV_ALIGN_LEFT_MID, 52, 0);
    lv_obj_set_style_bg_color(s_vol_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, vol_cb, LV_EVENT_RELEASED, NULL);
    s_vol_lbl = ui_label(vol, &g_font22, COL_DIM, "");
    lv_label_set_text_fmt(s_vol_lbl, "%d", player_get_volume());
    lv_obj_align(s_vol_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
}

void ui_init(lv_display_t *disp)
{
    ui_fonts_init();
    lv_theme_t *th = lv_theme_default_init(disp, COL_ACCENT, lv_color_hex(0x0090FF), true, &g_font22);
    lv_display_set_theme(disp, th);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_font(scr, &g_font22, 0);
    lv_obj_set_style_base_dir(scr, LV_BASE_DIR_RTL, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_now_playing(scr);
    build_lists(scr);
    build_settings();

    lv_slider_set_value(s_bl_slider, 80, LV_ANIM_OFF);
    board_backlight_set(80);
    update_now_playing_header();
    update_clock(true);

    lv_timer_create(tick_cb, 200, NULL);
    ESP_LOGI(TAG, "UI ready");
}

/* Settings saved on the SD card: brightness, volume, last station. Call with the LVGL lock held. */
void ui_load_settings(void)
{
    int bl = sdcard_load_int("brightness.txt", 80);
    lv_slider_set_value(s_bl_slider, bl, LV_ANIM_OFF);
    board_backlight_set(bl);

    lv_slider_set_value(s_vol_slider, player_get_volume(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_lbl, "%d", player_get_volume());

    if (player_last_station(&s_current)) {
        s_have_current = true;
    }
    update_now_playing_header();

    /* Start on favorites if there are any */
    lv_tabview_set_active(s_tabview, station_db_count(STATION_LIST_FAVORITES) ? 0 : 1, LV_ANIM_OFF);
}

/* Called from main once Wi-Fi is up: resume the last station. */
void ui_resume_last_station(void)
{
    if (s_have_current) {
        player_play(&s_current);
    }
}
