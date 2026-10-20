/*
 * microSD card (SDMMC slot 0, 4-bit: CLK 43, CMD 44, D0-D3 39-42, powered by on-chip LDO 4).
 * Used at boot only, to pick up Wi-Fi credentials from /sdcard/WiFi.txt.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mount the card, read WiFi.txt and unmount again (the SDMMC host is then
 * free for esp_hosted). Accepted formats:
 *   SSID=MyNetwork          or   MyNetwork
 *   PASSWORD=secret              secret
 * Keys are case-insensitive, ':' works instead of '='; PASS / PSK / KEY are
 * accepted for the password. Leave the password empty for an open network.
 *
 * Returns ESP_OK with a non-empty ssid, ESP_ERR_NOT_FOUND if there is no
 * card / file / SSID.
 */
esp_err_t sdcard_read_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

#ifdef __cplusplus
}
#endif
