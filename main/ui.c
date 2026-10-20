#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdcard.h"
#include "board.h"
#include "player.h"
#include "station_db.h"
#include "wifi_mgr.h"

LV_FONT_DECLARE(font_ui_20);
LV_FONT_DECLARE(font_ui_28);
LV_FONT_DECLARE(font_ui_40);

/* FontAwesome glyphs that are in our fonts but not in LVGL's symbol list */
#define SYM_STAR   "\xEF\x80\x85"   /* U+F005 */

#define COL_BG       lv_color_hex(0x0E1116)
#define COL_CARD     lv_color_hex(0x171B22)
#define COL_CARD2    lv_color_hex(0x1F242D)
#define COL_ACCENT   lv_color_hex(0xF5A524)
#define COL_TEXT     lv_color_hex(0xE8EAED)
#define COL_DIM      lv_color_hex(0x8A93A0)
#define COL_ERR      lv_color_hex(0xFF5C5C)
#define COL_OK       lv_color_hex(0x4CC38A)

static const char *TAG = "ui";

/* ---- widgets ---- */
static lv_obj_t *s_clock;
static lv_obj_t *s_wifi_lbl;
static lv_obj_t *s_tabview;
static lv_obj_t *s_list[2];          /* [STATION_LIST_ALL], [STATION_LIST_FAVORITES] */
static lv_obj_t *s_empty_lbl[2];
static lv_obj_t *s_db_status;
static lv_obj_t *s_refresh_btn;

static lv_obj_t *s_avatar;
static lv_obj_t *s_avatar_lbl;
static lv_obj_t *s_station_lbl;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_format_lbl;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_buffer_bar;
static lv_obj_t *s_play_btn;
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

/* ---- state ---- */
static station_t s_current;
static bool s_have_current;
static station_list_t s_current_list = STATION_LIST_ALL;
static int s_current_index = -1;
static uint32_t s_list_rev_seen[2] = { UINT32_MAX, UINT32_MAX };
static uint32_t s_status_rev_seen = UINT32_MAX;
static uint32_t s_player_rev_seen = UINT32_MAX;

static wifi_ap_info_t s_scan_res[24];
static volatile size_t s_scan_count;
static volatile bool s_scan_running;
static volatile bool s_scan_done;

/* ------------------------------------------------------------------ */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);   /* Hebrew / Arabic render right-to-left */
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    return c;
}

static lv_obj_t *make_icon_button(lv_obj_t *parent, const char *sym, int size, const lv_font_t *font)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COL_CARD2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = make_label(b, font, COL_TEXT, sym);
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
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 4, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_ver(btn, 10, 0);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(btn, 2, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *name = make_label(btn, &font_ui_20, COL_TEXT, st.name);
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

        char sub[48];
        if (st.bitrate) {
            snprintf(sub, sizeof(sub), "%s  ·  %u kbps", st.codec, st.bitrate);
        } else {
            snprintf(sub, sizeof(sub), "%s", st.codec);
        }
        lv_obj_t *meta = make_label(btn, &font_ui_20, COL_DIM, sub);
        lv_obj_set_style_text_font(meta, &font_ui_20, 0);
        lv_obj_set_style_base_dir(meta, LV_BASE_DIR_LTR, 0);
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
        lv_label_set_text(s_db_status, "Connect to Wi-Fi first (" LV_SYMBOL_SETTINGS ")");
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

    s_empty_lbl[which] = make_label(tab, &font_ui_20, COL_DIM, empty_text);
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
        lv_label_set_text(s_station_lbl, "Pick a station");
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
        return st->error[0] ? st->error : "Connecting…";
    case PLAYER_BUFFERING:
        *color = COL_ACCENT;
        return "Buffering…";
    case PLAYER_PLAYING:
        *color = COL_OK;
        return LV_SYMBOL_PLAY "  Live";
    case PLAYER_ERROR:
        *color = COL_ERR;
        return st->error[0] ? st->error : "Error";
    default:
        return "Stopped";
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
    const char *txt = state_text(&st, &c);
    lv_label_set_text(s_state_lbl, txt);
    lv_obj_set_style_text_color(s_state_lbl, c, 0);

    lv_label_set_text(s_title_lbl, st.title[0] ? st.title : "");

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
    lv_label_set_text(s_scan_btn_lbl, "Scanning…");
    xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, NULL, 3, NULL, 0);
}

