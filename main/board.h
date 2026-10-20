/*
 * Board support for the Guition JC1060P470C (ESP32-P4 + ESP32-C6, 7" 1024x600).
 *
 * Pin map (from the Guition schematic / community bring-up):
 *   LCD     : MIPI-DSI 2 lanes, JD9165, reset GPIO5 (V1 panel) or GPIO0 (V2 panel)
 *   Backlight: GPIO23 (PWM)
 *   I2C     : SDA GPIO7, SCL GPIO8 (GT911 touch + ES8311 codec)
 *   I2S     : MCLK 13, BCLK 12, LRCK 10, DOUT 9, DIN 48
 *   Speaker : NS4150 amplifier enable on GPIO11
 *   Wi-Fi   : ESP32-C6 over SDIO (CLK 18, CMD 19, D0-D3 14-17, C6 reset 54) via esp_hosted
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_H_RES        1024
#define BOARD_LCD_V_RES        600

#define BOARD_I2C_SDA          7
#define BOARD_I2C_SCL          8

#define BOARD_I2S_MCLK         13
#define BOARD_I2S_BCLK         12
#define BOARD_I2S_LRCK         10
#define BOARD_I2S_DOUT         9
#define BOARD_I2S_DIN          48

/** Bring up I2C, display, touch and LVGL. Returns the LVGL display. */
esp_err_t board_display_init(lv_display_t **out_disp);

/** Backlight brightness 0..100 %. */
void board_backlight_set(int percent);

/** Bring up I2S + ES8311 codec + speaker amplifier. */
esp_err_t board_audio_init(void);

/**
 * (Re)open the audio output for a given PCM format. Output is always
 * 16-bit stereo on the wire; call again when the sample rate changes.
 */
esp_err_t board_audio_open(uint32_t sample_rate);
void board_audio_close(void);

/** Blocking write of interleaved 16-bit stereo PCM. */
esp_err_t board_audio_write(const int16_t *pcm, size_t bytes);

/** Output volume 0..100. */
void board_audio_set_volume(int volume);
void board_audio_mute(bool mute);

#ifdef __cplusplus
}
#endif
