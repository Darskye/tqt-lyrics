#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// Particle visualisers shown whenever there is no lyric on screen.
//
// An honest note on "reacts to the music": it cannot, and does not pretend to.
// Spotify deprecated /audio-features and /audio-analysis in November 2024 and
// this app gets 403 from both (verified against the live API, not assumed),
// and the board has no microphone. There is no beat, tempo or loudness signal
// available anywhere.
//
// What drives these instead is real, just not audio:
//   - playback position, so motion advances with the song and the seek bar is
//     genuinely accurate
//   - a per-track seed seeded from the Spotify track id, so every song gets its
//     own visualiser, speed and density, consistent every time you play it
//
// Genuine reactivity needs an I2S microphone on the breakout pads. See README.

#define VIZ_COUNT 6

// Draws a full frame: visualiser, then track/artist and a seek bar along the
// bottom over a darkened band.
//   seed     -- per-track, picks the visualiser and its character
//   tSec     -- free-running seconds, for motion
//   progress -- 0..1 through the track, for the seek bar
void vizDraw(TFT_eSprite& s, uint32_t seed, float tSec, float progress,
             const char* track, const char* artist);

const char* vizName(uint32_t seed);
