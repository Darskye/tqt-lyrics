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
// holds for that line and changes on the next. Type auto-fits: the size steps
// down until the whole line fits, so a long line is never clipped.
//
//   ageMs  -- how long this line has been showing
//   holdMs -- how long until the next line takes over
//   ink    -- the single colour everything is drawn in (white by default,
//             or a colour pulled from the album art)
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed,
              uint16_t ink = INK_ON);

// Shown between tracks, or when a track has no synced lyrics.
void typeDrawIdle(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms, uint16_t ink = INK_ON);

const char* typeSceneName(uint32_t seed);

// Result of the most recent fit: chosen size, rows used, size asked for,
// and whether even size 1 failed to contain the text (should never be true).
void typeLastFit(int& size, int& rows, int& asked, bool& clipped);
