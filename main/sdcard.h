/*
 * microSD card (SDMMC slot 0, 4-bit: CLK 43, CMD 44, D0-D3 39-42, powered by on-chip LDO 4).
 *
 * The radio never writes the internal flash: flash erase/write stalls PSRAM, which the
 * display scans out of, and the panel blinks. Everything persistent lives on the card:
 *   /sdcard/WiFi.txt          Wi-Fi credentials (user-editable)
 *   /sdcard/radio/...         station cache, favorites, last station, volume, brightness
 * Without a card the radio still works, it just forgets on reboot.
 *
 * The SDMMC host is shared with the ESP32-C6 link (slot 1), so mount only after
 * wifi_mgr_start() and never unmount.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the card and keep it mounted. */
esp_err_t sdcard_mount(void);

bool sdcard_ready(void);

/**
 * Read Wi-Fi credentials from /sdcard/WiFi.txt. Accepted formats:
 *   SSID=MyNetwork          or   MyNetwork
 *   PASSWORD=secret              secret
 * Keys are case-insensitive, ':' works instead of '='; PASS / PSK / KEY are
 * accepted for the password. Leave the password empty for an open network.
 *
 * Returns ESP_OK with a non-empty ssid, ESP_ERR_NOT_FOUND if there is no
 * card / file / SSID.
 */
esp_err_t sdcard_read_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/** Write /sdcard/WiFi.txt (used when a network is picked on the screen). */
esp_err_t sdcard_write_wifi(const char *ssid, const char *pass);

/**
 * Read /sdcard/radio/<name> into a PSRAM buffer (NUL-terminated, caller frees).
 * Returns NULL if there is no card or file.
 */
void *sdcard_load(const char *name, size_t *len);

/** Write /sdcard/radio/<name> atomically (temp file + rename). */
esp_err_t sdcard_save(const char *name, const void *data, size_t len);

/** Small integer settings stored as text files under /sdcard/radio/. */
int sdcard_load_int(const char *name, int def);
void sdcard_save_int(const char *name, int value);

#ifdef __cplusplus
}
#endif
