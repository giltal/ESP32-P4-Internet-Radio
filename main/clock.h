/*
 * Wall clock: RX8025T RTC on the board's I2C bus (0x32) + SNTP, Israel time zone.
 * The RTC holds UTC, so summer time is always applied by the TZ rules, never baked in.
 */
#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Set the time zone and load the system time from the RTC (call before the UI). */
esp_err_t clock_init(i2c_master_bus_handle_t bus);

/** True once the time came from the RTC, the network or the user. */
bool clock_is_valid(void);

/** Set the clock from a local date/time (from the settings screen). Updates the RTC. */
esp_err_t clock_set_local(const struct tm *local);

/** Automatic time from the network (saved on the SD card; default on). */
bool clock_auto_ntp(void);
void clock_set_auto_ntp(bool on);
void clock_load_settings(void);

/** Called by Wi-Fi once online: start SNTP if automatic time is on. */
void clock_network_up(void);

/** Ask the network for the time right now (settings button), even if automatic time is off. */
void clock_sync_now(void);

#ifdef __cplusplus
}
#endif
