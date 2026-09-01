#pragma once
#include <Arduino.h>

// Album art is shown during gaps: while paused, between lyric lines, or when a
// track has no synced lyrics at all.
//
// Spotify's smallest cover thumbnail is 64x64, which is exactly half the
// panel and decodes to 8KB -- small enough to keep a whole track's art
// resident and redraw it every frame with no refetching.
#define ART_W 64
#define ART_H 64
#define ART_PX (ART_W * ART_H)

// Fetches and decodes a cover into `dst` (must hold ART_PX uint16_t).
// Returns false on any failure; callers should fall back to a text-only card.
bool artFetchBitmap(const char* imageUrl, uint16_t* dst);
