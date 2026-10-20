#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "sdkconfig.h"

static const char *TAG = "board";

/* Kconfig bools are undefined when off; turn them into 0/1 for initializers */
#if CONFIG_RADIO_BL_ACTIVE_LOW
#define BL_ACTIVE_LOW 1
#else
#define BL_ACTIVE_LOW 0
#endif
#if CONFIG_RADIO_PANEL_V2
#define PANEL_IS_V2 1
#else
#define PANEL_IS_V2 0
#endif
#if CONFIG_RADIO_TOUCH_SWAP_XY
#define TP_SWAP_XY 1
#else
#define TP_SWAP_XY 0
#endif
#if CONFIG_RADIO_TOUCH_MIRROR_X
#define TP_MIRROR_X 1
#else
#define TP_MIRROR_X 0
#endif
#if CONFIG_RADIO_TOUCH_MIRROR_Y
#define TP_MIRROR_Y 1
#else
#define TP_MIRROR_Y 0
#endif

static i2c_master_bus_handle_t s_i2c_bus;

/* ------------------------------------------------------------------ */
/* JD9165 panel init tables (taken from Guition's reference drivers)   */
/* ------------------------------------------------------------------ */

#define CMD(c, ...) { (c), (uint8_t[]){ __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), 0 }
#define CMD_DELAY(c, ms) { (c), (uint8_t[]){ 0x00 }, 0, (ms) }

#if CONFIG_RADIO_PANEL_V2
static const jd9165_lcd_init_cmd_t s_panel_init[] = {
    CMD(0x30, 0x00), CMD(0xF7, 0x49, 0x61, 0x02, 0x00), CMD(0x30, 0x01), CMD(0x04, 0x0C), CMD(0x05, 0x08),
    CMD(0x0B, 0x11), CMD(0x20, 0x04), CMD(0x1F, 0x05), CMD(0x23, 0x38), CMD(0x28, 0x18), CMD(0x29, 0x29),
    CMD(0x2A, 0x01), CMD(0x2B, 0x29), CMD(0x2C, 0x01), CMD(0x30, 0x02), CMD(0x00, 0x05), CMD(0x01, 0x22),
    CMD(0x02, 0x08), CMD(0x03, 0x12), CMD(0x04, 0x16), CMD(0x05, 0x64), CMD(0x06, 0x00), CMD(0x07, 0x00),
    CMD(0x08, 0x78), CMD(0x09, 0x00), CMD(0x0A, 0x04),
    CMD(0x0B, 0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09),
    CMD(0x0C, 0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0D, 0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08),
    CMD(0x0E, 0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0F, 0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F),
    CMD(0x10, 0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x11, 0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E),
    CMD(0x12, 0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x13, 0x00, 0x00, 0x00, 0x00), CMD(0x14, 0x00, 0x00, 0x41, 0x41), CMD(0x15, 0x00, 0x00, 0x00, 0x00),
    CMD(0x17, 0x00), CMD(0x18, 0x85), CMD(0x19, 0x06, 0x09), CMD(0x1A, 0x05, 0x08), CMD(0x1B, 0x0A, 0x04),
    CMD(0x26, 0x00), CMD(0x27, 0x00), CMD(0x30, 0x06),
    CMD(0x12, 0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30),
    CMD(0x13, 0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30),
    CMD(0x30, 0x0A), CMD(0x02, 0x4F), CMD(0x0B, 0x40), CMD(0x30, 0x0D), CMD(0x0D, 0x04), CMD(0x10, 0x05),
    CMD(0x11, 0x0C), CMD(0x12, 0x05), CMD(0x13, 0x0C), CMD(0x30, 0x00),
    CMD_DELAY(0x11, 120),   /* SLPOUT */
    CMD_DELAY(0x29, 20),    /* DISPON */
};
#define PANEL_PCLK_MHZ  52
#define PANEL_HPW       24
#define PANEL_HBP       136
#define PANEL_HFP       160
#define PANEL_VPW       2
#define PANEL_VBP       21
#define PANEL_VFP       12
#else
static const jd9165_lcd_init_cmd_t s_panel_init[] = {
    CMD(0x30, 0x00), CMD(0xF7, 0x49, 0x61, 0x02, 0x00), CMD(0x30, 0x01), CMD(0x04, 0x0C), CMD(0x05, 0x00),
    CMD(0x06, 0x00), CMD(0x0B, 0x11), CMD(0x17, 0x00), CMD(0x20, 0x04), CMD(0x1F, 0x05), CMD(0x23, 0x00),
    CMD(0x25, 0x19), CMD(0x28, 0x18), CMD(0x29, 0x04), CMD(0x2A, 0x01), CMD(0x2B, 0x04), CMD(0x2C, 0x01),
    CMD(0x30, 0x02), CMD(0x01, 0x22), CMD(0x03, 0x12), CMD(0x04, 0x00), CMD(0x05, 0x64), CMD(0x0A, 0x08),
    CMD(0x0B, 0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D),
    CMD(0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0D, 0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C),
    CMD(0x0E, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0F, 0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07),
    CMD(0x10, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x11, 0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06),
    CMD(0x12, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x14, 0x00, 0x00, 0x11, 0x11), CMD(0x18, 0x99), CMD(0x30, 0x06),
    CMD(0x12, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x13, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x30, 0x0A), CMD(0x02, 0x4F), CMD(0x0B, 0x40), CMD(0x12, 0x3E), CMD(0x13, 0x78), CMD(0x30, 0x0D),
    CMD(0x0D, 0x04), CMD(0x10, 0x0C), CMD(0x11, 0x0C), CMD(0x12, 0x0C), CMD(0x13, 0x0C), CMD(0x30, 0x00),
    CMD_DELAY(0x11, 120),   /* SLPOUT */
    CMD_DELAY(0x29, 20),    /* DISPON */
};
#define PANEL_PCLK_MHZ  54
#define PANEL_HPW       40
#define PANEL_HBP       160
#define PANEL_HFP       160
#define PANEL_VPW       10
#define PANEL_VBP       23
#define PANEL_VFP       12
#endif

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

