#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// White particle visualisers shown instead of, or alongside, the lyrics.
//
// These do not react to the audio and do not pretend to. Spotify deprecated
// /audio-features and /audio-analysis in November 2024 and this app gets 403
// from both (verified against the live API, not assumed from a changelog), and
// the board has no microphone. There is no beat or loudness signal anywhere.
// Motion comes from playback position and a per-track seed -- real, but not
// audio. Genuine reactivity needs an I2S mic on the breakout pads; see README.

#define VIZ_COUNT 13

// Draws a full frame: the chosen visualiser, then track/artist and a seek bar
// along the bottom over a darkened band.
//   index    -- which visualiser, 0..VIZ_COUNT-1
//   seed     -- per-track, varies speed/density within a visualiser
//   tSec     -- free-running seconds, for motion
//   progress -- 0..1 through the track, for the seek bar
void vizDraw(TFT_eSprite& s, int index, uint32_t seed, float tSec, float progress,
             const char* track, const char* artist);

// Draws only the field, no text or seek bar -- used behind lyrics.
void vizField(TFT_eSprite& s, int index, uint32_t seed, float tSec);

// The "lyricform" visualiser: particles drift chaotically and assemble into
// the current lyric line, then scatter again between lines.
//
// vizMorphSet rasterises a line into `mask` (an 8bpp scratch sprite) and
// harvests its lit pixels as particle targets -- call it when the line
// changes, not per frame. vizMorphAmount is the 0..1 assembly factor and is
// cheap enough to set every frame.
void vizMorphSet(TFT_eSprite& mask, const char* text);
void vizMorphAmount(float m);

int         vizIndexForSeed(uint32_t seed);
const char* vizNameAt(int index);
