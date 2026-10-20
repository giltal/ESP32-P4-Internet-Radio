#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the radio UI. Call with the LVGL port lock held. */
void ui_init(lv_display_t *disp);

/** Apply brightness / volume / last station saved on the SD card. Call with the LVGL lock held. */
void ui_load_settings(void);

/** Resume the station that was playing before power-off (call once online). */
void ui_resume_last_station(void);

#ifdef __cplusplus
}
#endif
