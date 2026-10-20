#include "ui_common.h"

#include <string.h>

LV_FONT_DECLARE(font_heb_22);
LV_FONT_DECLARE(font_heb_32);
LV_FONT_DECLARE(font_heb_48);
LV_FONT_DECLARE(font_ui_20);
LV_FONT_DECLARE(font_ui_28);
LV_FONT_DECLARE(font_ui_40);

lv_font_t g_font22;
lv_font_t g_font32;
lv_font_t g_font48;

void ui_fonts_init(void)
{
    /* The generated fonts are const; copy them so a fallback can be attached */
    g_font22 = font_heb_22;
    g_font32 = font_heb_32;
    g_font48 = font_heb_48;
    g_font22.fallback = &font_ui_20;
    g_font32.fallback = &font_ui_28;
    g_font48.fallback = &font_ui_40;
}

void ui_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
{
    /* LVGL 9.4 only mirrors LV_ALIGN_DEFAULT in RTL parents; explicit LEFT/RIGHT
     * alignments are already physical, so no remapping is needed here. */
    lv_obj_align(obj, align, x, y);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/* ---- keyboard ---- */

#define KEY_HEB "עב"
#define KEY_EN  "EN"
#define KEY_NUM "123"
#define F LV_KEYBOARD_CTRL_BUTTON_FLAGS

/* Israeli standard layout */
static const char *const kb_map_heb[] = {
    "ק", "ר", "א", "ט", "ו", "ן", "ם", "פ", LV_SYMBOL_BACKSPACE, "\n",
    "ש", "ד", "ג", "כ", "ע", "י", "ח", "ל", "ך", "ף", "\n",
    "ז", "ס", "ב", "ה", "נ", "מ", "צ", "ת", "ץ", "'", "\n",
    KEY_EN, KEY_NUM, " ", "-", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_heb[] = {
    4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 14, 4, F | 6,
};

static const char *const kb_map_en_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "'", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", "-", ".", "\n",
    KEY_HEB, KEY_NUM, " ", "@", LV_SYMBOL_OK, ""
};
static const char *const kb_map_en_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\"", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", "_", ",", "\n",
    KEY_HEB, KEY_NUM, " ", "#", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_en[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 6, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 14, 4, F | 6,
};

static const char *const kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "-", "/", ".", "(", ")", "'", "@", "_", "!", "?", "\n",
    "#", "$", "%", "&", "*", "+", "=", ":", ";", ",", "\n",
    KEY_HEB, KEY_EN, " ", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_num[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 18, F | 6,
};

static void kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    if (!txt) {
        return;
    }
    if (strcmp(txt, KEY_HEB) == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1); return; }
    if (strcmp(txt, KEY_EN) == 0 || strcmp(txt, "abc") == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_2); return; }
    if (strcmp(txt, "ABC") == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_4); return; }
    if (strcmp(txt, KEY_NUM) == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_3); return; }
    lv_keyboard_def_event_cb(e);
}

lv_obj_t *ui_keyboard_create(lv_obj_t *parent)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_map_heb, kb_ctrl_heb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, kb_map_en_lower, kb_ctrl_en);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_3, kb_map_num, kb_ctrl_num);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_4, kb_map_en_upper, kb_ctrl_en);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_2);

    /* Replace the default handler so the layout-switch keys aren't typed */
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_set_size(kb, LV_PCT(100), 250);
    lv_obj_set_style_base_dir(kb, LV_BASE_DIR_LTR, 0);   /* maps are in visual order */
    lv_obj_set_style_text_font(kb, &g_font32, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, COL_CARD, 0);
    lv_obj_set_style_bg_color(kb, COL_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A4250), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

void ui_keyboard_attach(lv_obj_t *kb, lv_obj_t *textarea, bool hebrew)
{
    lv_keyboard_set_textarea(kb, textarea);
    lv_keyboard_set_mode(kb, hebrew ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_USER_2);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

/* ---- toast ---- */

static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_delete(lv_timer_get_user_data(t));
}

void ui_toast(const char *text, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_hor(box, 28, 0);
    lv_obj_set_style_pad_ver(box, 14, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(box, &g_font32, lv_color_white(), text);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_RTL, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_timer_t *t = lv_timer_create(toast_del_cb, 2500, box);
    lv_timer_set_repeat_count(t, 1);
}
