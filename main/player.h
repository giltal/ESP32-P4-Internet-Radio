/*
 * Streaming player: HTTP(S) / Icecast / Shoutcast -> ring buffer -> MP3/AAC
 * decoder -> ES8311. Handles redirects (StreamTheWorld), .pls/.m3u playlists,
 * ICY "now playing" metadata and automatic reconnects.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "station_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_CONNECTING,
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    station_t station;
    char title[160];        /* ICY StreamTitle, UTF-8 */
    char error[80];
    char codec[8];
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t bitrate_kbps;
    uint8_t buffer_pct;     /* ring buffer fill */
    uint8_t level;          /* output peak 0..100, for a level meter */
    uint32_t revision;      /* changes whenever title/station/state changes */
} player_status_t;

esp_err_t player_init(void);
void player_play(const station_t *st);
void player_stop(void);
void player_set_volume(int volume);
int player_get_volume(void);
/** Persist the current volume (call when the user lets go of the slider). */
/** Restore the saved volume from the SD card (after sdcard_mount()). */
void player_load_settings(void);
void player_save_volume(void);
/** Station played last (restored at boot). */
bool player_last_station(station_t *out);
void player_get_status(player_status_t *out);

#ifdef __cplusplus
}
#endif
