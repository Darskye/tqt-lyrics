#pragma once
#include <Arduino.h>

// Album art is shown during gaps: while paused, between lyric lines, or when a
// track has no synced lyrics at all.
//
// Full-bleed: the cover fills the panel. Spotify's 64x64 thumbnail is far too
// soft for that, so the ~300px cover is fetched and the decoder scales it
// down, which keeps far more detail than upscaling a tiny one. 128x128x2 is
// 32KB, small enough to keep a whole track's art resident and redraw it every
// frame with no refetching.
#define ART_W 128
#define ART_H 128
#define ART_PX (ART_W * ART_H)

// Fetches and decodes a cover into `dst` (must hold ART_PX uint16_t).
// Returns false on any failure; callers should fall back to a text-only card.
bool artFetchBitmap(const char* imageUrl, uint16_t* dst);
