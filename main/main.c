/*
 * ESP32-P4 Internet Radio for the Guition JC1060P470C (7" 1024x600 touch).
 *
 * Boot order: NVS -> display/UI -> audio -> Wi-Fi (ESP32-C6 over SDIO)
 *             -> station directory refresh -> resume last station.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "player.h"
#include "sdcard.h"
#include "station_db.h"
#include "wifi_mgr.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));

    if (board_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio codec init failed - continuing without sound");
    }

    ESP_ERROR_CHECK(station_db_init());
    ESP_ERROR_CHECK(player_init());

    lvgl_port_lock(0);
    ui_init(disp);
    lvgl_port_unlock();

    /* Wi-Fi credentials from /sdcard/WiFi.txt win over what NVS holds */
    char ssid[33], pass[65];
    if (sdcard_read_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        wifi_mgr_preset(ssid, pass);
    }
    memset(pass, 0, sizeof(pass));

    ESP_ERROR_CHECK(wifi_mgr_start());

    /* Wait for the network (the UI stays responsive meanwhile) */
    while (!wifi_mgr_wait_connected(60000)) {
        ESP_LOGW(TAG, "still waiting for Wi-Fi - set it up from the settings screen");
    }

    /* First boot, or the cached directory is getting old: fetch a fresh one */
    station_db_refresh_async();
    ui_resume_last_station();

    /* Refresh the directory once a day so moved stream URLs heal themselves */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(24 * 60 * 60 * 1000));
        station_db_refresh_async();
    }
}
