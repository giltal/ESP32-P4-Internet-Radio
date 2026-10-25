/*
 * Weather from Open-Meteo (free, no API key): current conditions + daily forecast for one
 * location. The location (name, lat, lon) is chosen by a city search (OpenStreetMap Nominatim,
 * Open-Meteo geocoding as fallback)
 * and saved on the SD card; the default is Tel Aviv.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_DAYS 6

typedef struct {
    int year, month, day;
    int code;                       /* WMO weather code */
    float tmax, tmin;
    int rain_pct;                   /* max precipitation probability, -1 = unknown */
} weather_day_t;

typedef struct {
    bool valid;
    float temp;
    int code;
    bool is_day;
    int humidity;
    float wind_kmh;
    int ndays;
    weather_day_t days[WEATHER_DAYS];   /* days[0] = today */
    time_t updated;
    char place[64];
} weather_t;

/** Load the saved location (after sdcard_mount()) and start the background task. */
void weather_init(void);

/** Fetch now (e.g. right after Wi-Fi connects). */
void weather_refresh(void);

/** Bumped whenever the forecast, the location or the search status changes. */
uint32_t weather_revision(void);
void weather_get(weather_t *out);

/** Search a city by name (Hebrew or English); the best match becomes the location. */
void weather_search_city(const char *query);

/** Human-readable status of the last search / fetch (Hebrew), "" when all is fine. */
void weather_status(char *buf, size_t len);

/** Hebrew description and FontAwesome icon (UTF-8) for a WMO weather code. */
const char *weather_text(int code);
const char *weather_icon(int code, bool is_day);

#ifdef __cplusplus
}
#endif
