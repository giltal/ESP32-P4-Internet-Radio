/*
 * Station logos: downloaded once from the station's radio-browser "favicon" URL,
 * decoded (PNG, PNG-in-ICO, baseline JPEG), fitted into a LOGO_SIZE square and
 * cached on the SD card as a ready-to-draw ARGB8888 thumbnail.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOGO_SIZE 84

void logo_init(void);

/** Fetch the logo for this URL in the background (empty URL = no logo). */
void logo_request(const char *url);

/** Bumped whenever the current logo changes (loaded or cleared). */
uint32_t logo_revision(void);

/**
 * Copy the thumbnail for `url` (LOGO_SIZE x LOGO_SIZE ARGB8888, 4 bytes/pixel)
 * into dst. Returns false while it is loading or if the station has no usable logo.
 */
bool logo_copy(const char *url, uint8_t *dst);

#ifdef __cplusplus
}
#endif
