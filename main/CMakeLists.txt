idf_component_register(
    SRCS
        "main.c"
        "board.c"
        "byte_ring.c"
        "clock.c"
        "logo.c"
        "nowplaying.c"
        "weather.c"
        "player.c"
        "sdcard.c"
        "station_db.c"
        "wifi_mgr.c"
        "ui.c"
        "ui_common.c"
        "fonts/font_ui_20.c"
        "fonts/font_ui_28.c"
        "fonts/font_ui_40.c"
        "fonts/font_heb_22.c"
        "fonts/font_heb_32.c"
        "fonts/font_heb_48.c"
        "fonts/font_clock_120.c"
        "fonts/font_wx_24.c"
        "fonts/font_wx_32.c"
        "fonts/font_wx_64.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_http_client esp-tls esp_netif esp_wifi esp_event esp_timer
        esp_lcd esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_gpio
        json mbedtls fatfs sdmmc esp_driver_sdmmc esp_driver_jpeg
)

# The generated font files include "lvgl.h" directly when this is defined
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
