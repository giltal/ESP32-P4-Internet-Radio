/*
 * ESP32-P4 Internet Radio for the Guition JC1060P470C (7" 1024x600 touch).
 *
 * Boot order: display/UI -> audio -> Wi-Fi (ESP32-C6 over SDIO) -> SD card
 *             -> saved settings + WiFi.txt -> station directory refresh -> resume last station.
 *
 * Nothing is ever written to the internal flash (it stalls PSRAM and the panel blinks):
 * state lives in RAM and, when a card is present, in files on the SD card.
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
#include "clock.h"
#include "logo.h"
#include "weather.h"
#include "nowplaying.h"
#include "sdcard.h"
#include "station_db.h"
#include "wifi_mgr.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    /* NVS is initialised for the Wi-Fi stack only; the app never writes it */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    clock_init(board_i2c_bus());   /* system time from the RTC before anything shows it */

    if (board_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio codec init failed - continuing without sound");
    }

    ESP_ERROR_CHECK(station_db_init());
    ESP_ERROR_CHECK(player_init());
    logo_init();
    nowplaying_init();

    lvgl_port_lock(0);
    ui_init(disp);
    lvgl_port_unlock();

    /* esp_hosted brings up the SDMMC host for the C6 (slot 1); the card on slot 0 joins it */
    ESP_ERROR_CHECK(wifi_mgr_start());
    sdcard_mount();

    station_db_load();
    player_load_settings();
    clock_load_settings();
    weather_init();
    lvgl_port_lock(0);
    ui_load_settings();
    lvgl_port_unlock();

    char ssid[33], pass[65];
    if (sdcard_read_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        wifi_mgr_connect(ssid, pass);
    }
    memset(pass, 0, sizeof(pass));

    /* Wait for the network (the UI stays responsive meanwhile) */
    while (!wifi_mgr_wait_connected(60000)) {
        ESP_LOGW(TAG, "still waiting for Wi-Fi - set it up from the settings screen");
    }

    /* First boot, or the cached directory is getting old: fetch a fresh one */
    station_db_refresh_async();
    weather_refresh();
    ui_resume_last_station();

    /* Refresh the directory once a day so moved stream URLs heal themselves */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(24 * 60 * 60 * 1000));
        station_db_refresh_async();
    }
}
