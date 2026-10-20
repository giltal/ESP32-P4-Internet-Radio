#include "sdcard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "sdcard";

#define MOUNT_POINT   "/sdcard"
#define WIFI_FILE     MOUNT_POINT "/WiFi.txt"
#define SD_LDO_CHAN   4     /* VDD_SDIO of the card slot comes from the P4's LDO4 */

/* Trim whitespace, a UTF-8 BOM and surrounding quotes in place */
static char *trim(char *s)
{
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s += 3;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    if (e - s >= 2 && (s[0] == '"' || s[0] == '\'') && e[-1] == s[0]) {
        e[-1] = 0;
        s++;
    }
    return s;
}

static bool key_is(const char *k, const char *const *names)
{
    for (; *names; names++) {
        if (strcasecmp(k, *names) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_wifi_file(FILE *f, char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    static const char *const ssid_keys[] = { "ssid", "wifi_ssid", "name", "network", NULL };
    static const char *const pass_keys[] = { "password", "pass", "pwd", "psk", "key", "wifi_password", NULL };
    char line[160];
    int plain = 0;              /* lines without a key: 1st = SSID, 2nd = password */

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') {
            continue;
        }
        char *sep = strpbrk(s, "=:");
        if (sep) {
            char kv[sizeof(line)];
            strlcpy(kv, s, sizeof(kv));
            kv[sep - s] = 0;
            char *k = trim(kv);
            char *v = trim(kv + (sep - s) + 1);
            if (key_is(k, ssid_keys)) {
                strlcpy(ssid, v, ssid_len);
                continue;
            }
            if (key_is(k, pass_keys)) {
                strlcpy(pass, v, pass_len);
                continue;
            }
            /* unknown key: treat the whole line as plain text */
        }
        if (plain == 0) {
            strlcpy(ssid, s, ssid_len);
        } else if (plain == 1) {
            strlcpy(pass, s, pass_len);
        }
        plain++;
    }
}

esp_err_t sdcard_read_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    ssid[0] = 0;
    pass[0] = 0;

    sd_pwr_ctrl_handle_t pwr = NULL;
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO power control failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;

    const esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "card mounted: %s, %llu MB", card->cid.name,
             ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));

    FILE *f = fopen(WIFI_FILE, "r");
    if (f) {
        parse_wifi_file(f, ssid, ssid_len, pass, pass_len);
        fclose(f);
        if (ssid[0]) {
            ESP_LOGI(TAG, "WiFi.txt: SSID \"%s\", password %s", ssid, pass[0] ? "set" : "empty (open network)");
        } else {
            ESP_LOGW(TAG, "WiFi.txt has no SSID");
        }
    } else {
        ESP_LOGW(TAG, "%s not found", WIFI_FILE);
    }

    /* Release the SDMMC host before esp_hosted claims it for the C6 link */
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    sd_pwr_ctrl_del_on_chip_ldo(pwr);
    return ssid[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}
