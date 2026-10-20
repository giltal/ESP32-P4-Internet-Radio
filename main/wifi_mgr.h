/*
 * Wi-Fi station via the on-board ESP32-C6 (esp_hosted + esp_wifi_remote).
 * Credentials live in NVS; menuconfig values are only the first-boot default.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_IDLE = 0,       /* no credentials */
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_ap_info_t;

/**
 * Store credentials in NVS before wifi_mgr_start() (e.g. from the SD card).
 * NVS is only written when they differ from what is already saved.
 */
esp_err_t wifi_mgr_preset(const char *ssid, const char *password);

esp_err_t wifi_mgr_start(void);

wifi_state_t wifi_mgr_state(void);
bool wifi_mgr_wait_connected(uint32_t timeout_ms);

/** SSID being used (connected or trying). */
void wifi_mgr_ssid(char *buf, size_t len);
int wifi_mgr_rssi(void);

/** Save credentials and reconnect. */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/** Blocking scan; returns number of entries written (strongest first). */
size_t wifi_mgr_scan(wifi_ap_info_t *out, size_t max);

#ifdef __cplusplus
}
#endif
