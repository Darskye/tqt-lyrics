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
// styleOverride: -1 picks scene and typeface from the seed, so each line
// differs. 0..TYPE_STYLE_COUNT-1 locks a specific pairing, which is what the
// left button steps through.
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed,
              int styleOverride = -1);

#define TYPE_STYLE_COUNT 18
const char* typeStyleName(int styleIndex);

// Shown only when nothing is playing at all -- a status line and a slow caret,
// so the panel never looks dead. Anything with a track behind it goes to the
// visualiser instead (viz.h).
void typeDrawCard(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms, uint32_t seed);

// Shown during a prolonged gap between lyric lines, and for tracks with no
// synced lyrics. Floating note glyphs with the track and artist beneath.
//
// The glyphs are drawn geometrically rather than typed: TFT_eSPI's built-in
// fonts and the GFX free fonts are ASCII only, so U+2669..U+266F (the music
// symbols) are simply not in them and would render as garbage.
void typeDrawNotes(TFT_eSprite& s, const char* track, const char* artist,
                   float tSec, uint32_t seed);

// Lays a line out exactly as the lyric scenes would and rasterises it into an
// 8bpp mask sprite. Returns the number of lit pixels, which the morph
// visualiser harvests as particle targets.
int typeRasterise(TFT_eSprite& mask, const char* text);

const char* typeSceneName(uint32_t seed);
const char* typeFaceName(uint32_t seed);
uint16_t    typeInk(uint32_t seed);

// Result of the most recent fit: rows used, and whether even the smallest rung
// failed to contain the text (should never be true).
void typeLastFit(int& rows, bool& clipped);

// The scene and typeface typeDraw actually resolved and drew.
void typeLastDrawn(const char*& scene, const char*& face);

// Which phrase of a split line is showing, and how many the line became.
// count == 1 means the line fitted whole and was not split.
void typeLastChunk(int& idx, int& count);
