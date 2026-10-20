/*
 * "Now playing" from station websites, for streams that carry no song titles.
 * Currently: Kan (kan.org.il arc-cloud live-track API) for Kan 88, Kol HaMusica,
 * Tarbut, Reshet Bet and Gimel. Galgalatz/GLZ publish only programme names.
 */
#pragma once

#include "station_db.h"

#ifdef __cplusplus
extern "C" {
#endif

void nowplaying_init(void);

/** Follow this station (NULL = stopped). Called by the player on play/stop. */
void nowplaying_track(const station_t *st);

#ifdef __cplusplus
}
#endif