static void scan_btn_cb(lv_event_t *e)
{
    start_scan();
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(s_pass_ta, LV_STATE_FOCUSED);
    }
}

static void connect_btn_cb(lv_event_t *e)
{
    char ssid[33];
    lv_dropdown_get_selected_str(s_ssid_dd, ssid, sizeof(ssid));
    if (!ssid[0] || strcmp(ssid, "(scan first)") == 0) {
        return;
    }
    wifi_mgr_connect(ssid, lv_textarea_get_text(s_pass_ta));
    sdcard_write_wifi(ssid, lv_textarea_get_text(s_pass_ta));   /* remembered in WiFi.txt */
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_wifi_status_lbl, "Connecting to %s…", ssid);
}

static void bl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bl_slider);
    board_backlight_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        sdcard_save_int("brightness.txt", v);
    }
}

static void settings_open_cb(lv_event_t *e)
{
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

static void build_settings(void)
{
    s_settings = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_settings, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(s_settings, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_radius(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 24, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(s_settings, &font_ui_28, COL_TEXT, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close = make_icon_button(s_settings, LV_SYMBOL_CLOSE, 56, &font_ui_28);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -8);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);

    /* Wi-Fi card */
    lv_obj_t *card = make_card(s_settings);
    lv_obj_set_size(card, 600, 300);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = make_label(card, &font_ui_20, COL_DIM, LV_SYMBOL_WIFI "  Wi-Fi network");
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ssid_dd = lv_dropdown_create(card);
    lv_obj_set_width(s_ssid_dd, 380);
    lv_obj_set_style_text_font(s_ssid_dd, &font_ui_20, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_ssid_dd), &font_ui_20, 0);
    lv_dropdown_set_options(s_ssid_dd, "(scan first)");
    lv_obj_align(s_ssid_dd, LV_ALIGN_TOP_LEFT, 0, 36);

    lv_obj_t *scan = lv_button_create(card);
    lv_obj_set_size(scan, 160, 44);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, 0, 36);
    s_scan_btn_lbl = make_label(scan, &font_ui_20, COL_TEXT, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(s_scan_btn_lbl);
    lv_obj_add_event_cb(scan, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    s_pass_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "Password");
    lv_obj_set_style_text_font(s_pass_ta, &font_ui_20, 0);
    lv_obj_set_width(s_pass_ta, 380);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_add_event_cb(s_pass_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_pass_ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *conn = lv_button_create(card);
    lv_obj_set_size(conn, 160, 44);
    lv_obj_align(conn, LV_ALIGN_TOP_RIGHT, 0, 100);
    lv_obj_set_style_bg_color(conn, COL_ACCENT, 0);
    lv_obj_t *cl = make_label(conn, &font_ui_20, lv_color_hex(0x1A1200), LV_SYMBOL_OK "  Connect");
    lv_obj_center(cl);
    lv_obj_add_event_cb(conn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_status_lbl = make_label(card, &font_ui_20, COL_DIM, "");
    lv_obj_set_width(s_wifi_status_lbl, 560);
    lv_label_set_long_mode(s_wifi_status_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_wifi_status_lbl, LV_ALIGN_TOP_LEFT, 0, 170);

    /* Display card */
    lv_obj_t *dcard = make_card(s_settings);
    lv_obj_set_size(dcard, 340, 160);
    lv_obj_align(dcard, LV_ALIGN_TOP_RIGHT, 0, 60);
    lv_obj_remove_flag(dcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dh = make_label(dcard, &font_ui_20, COL_DIM, LV_SYMBOL_EYE_OPEN "  Brightness");
    lv_obj_align(dh, LV_ALIGN_TOP_LEFT, 0, 0);
    s_bl_slider = lv_slider_create(dcard);
    lv_obj_set_width(s_bl_slider, 280);
    lv_slider_set_range(s_bl_slider, 5, 100);
    lv_obj_align(s_bl_slider, LV_ALIGN_TOP_LEFT, 10, 60);
    lv_obj_set_style_bg_color(s_bl_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bl_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bl_slider, bl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bl_slider, bl_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *about = make_label(s_settings, &font_ui_20, COL_DIM,
                                 "Stations: radio-browser.info  ·  MP3 / AAC / HE-AAC  ·  Guition JC1060P470C (ESP32-P4 + C6)");
    lv_obj_align(about, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_kb = lv_keyboard_create(s_settings);
    lv_obj_set_size(s_kb, BOARD_LCD_H_RES - 48, 230);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, &font_ui_20, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_ALL, NULL);
}

static void update_settings(void)
{
    if (s_scan_done) {
        s_scan_done = false;
        lv_label_set_text(s_scan_btn_lbl, LV_SYMBOL_REFRESH "  Scan");
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
        lv_dropdown_set_options(s_ssid_dd, s_scan_count ? opts : "(scan first)");
        lv_dropdown_set_selected(s_ssid_dd, sel);
    }
    char ssid[33];
    wifi_mgr_ssid(ssid, sizeof(ssid));
    switch (wifi_mgr_state()) {
    case WIFI_STATE_CONNECTED:
        lv_label_set_text_fmt(s_wifi_status_lbl, "Connected to %s (%d dBm)", ssid, wifi_mgr_rssi());
        break;
    case WIFI_STATE_CONNECTING:
        lv_label_set_text_fmt(s_wifi_status_lbl, "Connecting to %s…", ssid);
        break;
    case WIFI_STATE_FAILED:
        lv_label_set_text_fmt(s_wifi_status_lbl, "Can't connect to %s. Check the password.", ssid);
        break;
    default:
        lv_label_set_text(s_wifi_status_lbl, "Pick a network and enter its password.");
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
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year + 1900 >= 2024) {
            lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        }
        char ssid[33];
        wifi_mgr_ssid(ssid, sizeof(ssid));
        switch (wifi_mgr_state()) {
        case WIFI_STATE_CONNECTED:
            lv_label_set_text_fmt(s_wifi_lbl, LV_SYMBOL_WIFI "  %s", ssid);
            lv_obj_set_style_text_color(s_wifi_lbl, COL_TEXT, 0);
            break;
        case WIFI_STATE_CONNECTING:
            lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI "  connecting…");
            lv_obj_set_style_text_color(s_wifi_lbl, COL_ACCENT, 0);
            break;
        case WIFI_STATE_FAILED:
            lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WARNING "  Wi-Fi failed");
            lv_obj_set_style_text_color(s_wifi_lbl, COL_ERR, 0);
            break;
        default:
            lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WARNING "  No Wi-Fi set up");
            lv_obj_set_style_text_color(s_wifi_lbl, COL_ERR, 0);
            break;
        }
    }
    if (!lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN) || s_scan_done) {
        update_settings();
    }
}

