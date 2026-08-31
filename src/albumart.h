#pragma once
#include <Arduino.h>

// Derives a single ink colour from a track's album art.
//
// Fetches Spotify's smallest cover thumbnail (64x64, a few KB), decodes it,
// and picks the dominant *colourful* hue -- weighting each pixel by saturation
// so a mostly-grey sleeve with one red stripe reads as red rather than grey.
//
// Returns false when the art has nothing usable: all black, greyscale, or the
// fetch failed. Callers should use white in that case, which is also the right
// answer aesthetically for a black sleeve.
//
// `outInk` is RGB565, brightness-normalised so it stays legible on black.
bool artFetchInk(const char* imageUrl, uint16_t& outInk);
