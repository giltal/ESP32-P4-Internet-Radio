/*
 * Shared UI pieces: fonts, palette, RTL helpers, on-screen keyboard, toast.
 * Adapted from ESP32_P4_FingerprintLoginSystem. Call with the LVGL lock held.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

/*
 * Alef (Hebrew + Latin) with Noto Sans fallback, so Cyrillic / Arabic station
 * names and LVGL / FontAwesome symbols still render.
 */
extern lv_font_t g_font22;
extern lv_font_t g_font32;
extern lv_font_t g_font48;
LV_FONT_DECLARE(font_clock_120);   /* Alef Bold digits for the big clock */
LV_FONT_DECLARE(font_wx_24);       /* FontAwesome weather icons (tools/gen_fonts_wx.sh) */
LV_FONT_DECLARE(font_wx_32);
LV_FONT_DECLARE(font_wx_64);
#define SYM_DROPLET "\xEF\x81\x83"  /* F043 */
#define SYM_WIND    "\xEF\x9C\xAE"  /* F72E */

/* FontAwesome star: in the Noto fallback fonts, not in LVGL's symbol list */
#define SYM_STAR   "\xEF\x80\x85"   /* U+F005 */

#define COL_BG       lv_color_hex(0x0E1116)
#define COL_CARD     lv_color_hex(0x171B22)
#define COL_CARD2    lv_color_hex(0x1F242D)
#define COL_ACCENT   lv_color_hex(0xF5A524)
#define COL_TEXT     lv_color_hex(0xE8EAED)
#define COL_DIM      lv_color_hex(0x8A93A0)
#define COL_ERR      lv_color_hex(0xFF5C5C)
#define COL_OK       lv_color_hex(0x4CC38A)

void ui_fonts_init(void);

/* lv_obj_align() with PHYSICAL left/right, also inside RTL parents
 * (LVGL mirrors LEFT/RIGHT alignments when the parent's base_dir is RTL). */
void ui_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);

/* On-screen keyboard with Hebrew / English / digits layouts (starts hidden). */
lv_obj_t *ui_keyboard_create(lv_obj_t *parent);
void ui_keyboard_attach(lv_obj_t *kb, lv_obj_t *textarea, bool hebrew);

/* Short message on top of everything for a few seconds. */
void ui_toast(const char *text, lv_color_t color);