/* ------------------------------------------------------------------ */

void ui_init(lv_display_t *disp)
{
    lv_theme_t *th = lv_theme_default_init(disp, COL_ACCENT, lv_color_hex(0x0090FF), true, &font_ui_20);
    lv_display_set_theme(disp, th);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_font(scr, &font_ui_20, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------- header ---------- */
    lv_obj_t *app = make_label(scr, &font_ui_28, COL_TEXT, LV_SYMBOL_AUDIO "  Radio");
    lv_obj_align(app, LV_ALIGN_TOP_LEFT, 24, 14);

    s_clock = make_label(scr, &font_ui_28, COL_TEXT, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *gear = make_icon_button(scr, LV_SYMBOL_SETTINGS, 48, &font_ui_20);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -16, 8);
    lv_obj_add_event_cb(gear, settings_open_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_lbl = make_label(scr, &font_ui_20, COL_DIM, LV_SYMBOL_WIFI);
    lv_obj_align_to(s_wifi_lbl, gear, LV_ALIGN_OUT_LEFT_MID, -16, 0);
    lv_obj_set_style_base_dir(s_wifi_lbl, LV_BASE_DIR_LTR, 0);
    lv_obj_set_width(s_wifi_lbl, 300);
    lv_obj_set_style_text_align(s_wifi_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align_to(s_wifi_lbl, gear, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    /* ---------- station lists (left) ---------- */
    lv_obj_t *left = make_card(scr);
    lv_obj_set_size(left, 440, 520);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 16, 68);
    lv_obj_set_style_pad_all(left, 8, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    s_tabview = lv_tabview_create(left);
    lv_tabview_set_tab_bar_size(s_tabview, 48);
    lv_obj_set_width(s_tabview, lv_pct(100));
    lv_obj_set_flex_grow(s_tabview, 1);
    lv_obj_set_style_bg_opa(s_tabview, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabview);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_text_font(bar, &font_ui_20, 0);
    lv_obj_t *tab_fav = lv_tabview_add_tab(s_tabview, SYM_STAR "  Favorites");
    lv_obj_t *tab_all = lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST "  " CONFIG_RADIO_COUNTRY_CODE " stations");
    lv_obj_remove_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);

    build_list_tab(tab_fav, STATION_LIST_FAVORITES,
                   "No favorites yet.\nPlay a station and tap " SYM_STAR " to add it here.");
    build_list_tab(tab_all, STATION_LIST_ALL,
                   "No stations yet.\nConnect to Wi-Fi, then tap Refresh.");

    lv_obj_t *foot = lv_obj_create(left);
    lv_obj_set_size(foot, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 4, 0);
    lv_obj_remove_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    s_db_status = make_label(foot, &font_ui_20, COL_DIM, "");
    lv_obj_set_width(s_db_status, 260);
    lv_label_set_long_mode(s_db_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_db_status, LV_ALIGN_LEFT_MID, 4, 0);
    s_refresh_btn = lv_button_create(foot);
    lv_obj_set_size(s_refresh_btn, 140, 42);
    lv_obj_align(s_refresh_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_refresh_btn, COL_CARD2, 0);
    lv_obj_t *rl = make_label(s_refresh_btn, &font_ui_20, COL_TEXT, LV_SYMBOL_REFRESH "  Refresh");
    lv_obj_center(rl);
    lv_obj_add_event_cb(s_refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ---------- now playing (right) ---------- */
    lv_obj_t *np = make_card(scr);
    lv_obj_set_size(np, 536, 520);
    lv_obj_align(np, LV_ALIGN_TOP_RIGHT, -16, 68);
    lv_obj_set_style_pad_all(np, 24, 0);
    lv_obj_remove_flag(np, LV_OBJ_FLAG_SCROLLABLE);

    s_avatar = lv_obj_create(np);
    lv_obj_set_size(s_avatar, 112, 112);
    lv_obj_set_style_radius(s_avatar, 28, 0);
    lv_obj_set_style_border_width(s_avatar, 0, 0);
    lv_obj_set_style_bg_color(s_avatar, COL_CARD2, 0);
    lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_avatar, LV_ALIGN_TOP_LEFT, 0, 0);
    s_avatar_lbl = make_label(s_avatar, &font_ui_40, lv_color_white(), LV_SYMBOL_AUDIO);
    lv_obj_center(s_avatar_lbl);

    s_station_lbl = make_label(np, &font_ui_40, COL_TEXT, "Pick a station");
    lv_obj_set_width(s_station_lbl, 360);
    lv_label_set_long_mode(s_station_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_station_lbl, LV_SIZE_CONTENT);
    lv_obj_align(s_station_lbl, LV_ALIGN_TOP_LEFT, 132, 4);

    s_state_lbl = make_label(np, &font_ui_20, COL_DIM, "Stopped");
    lv_obj_set_style_base_dir(s_state_lbl, LV_BASE_DIR_LTR, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_LEFT, 132, 72);

    s_title_lbl = make_label(np, &font_ui_28, COL_TEXT, "");
    lv_obj_set_width(s_title_lbl, lv_pct(100));
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_LEFT, 0, 150);

    s_format_lbl = make_label(np, &font_ui_20, COL_DIM, "");
    lv_obj_set_style_base_dir(s_format_lbl, LV_BASE_DIR_LTR, 0);
    lv_obj_align(s_format_lbl, LV_ALIGN_TOP_LEFT, 0, 196);

    /* level + buffer meters */
    s_level_bar = lv_bar_create(np);
    lv_obj_set_size(s_level_bar, lv_pct(100), 8);
    lv_obj_align(s_level_bar, LV_ALIGN_TOP_LEFT, 0, 238);
    lv_obj_set_style_bg_color(s_level_bar, COL_CARD2, 0);
    lv_obj_set_style_bg_color(s_level_bar, COL_OK, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_level_bar, 150, 0);

    s_buffer_bar = lv_bar_create(np);
    lv_obj_set_size(s_buffer_bar, lv_pct(100), 4);
    lv_obj_align(s_buffer_bar, LV_ALIGN_TOP_LEFT, 0, 254);
    lv_obj_set_style_bg_color(s_buffer_bar, COL_CARD2, 0);
    lv_obj_set_style_bg_color(s_buffer_bar, COL_DIM, LV_PART_INDICATOR);

    /* transport controls */
    lv_obj_t *ctl = lv_obj_create(np);
    lv_obj_set_size(ctl, lv_pct(100), 120);
    lv_obj_align(ctl, LV_ALIGN_TOP_MID, 0, 276);
    lv_obj_set_style_bg_opa(ctl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctl, 0, 0);
    lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 28, 0);

    lv_obj_t *prev = make_icon_button(ctl, LV_SYMBOL_PREV, 72, &font_ui_28);
    lv_obj_add_event_cb(prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);

    s_play_btn = make_icon_button(ctl, LV_SYMBOL_PLAY, 104, &font_ui_40);
    lv_obj_set_style_bg_color(s_play_btn, COL_ACCENT, 0);
    s_play_icon = lv_obj_get_child(s_play_btn, 0);
    lv_obj_set_style_text_color(s_play_icon, lv_color_hex(0x1A1200), 0);
    lv_obj_add_event_cb(s_play_btn, play_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = make_icon_button(ctl, LV_SYMBOL_NEXT, 72, &font_ui_28);
    lv_obj_add_event_cb(next, next_btn_cb, LV_EVENT_CLICKED, NULL);

    s_fav_btn = make_icon_button(ctl, SYM_STAR, 72, &font_ui_28);
    lv_obj_add_event_cb(s_fav_btn, fav_btn_cb, LV_EVENT_CLICKED, NULL);

    /* volume */
    lv_obj_t *vic = make_label(np, &font_ui_28, COL_DIM, LV_SYMBOL_VOLUME_MID);
    lv_obj_align(vic, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    s_vol_slider = lv_slider_create(np);
    lv_obj_set_width(s_vol_slider, 380);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, player_get_volume(), LV_ANIM_OFF);
    lv_obj_align(s_vol_slider, LV_ALIGN_BOTTOM_LEFT, 52, -14);
    lv_obj_set_style_bg_color(s_vol_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, vol_cb, LV_EVENT_RELEASED, NULL);
    s_vol_lbl = make_label(np, &font_ui_20, COL_DIM, "");
    lv_label_set_text_fmt(s_vol_lbl, "%d", player_get_volume());
    lv_obj_align(s_vol_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, -8);

    build_settings();

    lv_slider_set_value(s_bl_slider, 80, LV_ANIM_OFF);
    board_backlight_set(80);
    update_now_playing_header();

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
