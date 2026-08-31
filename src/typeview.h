#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

#define SCR_W 128
#define SCR_H 128

#define INK_ON  0xFFFF
#define INK_OFF 0x0000

// Draws one lyric line as the entire composition. There is no background layer
// underneath -- the type is the picture, so every scene owns the whole frame
// including clearing it.
//
// The scene is chosen deterministically from `seed` (the line index) so it
// holds for that line and changes on the next. Scenes that cannot fit the text
// fall back rather than clip.
//
//   ageMs  -- how long this line has been showing
//   holdMs -- how long until the next line takes over
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed);

// Shown between tracks, or when a track has no synced lyrics.
void typeDrawIdle(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms);

const char* typeSceneName(uint32_t seed);