/* ------------------------------------------------------------------ */
/* Backlight                                                           */
/* ------------------------------------------------------------------ */

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = CONFIG_RADIO_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .flags.output_invert = BL_ACTIVE_LOW,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void board_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ------------------------------------------------------------------ */
/* Display + touch + LVGL                                              */
/* ------------------------------------------------------------------ */

/* The GT911 on this module is configured for 800x480; scale to the panel (as Guition's driver does) */
#define TP_NATIVE_W 800
#define TP_NATIVE_H 480

static void touch_scale(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                        uint8_t *point_num, uint8_t max_point_num)
{
    for (uint8_t i = 0; i < *point_num; i++) {
        x[i] = (uint16_t)((uint32_t)x[i] * BOARD_LCD_H_RES / TP_NATIVE_W);
        y[i] = (uint16_t)((uint32_t)y[i] * BOARD_LCD_V_RES / TP_NATIVE_H);
        if (x[i] >= BOARD_LCD_H_RES) x[i] = BOARD_LCD_H_RES - 1;
        if (y[i] >= BOARD_LCD_V_RES) y[i] = BOARD_LCD_V_RES - 1;
    }
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    backlight_init();

    /* MIPI D-PHY is powered from on-chip LDO channel 3 */
    static esp_ldo_channel_handle_t ldo_phy;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_phy), TAG, "ldo");

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PCLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,  /* two frame buffers in PSRAM: LVGL renders into one while the other scans out */
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = PANEL_HPW,
            .hsync_back_porch = PANEL_HBP,
            .hsync_front_porch = PANEL_HFP,
            .vsync_pulse_width = PANEL_VPW,
            .vsync_back_porch = PANEL_VBP,
            .vsync_front_porch = PANEL_VFP,
        },
        .flags.use_dma2d = true,
    };
    jd9165_vendor_config_t vendor_cfg = {
        .init_cmds = s_panel_init,
        .init_cmds_size = sizeof(s_panel_init) / sizeof(s_panel_init[0]),
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_RADIO_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(dbi_io, &panel_cfg, &panel), TAG, "jd9165");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(120));   /* JD9165 needs 120 ms after reset before the vendor table */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_LOGI(TAG, "JD9165 panel up (%s)", PANEL_IS_V2 ? "V2" : "V1");

    /* Touch: GT911 at 0x5D. RST/INT are not wired to the P4, and an I2C probe
     * right after the panel reset upsets the controller, so address it directly. */
    uint8_t tp_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = tp_addr;
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG, "touch io");
    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = tp_addr };
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = TP_SWAP_XY,
            .mirror_x = TP_MIRROR_X,
            .mirror_y = TP_MIRROR_Y,
        },
        .process_coordinates = touch_scale,
        .driver_data = &gt911_cfg,
    };
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t tp_err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);
    if (tp_err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 touch not found (0x%02X): %s", tp_addr, esp_err_to_name(tp_err));
        tp = NULL;
    } else {
        ESP_LOGI(TAG, "GT911 touch at 0x%02X", tp_addr);
    }

    /* LVGL */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 12 * 1024;
    port_cfg.task_affinity = 1;
    port_cfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = dbi_io,
        .panel_handle = panel,
        .control_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
            .direct_mode = true,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags.avoid_tearing = true };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl display");

    if (tp) {
        lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    }

    board_backlight_set(80);
    *out_disp = disp;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Audio: I2S + ES8311 + NS4150                                        */
