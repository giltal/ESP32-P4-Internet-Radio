idf_component_register(
    SRCS
        "main.c"
        "board.c"
        "byte_ring.c"
        "player.c"
        "station_db.c"
        "wifi_mgr.c"
        "ui.c"
        "fonts/font_ui_20.c"
        "fonts/font_ui_28.c"
        "fonts/font_ui_40.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_http_client esp-tls esp_netif esp_wifi esp_event esp_timer
        esp_lcd esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_gpio
        json mbedtls
)

# The generated font files include "lvgl.h" directly when this is defined
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
