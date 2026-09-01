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
// Scene, typeface and colour are all chosen deterministically from `seed` (the
// line index), so they hold steady for that line and change on the next.
// Type auto-fits down a ladder of faces and sizes, so a long line is never
// clipped -- it just arrives in a smaller face.
//
//   ageMs  -- how long this line has been showing
//   holdMs -- how long until the next line takes over
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed);

// Shown whenever there is no lyric to show: paused, between lines, or a track
// with no synced lyrics. Displays the cover art and the track name.
// `art` may be null / `haveArt` false, in which case it falls back to type.
void typeDrawCard(TFT_eSprite& s, const uint16_t* art, bool haveArt,
                  const char* track, const char* artist, const char* status,
                  uint32_t ms, uint32_t seed);

const char* typeSceneName(uint32_t seed);
const char* typeFaceName(uint32_t seed);
uint16_t    typeInk(uint32_t seed);

// Result of the most recent fit: rows used, and whether even the smallest rung
// failed to contain the text (should never be true).
void typeLastFit(int& rows, bool& clipped);