/* ------------------------------------------------------------------ */

static esp_codec_dev_handle_t s_spk;
static bool s_spk_open;
static uint32_t s_spk_rate;
static int s_volume = 60;

esp_err_t board_audio_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");

    /* Amplifier off until the codec is running */
    if (CONFIG_RADIO_PA_GPIO >= 0) {
        gpio_config_t pa = { .pin_bit_mask = 1ULL << CONFIG_RADIO_PA_GPIO, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&pa);
        gpio_set_level(CONFIG_RADIO_PA_GPIO, 0);
    }

    /* Full duplex like the board's stock BSP: the ES8311 is clocked from one I2S port for both directions */
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;       /* send silence on underrun instead of repeating the last buffer */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s std tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s std rx");
    /* MCLK must be running before the ES8311 is configured */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s enable");

    audio_codec_i2s_cfg_t i2s_if_cfg = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);

    audio_codec_i2c_cfg_t i2c_if_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = CONFIG_RADIO_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8311 not responding on I2C");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_spk, ESP_FAIL, TAG, "codec dev");
    ESP_LOGI(TAG, "ES8311 ready");
    return ESP_OK;
}

esp_err_t board_audio_open(uint32_t sample_rate)
{
    if (!s_spk) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_spk_open && s_spk_rate == sample_rate) {
        return ESP_OK;
    }
    board_audio_close();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    int ret = esp_codec_dev_open(s_spk, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open %lu Hz failed (%d)", (unsigned long)sample_rate, ret);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    s_spk_open = true;

    s_spk_rate = sample_rate;
    ESP_LOGI(TAG, "audio out %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

void board_audio_close(void)
{
    if (s_spk && s_spk_open) {
        esp_codec_dev_close(s_spk);
    }
    s_spk_open = false;
    s_spk_rate = 0;
}

esp_err_t board_audio_write(const int16_t *pcm, size_t bytes)
{
    if (!s_spk_open) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_write(s_spk, (void *)pcm, (int)bytes) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void board_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_spk && s_spk_open) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

void board_audio_mute(bool mute)
{
    if (s_spk && s_spk_open) {
        esp_codec_dev_set_out_mute(s_spk, mute);
    }
}
