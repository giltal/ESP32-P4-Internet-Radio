/*
 * Station directory: pulls stations for one country from radio-browser.info,
 * keeps only streams the ESP32-P4 can play (plain MP3 / AAC, no HLS),
 * caches the list in NVS and manages favorites.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[72];
    char url[256];
    char codec[8];      /* "MP3" or "AAC" */
    uint16_t bitrate;   /* kbps, 0 = unknown */
} station_t;

typedef enum {
    STATION_LIST_ALL = 0,
    STATION_LIST_FAVORITES,
} station_list_t;

/** Allocate the in-memory lists (empty until station_db_load()). */
esp_err_t station_db_init(void);

/** Load the cached directory and favorites from the SD card (after sdcard_mount()). */
void station_db_load(void);

/** Download a fresh directory (blocking; call from a worker task). */
esp_err_t station_db_refresh(void);

/** Kick off station_db_refresh() in a background task. */
void station_db_refresh_async(void);

bool station_db_busy(void);

/** Incremented whenever that list changes; the UI polls it. */
uint32_t station_db_revision(station_list_t list);

/** Incremented whenever the status text or busy flag changes. */
uint32_t station_db_status_revision(void);

size_t station_db_count(station_list_t list);

/** Copy entry `index` of `list` into `out`. */
bool station_db_get(station_list_t list, size_t index, station_t *out);

bool station_db_is_favorite(const station_t *st);
void station_db_set_favorite(const station_t *st, bool fav);

/** Status line for the UI ("200 stations", "Updating…", error text). */
void station_db_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
