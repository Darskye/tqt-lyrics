#include "typeview.h"
#include <string.h>
#include <stdio.h>

// The free fonts arrive with TFT_eSPI.h: its Fonts/GFXFF/gfxfont.h is an
// aggregator that includes the whole set. Those headers carry no include
// guards, so including one here again is a redefinition error.


#define ROW_MAX   16
#define ROW_CHARS 64

#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// ---------------------------------------------------------------- faces
// A rung is one typeface at one size. gfx == nullptr means the built-in 6x8
// GLCD cell, which scales by whole multiples and is the guaranteed floor:
// 21 columns by 16 rows holds ~330 characters, longer than any lyric line.
struct Rung { const GFXfont* gfx; uint8_t size; };
struct Face { const char* name; const Rung* rungs; uint8_t n; };

static const Rung R_SANS[] = {
  {&FreeSansBold24pt7b, 1}, {&FreeSansBold18pt7b, 1}, {&FreeSansBold12pt7b, 1},
  {&FreeSansBold9pt7b, 1},  {&FreeSans9pt7b, 1},      {nullptr, 1},
};
static const Rung R_SERIF[] = {
  {&FreeSerifBold24pt7b, 1}, {&FreeSerifBold18pt7b, 1},
  {&FreeSerifBold12pt7b, 1}, {&FreeSerif9pt7b, 1},    {nullptr, 1},
};
static const Rung R_MONO[] = {
  {&FreeMonoBold18pt7b, 1}, {&FreeMonoBold12pt7b, 1},
  {&FreeMonoBold9pt7b, 1},  {&FreeMono9pt7b, 1},      {nullptr, 1},
};
static const Rung R_OBLIQUE[] = {
  {&FreeSansBoldOblique24pt7b, 1}, {&FreeSansBoldOblique18pt7b, 1},
  {&FreeSansBoldOblique12pt7b, 1}, {&FreeSansOblique9pt7b, 1}, {nullptr, 1},
};
static const Rung R_PIXEL[] = {
  {nullptr, 5}, {nullptr, 4}, {nullptr, 3}, {nullptr, 2}, {nullptr, 1},
};

static const Face kFaces[] = {
  {"sans",    R_SANS,    (uint8_t)(sizeof(R_SANS)    / sizeof(Rung))},
  {"serif",   R_SERIF,   (uint8_t)(sizeof(R_SERIF)   / sizeof(Rung))},
  {"mono",    R_MONO,    (uint8_t)(sizeof(R_MONO)    / sizeof(Rung))},
  {"oblique", R_OBLIQUE, (uint8_t)(sizeof(R_OBLIQUE) / sizeof(Rung))},
  {"pixel",   R_PIXEL,   (uint8_t)(sizeof(R_PIXEL)   / sizeof(Rung))},
};
static const int kFaceCount = sizeof(kFaces) / sizeof(kFaces[0]);
#define FACE_SANS  kFaces[0]
#define FACE_PIXEL kFaces[4]

// ---------------------------------------------------------------- ink
// A curated set rather than truly random RGB: arbitrary values land on muddy
// or too-dark colours often enough to matter on a black field. These are all
// high-value and legible.
static const uint16_t kInks[] = {
  RGB565(255, 255, 255), RGB565(255,  70,  70), RGB565(255, 140,  40),
  RGB565(255, 214,   0), RGB565(170, 255,  60), RGB565( 50, 255, 150),
  RGB565(  0, 232, 255), RGB565( 80, 165, 255), RGB565(150, 125, 255),
  RGB565(230, 100, 255), RGB565(255,  95, 185), RGB565(255, 185, 140),
};
static const int kInkCount = sizeof(kInks) / sizeof(kInks[0]);

// ---------------------------------------------------------------- scenes
enum Scene {
  SC_HERO, SC_STACK, SC_INVERT, SC_WIPE, SC_SCROLL,
  SC_TYPE, SC_FLASH, SC_RULE, SC_BOX, SC_SPLIT,
  SC_ZOOM, SC_WIPEUP, SC_GLITCH, SC_SHADOW, SC_MIRROR,
  SC_DECODE, SC_ROWFADE, SC_STAMP, SC_COUNT
};

static const char* kSceneNames[SC_COUNT] = {
  "hero", "stack", "invert", "wipe", "scroll",
  "type", "flash", "rule", "box", "split",
  "zoom", "wipeup", "glitch", "shadow", "mirror",
  "decode", "rowfade", "stamp"
};

struct Layout {
  Rung rung;
  int  nRows, lineH, blockH;
  char rows[ROW_MAX][ROW_CHARS];
};

static int  g_lastRows = 0;
static bool g_lastClipped = false;
// What typeDraw actually resolved and drew, as opposed to what was requested.
// Reporting the request cannot detect the request being ignored.
static int  g_drawnScene = -1;
static int  g_drawnFace  = -1;
void typeLastFit(int& rows, bool& clipped) { rows = g_lastRows; clipped = g_lastClipped; }

void typeLastDrawn(const char*& scene, const char*& face) {
  scene = (g_drawnScene >= 0) ? kSceneNames[g_drawnScene] : "-";
  face  = (g_drawnFace  >= 0) ? kFaces[g_drawnFace].name  : "-";
}

static float easeOutCubic(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

// Three independent draws from one seed, so scene, face and colour vary
// independently instead of moving in lockstep.
static uint32_t mix(uint32_t seed, uint32_t salt) {
  uint32_t h = (seed + salt * 0x9E3779B9u) * 2654435761u;
  h ^= h >> 15;
  return h;
}

static Scene sceneFromSeed(uint32_t seed) { return (Scene)(mix(seed, 1) % SC_COUNT); }
static const Face& faceFromSeed(uint32_t seed) { return kFaces[mix(seed, 2) % kFaceCount]; }

// Everything draws white. The palette below is kept because the machinery for
// per-line colour is otherwise intact -- swap the return for
// `kInks[mix(seed, 3) % kInkCount]` to turn it back on.
uint16_t    typeInk(uint32_t seed)       { (void)seed; return INK_ON; }
const char* typeSceneName(uint32_t seed) { return kSceneNames[sceneFromSeed(seed)]; }
const char* typeFaceName(uint32_t seed)  { return faceFromSeed(seed).name; }

// ---------------------------------------------------------------- text utils
static int wordCount(const char* s) {
  int n = 0;
  bool in = false;
  for (; *s; s++) {
    if (*s == ' ') in = false;
    else if (!in) { in = true; n++; }
  }
  return n;
}

static void nthWord(const char* text, int idx, char* out, int outSize) {
  const char* p = text;
  for (int i = 0; i < idx; i++) {
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;
  }
  while (*p == ' ') p++;
  int n = 0;
  while (p[n] && p[n] != ' ' && n < outSize - 1) { out[n] = p[n]; n++; }
  out[n] = 0;
}

static void longestWord(const char* text, char* out, int outSize) {
  int wc = wordCount(text);
  int best = 0, bestLen = -1;
  char buf[ROW_CHARS];
  for (int i = 0; i < wc; i++) {
    nthWord(text, i, buf, sizeof(buf));
    int L = (int)strlen(buf);
    if (L > bestLen) { bestLen = L; best = i; }
  }
  nthWord(text, best, out, outSize);
}

static void applyRung(TFT_eSprite& s, const Rung& r) {
  if (r.gfx) s.setFreeFont(r.gfx);
  else       s.setTextFont(1);         // also clears any free font
  s.setTextSize(r.size);
}


// Draws a row character by character. TFT_eSPI has no per-glyph hook, so each
// character is its own drawString and the pen advances by that glyph's own
// measured width -- which keeps proportional faces spaced correctly.
static void drawRowChars(TFT_eSprite& s, const char* row, int x, int y,
                         int resolved, uint32_t seed) {
  static const char pool[] = "#%&$@*+=?/|<>~^";
  char ch[2] = {0, 0};
  int pen = x;
  uint32_t frame = (uint32_t)(millis() / 45);
  for (int i = 0; row[i]; i++) {
    ch[0] = row[i];
    if (ch[0] != ' ' && i >= resolved)
      ch[0] = pool[mix(seed ^ (uint32_t)(i * 2654435761u), frame) % (sizeof(pool) - 1)];
    s.drawString(ch, pen, y);
    pen += s.textWidth(ch);
  }
}

static int rungIndexOf(const Face& face, const Rung& r) {
  for (uint8_t i = 0; i < face.n; i++)
    if (face.rungs[i].gfx == r.gfx && face.rungs[i].size == r.size) return i;
  return face.n - 1;
}

// Greedy word wrap against whatever face is currently set.
// Sets *overflow when text remained after maxRows -- the caller must not just
// draw what it got, because the rest would be silently lost.
static int wrapText(TFT_eSprite& s, const char* txt, char rows[][ROW_CHARS],
                    int maxRows, int maxW, bool* overflow) {
  if (overflow) *overflow = false;
  int n = 0;
  const char* p = txt;
  char cur[ROW_CHARS];
  cur[0] = 0;

  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;

    const char* ws = p;
    while (*p && *p != ' ') p++;
    int wlen = (int)(p - ws);
    if (wlen > ROW_CHARS - 1) wlen = ROW_CHARS - 1;
    char word[ROW_CHARS];
    memcpy(word, ws, wlen);
    word[wlen] = 0;

    char trial[ROW_CHARS * 2];
    if (cur[0]) snprintf(trial, sizeof(trial), "%s %s", cur, word);
    else        snprintf(trial, sizeof(trial), "%s", word);

    if (s.textWidth(trial) <= maxW || cur[0] == 0) {
      strncpy(cur, trial, ROW_CHARS - 1);
      cur[ROW_CHARS - 1] = 0;
    } else {
      if (n >= maxRows) { if (overflow) *overflow = true; return n; }
      strncpy(rows[n], cur, ROW_CHARS - 1); rows[n][ROW_CHARS - 1] = 0; n++;
      strncpy(cur, word, ROW_CHARS - 1);    cur[ROW_CHARS - 1] = 0;
    }
  }
  if (cur[0]) {
    if (n >= maxRows) { if (overflow) *overflow = true; return n; }
    strncpy(rows[n], cur, ROW_CHARS - 1); rows[n][ROW_CHARS - 1] = 0; n++;
  }
  return n;
}

// Walks the face's ladder from largest to smallest and takes the first rung
// where the ENTIRE string fits. The last rung is accepted unconditionally as
// the floor, so text is never dropped -- it just arrives smaller.
static void layoutFit(TFT_eSprite& s, const char* txt, const Face& face,
                      int maxW, int maxH, Layout& L) {
  for (uint8_t i = 0; i < face.n; i++) {
    const Rung& r = face.rungs[i];
    applyRung(s, r);

    bool ovf = false;
    int n = wrapText(s, txt, L.rows, ROW_MAX, maxW, &ovf);
    int lineH = s.fontHeight() + 1;

    bool tooWide = false;
    for (int k = 0; k < n; k++)
      if (s.textWidth(L.rows[k]) > maxW) { tooWide = true; break; }

    bool fits = !ovf && n > 0 && !tooWide && (n * lineH) <= maxH;
    if (fits || i == face.n - 1) {
      L.rung   = r;
      L.nRows  = n;
      L.lineH  = lineH;
      L.blockH = n * lineH;
      g_lastRows    = n;
      g_lastClipped = !fits;
      return;
    }
  }
}


// ---------------------------------------------------------------- chunking
// A long line used to shrink until it fitted, which on a 128px panel meant
// unreadably small type. Instead it is split into the fewest phrases that each
// fit at a decently large face, shown in sequence across the line's own
// duration. Each phrase gets time proportional to its length, which tracks the
// singing better than dividing the line evenly -- LRC gives per-line stamps
// only, so within a line this is an approximation either way.
#define CHUNK_MAX   6
#define CHUNK_CHARS (ROW_CHARS * 2)
// The face phrases are measured against, counted from the LARGE end of the
// ladder. Measuring from the small end let a 50-character line "fit" at a tiny
// face and never split, which is the thing this is meant to prevent. Index 2
// is roughly 12pt: big enough to read across the room, small enough that a
// line rarely explodes into more than three or four phrases.
#define CHUNK_RUNG 2

// Hard cap on rows per phrase. Height alone is not enough of a constraint: at
// a small face five rows still "fit", so a long line never split and stayed
// unreadable. Capping rows is what actually forces big type and more
// transitions.
#define PHRASE_MAX_ROWS 3

struct Chunks {
  int  n;
  int  totalLen;
  char text[CHUNK_MAX][CHUNK_CHARS];
};

// Rows a candidate string needs at the face currently applied.
static int rowsNeeded(TFT_eSprite& s, const char* txt, int maxW) {
  char rows[ROW_MAX][ROW_CHARS];
  bool ovf = false;
  int n = wrapText(s, txt, rows, ROW_MAX, maxW, &ovf);
  return ovf ? ROW_MAX + 1 : n;
}

static void chunkLine(TFT_eSprite& s, const char* text, const Face& face,
                      int maxW, int maxH, Chunks& C) {
  int rung = CHUNK_RUNG;
  if (rung > face.n - 1) rung = face.n - 1;
  applyRung(s, face.rungs[rung]);

  int lineH   = s.fontHeight() + 1;
  int maxRows = maxH / (lineH > 0 ? lineH : 1);
  if (maxRows > PHRASE_MAX_ROWS) maxRows = PHRASE_MAX_ROWS;
  if (maxRows < 1) maxRows = 1;

  C.n = 0;
  C.totalLen = (int)strlen(text);

  char cur[CHUNK_CHARS];
  cur[0] = 0;
  const char* p = text;

  while (*p && C.n < CHUNK_MAX) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* ws = p;
    while (*p && *p != ' ') p++;
    int wlen = (int)(p - ws);
    if (wlen > CHUNK_CHARS - 1) wlen = CHUNK_CHARS - 1;
    char word[CHUNK_CHARS];
    memcpy(word, ws, wlen);
    word[wlen] = 0;

    char trial[CHUNK_CHARS];
    if (cur[0]) snprintf(trial, sizeof(trial), "%s %s", cur, word);
    else        snprintf(trial, sizeof(trial), "%s", word);

    if (rowsNeeded(s, trial, maxW) <= maxRows || cur[0] == 0) {
      strncpy(cur, trial, CHUNK_CHARS - 1);
      cur[CHUNK_CHARS - 1] = 0;
    } else {
      strncpy(C.text[C.n], cur, CHUNK_CHARS - 1);
      C.text[C.n][CHUNK_CHARS - 1] = 0;
      C.n++;
      strncpy(cur, word, CHUNK_CHARS - 1);
      cur[CHUNK_CHARS - 1] = 0;
    }
  }
  if (cur[0] && C.n < CHUNK_MAX) {
    strncpy(C.text[C.n], cur, CHUNK_CHARS - 1);
    C.text[C.n][CHUNK_CHARS - 1] = 0;
    C.n++;
  }
  if (C.n == 0) {                       // degenerate: keep the line intact
    strncpy(C.text[0], text, CHUNK_CHARS - 1);
    C.text[0][CHUNK_CHARS - 1] = 0;
    C.n = 1;
  }
}

// Chunking walks the text repeatedly; caching keeps it off the per-frame path.
static Chunks      g_chunks;
static char        g_chunkKey[CHUNK_CHARS] = {0};
static const Face* g_chunkFace = nullptr;
static int         g_chunkIdx = 0;

static const Chunks& chunksFor(TFT_eSprite& s, const char* text, const Face& face,
                               int maxW, int maxH) {
  if (g_chunkFace != &face || strncmp(g_chunkKey, text, CHUNK_CHARS - 1) != 0) {
    chunkLine(s, text, face, maxW, maxH, g_chunks);
    strncpy(g_chunkKey, text, CHUNK_CHARS - 1);
    g_chunkKey[CHUNK_CHARS - 1] = 0;
    g_chunkFace = &face;
  }
  return g_chunks;
}

void typeLastChunk(int& idx, int& count) { idx = g_chunkIdx; count = g_chunks.n; }

// ---------------------------------------------------------------- draw
const char* typeStyleName(int i) {
  return kSceneNames[((i % SC_COUNT) + SC_COUNT) % SC_COUNT];
}

void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed,
              int styleOverride) {
  s.fillSprite(INK_OFF);
  if (!text || !*text) return;

  // A locked style pairs a scene with a typeface. 18 and 5 are coprime, so
  // stepping the index walks every scene and keeps changing the face too.
  Scene sc;
  const Face* fp;
  if (styleOverride >= 0) {
    sc = (Scene)(styleOverride % SC_COUNT);
    fp = &kFaces[styleOverride % kFaceCount];
  } else {
    sc = sceneFromSeed(seed);
    fp = &faceFromSeed(seed);
  }
  const Face& face = *fp;
  uint16_t ink = typeInk(seed);
  g_drawnFace = (int)(fp - kFaces);

  // Split a long line into big phrases rather than shrinking it to fit, then
  // pick whichever phrase this moment belongs to. Time is shared out in
  // proportion to phrase length, which follows the singing more closely than
  // an even split. LRC carries per-line stamps only, so within a line this is
  // an approximation however it is done.
  const Chunks& C = chunksFor(s, text, face, SCR_W - 12, SCR_H - 18);
  if (C.n > 1) {
    uint32_t span = holdMs ? holdMs : 3000;
    uint32_t acc = 0, pickStart = 0, pickSpan = span;
    int pick = C.n - 1;
    for (int i = 0; i < C.n; i++) {
      uint32_t slice = (uint32_t)((uint64_t)span * strlen(C.text[i]) /
                                  (uint32_t)(C.totalLen ? C.totalLen : 1));
      if (slice < 300) slice = 300;      // never flash past faster than readable
      if (ageMs < acc + slice || i == C.n - 1) {
        pick = i; pickStart = acc; pickSpan = slice;
        break;
      }
      acc += slice;
    }
    g_chunkIdx = pick;
    text   = C.text[pick];
    ageMs  = (ageMs > pickStart) ? ageMs - pickStart : 0;
    holdMs = pickSpan;
  } else {
    g_chunkIdx = 0;
  }

  int wc = wordCount(text);
  if (sc == SC_HERO  && wc < 2) sc = SC_STACK;
  if (sc == SC_SPLIT && wc < 2) sc = SC_BOX;
  if (sc == SC_SCROLL && strlen(text) < 18) sc = SC_RULE;
  g_drawnScene = (int)sc;

  float in = easeOutCubic((float)ageMs / 300.0f);
  Layout L;

  switch (sc) {
    case SC_HERO: {
      char key[ROW_CHARS];
      longestWord(text, key, sizeof(key));

      Layout K;
      layoutFit(s, key, face, SCR_W - 6, 58, K);
      applyRung(s, K.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      s.drawString(K.rows[0], SCR_W / 2, 48 + (int)((1.0f - in) * 10.0f));

      layoutFit(s, text, FACE_PIXEL, SCR_W - 8, 40, L);
      applyRung(s, L.rung);
      int y0 = 98 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    case SC_STACK: {
      layoutFit(s, text, face, SCR_W - 10, SCR_H - 12, L);
      applyRung(s, L.rung);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = (SCR_H - L.blockH) / 2;
      for (int i = 0; i < L.nRows; i++) {
        float li = easeOutCubic(((float)ageMs - i * 55.0f) / 300.0f);
        s.drawString(L.rows[i], 6 - (int)((1.0f - li) * 60.0f), y0 + i * L.lineH);
      }
      break;
    }

    case SC_INVERT: {
      s.fillSprite(ink);
      layoutFit(s, text, face, SCR_W - 14, SCR_H - 16, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(INK_OFF, ink);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    case SC_WIPE: {
      layoutFit(s, text, face, SCR_W - 14, SCR_H - 16, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      int cut = (int)(in * SCR_W);
      if (cut < SCR_W) {
        s.fillRect(cut, 0, SCR_W - cut, SCR_H, INK_OFF);
        s.fillRect(cut, 0, 2, SCR_H, ink);
      }
      break;
    }

    case SC_SCROLL: {
      // One row travelling across, so it only has to fit vertically. Take a
      // mid rung of the ladder: big enough to read, short enough to clear.
      applyRung(s, face.rungs[face.n >= 3 ? face.n - 3 : 0]);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);
      int w = s.textWidth(text);
      uint32_t span = holdMs ? holdMs : 3000;
      float t = span ? (float)ageMs / (float)span : 0.0f;
      if (t > 1.0f) t = 1.0f;
      int y = (SCR_H - s.fontHeight()) / 2;
      s.drawString(text, SCR_W - (int)(t * (w + SCR_W)), y);
      s.fillRect(0, y - 10, SCR_W, 2, ink);
      s.fillRect(0, y + s.fontHeight() + 8, SCR_W, 2, ink);
      g_lastRows = 1;
      g_lastClipped = false;
      break;
    }

    case SC_TYPE: {
      // Measure on the full line so the block does not jump as characters land.
      layoutFit(s, text, face, SCR_W - 10, SCR_H - 12, L);

      uint32_t span = holdMs ? (holdMs * 55 / 100) : 900;
      if (span < 150) span = 150;
      int total  = (int)strlen(text);
      int reveal = (int)((uint64_t)ageMs * (uint64_t)total / (uint64_t)span);
      if (reveal > total) reveal = total;

      char partial[ROW_CHARS * 4];
      int take = reveal;
      if (take > (int)sizeof(partial) - 1) take = (int)sizeof(partial) - 1;
      memcpy(partial, text, take);
      partial[take] = 0;

      applyRung(s, L.rung);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);

      char rows[ROW_MAX][ROW_CHARS];
      bool ovf = false;
      int n = wrapText(s, partial, rows, ROW_MAX, SCR_W - 10, &ovf);
      int y0 = (SCR_H - L.blockH) / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], 5, y0 + i * L.lineH);

      if (n > 0 && ((ageMs / 300) & 1) == 0) {
        int cw = s.textWidth(rows[n - 1]);
        int ch = s.fontHeight() - 2;
        if (ch < 4) ch = 4;
        s.fillRect(5 + cw + 2, y0 + (n - 1) * L.lineH, 6, ch, ink);
      }
      break;
    }

    case SC_FLASH: {
      bool flip = ageMs < 90 || (ageMs > 140 && ageMs < 200);
      if (flip) s.fillSprite(ink);
      layoutFit(s, text, face, SCR_W - 14, SCR_H - 16, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(flip ? INK_OFF : ink, flip ? ink : INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    case SC_RULE: {
      layoutFit(s, text, face, SCR_W - 16, SCR_H - 40, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      int barW = (int)(in * SCR_W);
      s.fillRect(0, SCR_H / 2 - L.blockH / 2 - 12, barW, 4, ink);
      s.fillRect(SCR_W - barW, SCR_H / 2 + L.blockH / 2 + 8, barW, 4, ink);
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    case SC_BOX: {
      layoutFit(s, text, face, SCR_W - 24, SCR_H - 32, L);
      int boxH = L.blockH + 16;
      int h = (int)(in * boxH);
      int y = SCR_H / 2 - h / 2;
      s.fillRect(0, y, SCR_W, h, ink);
      if (h >= boxH - 1) {
        applyRung(s, L.rung);
        s.setTextDatum(MC_DATUM);
        s.setTextColor(INK_OFF, ink);
        int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
        for (int i = 0; i < L.nRows; i++)
          s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      }
      break;
    }

    case SC_SPLIT: {
      int half = (wc + 1) / 2;
      char a[ROW_CHARS * 3] = {0}, b[ROW_CHARS * 3] = {0};
      char w[ROW_CHARS];
      for (int i = 0; i < wc; i++) {
        nthWord(text, i, w, sizeof(w));
        char* dst = (i < half) ? a : b;
        if (*dst) strncat(dst, " ", 2);
        strncat(dst, w, ROW_CHARS - 1);
      }

      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int offs = (int)((1.0f - in) * 130.0f);

      Layout A, B;
      layoutFit(s, a, face, SCR_W - 8, 54, A);
      applyRung(s, A.rung);
      int ay = 34 - (A.nRows - 1) * A.lineH / 2;
      for (int i = 0; i < A.nRows; i++)
        s.drawString(A.rows[i], SCR_W / 2 - offs, ay + i * A.lineH);

      layoutFit(s, b, face, SCR_W - 8, 54, B);
      applyRung(s, B.rung);
      int by = 92 - (B.nRows - 1) * B.lineH / 2;
      for (int i = 0; i < B.nRows; i++)
        s.drawString(B.rows[i], SCR_W / 2 + offs, by + i * B.lineH);
      break;
    }

    // Grows into place: starts a few rungs down the ladder and steps up.
    case SC_ZOOM: {
      layoutFit(s, text, face, SCR_W - 12, SCR_H - 14, L);
      int use = rungIndexOf(face, L.rung) + (int)((1.0f - in) * 3.0f);
      if (use > face.n - 1) use = face.n - 1;
      applyRung(s, face.rungs[use]);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int lh = s.fontHeight() + 1;
      char rows[ROW_MAX][ROW_CHARS];
      bool ovf = false;
      int n = wrapText(s, text, rows, ROW_MAX, SCR_W - 12, &ovf);
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      break;
    }

    // Shutter retreating downward instead of across.
    case SC_WIPEUP: {
      layoutFit(s, text, face, SCR_W - 14, SCR_H - 16, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      int cut = (int)(in * SCR_H);
      if (cut < SCR_H) {
        s.fillRect(0, 0, SCR_W, SCR_H - cut, INK_OFF);
        s.fillRect(0, SCR_H - cut, SCR_W, 2, ink);
      }
      break;
    }

    // Rows tear sideways on a fast frame clock, with occasional bigger slips.
    case SC_GLITCH: {
      layoutFit(s, text, face, SCR_W - 10, SCR_H - 12, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      uint32_t frame = ageMs / 70;
      for (int i = 0; i < L.nRows; i++) {
        uint32_t h = mix(seed ^ (uint32_t)(i * 977u), frame);
        int dx = (int)(h % 9) - 4;
        if (((h >> 8) % 11) == 0) dx = (int)((h >> 12) % 27) - 13;
        s.drawString(L.rows[i], SCR_W / 2 + dx, y0 + i * L.lineH);
      }
      break;
    }

    // A dim offset copy behind the type, closing in as the line lands.
    case SC_SHADOW: {
      layoutFit(s, text, face, SCR_W - 14, SCR_H - 16, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      int off = 2 + (int)((1.0f - in) * 8.0f);
      s.setTextColor(0x4208);
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2 + off, y0 + i * L.lineH + off);
      s.setTextColor(ink);
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    // The block sits high with a dimmed echo beneath it.
    case SC_MIRROR: {
      layoutFit(s, text, face, SCR_W - 14, 50, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      int y0 = 42 - (L.nRows - 1) * L.lineH / 2;
      s.setTextColor(ink);
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      s.setTextColor(0x39E7);
      int my = 78 + (int)((1.0f - in) * 12.0f);
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[L.nRows - 1 - i], SCR_W / 2, my + i * L.lineH);
      break;
    }

    // Characters settle out of noise, left to right across the whole line.
    case SC_DECODE: {
      layoutFit(s, text, face, SCR_W - 10, SCR_H - 12, L);
      applyRung(s, L.rung);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);
      uint32_t span = holdMs ? (holdMs * 60 / 100) : 900;
      if (span < 200) span = 200;
      int total = (int)strlen(text);
      int resolved = (int)((uint64_t)ageMs * (uint64_t)total / (uint64_t)span);
      int y0 = (SCR_H - L.blockH) / 2;
      int consumed = 0;
      for (int i = 0; i < L.nRows; i++) {
        int w = s.textWidth(L.rows[i]);
        drawRowChars(s, L.rows[i], (SCR_W - w) / 2, y0 + i * L.lineH,
                     resolved - consumed, seed);
        consumed += (int)strlen(L.rows[i]) + 1;
      }
      break;
    }

    // Rows arrive one at a time from alternating sides.
    case SC_ROWFADE: {
      layoutFit(s, text, face, SCR_W - 12, SCR_H - 14, L);
      applyRung(s, L.rung);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++) {
        float li = easeOutCubic(((float)ageMs - i * 110.0f) / 340.0f);
        if (li <= 0.0f) continue;
        int dx = (int)((1.0f - li) * ((i & 1) ? 120.0f : -120.0f));
        s.drawString(L.rows[i], SCR_W / 2 + dx, y0 + i * L.lineH);
      }
      break;
    }

    // Lands oversized for a beat, then settles onto the fitted size.
    case SC_STAMP: {
      layoutFit(s, text, face, SCR_W - 12, SCR_H - 14, L);
      int fitIdx = rungIndexOf(face, L.rung);
      int use = (ageMs < 130 && fitIdx > 0) ? fitIdx - 1 : fitIdx;
      applyRung(s, face.rungs[use]);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int lh = s.fontHeight() + 1;
      char rows[ROW_MAX][ROW_CHARS];
      bool ovf = false;
      int n = wrapText(s, text, rows, ROW_MAX, SCR_W - 6, &ovf);
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      break;
    }

    default: break;
  }

  s.setTextFont(1);
  s.setTextSize(1);
}

// ---------------------------------------------------------------- card
void typeDrawCard(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms, uint32_t seed) {
  s.fillSprite(INK_OFF);
  uint16_t ink = typeInk(seed);

  if (!track || !*track) {
    // Nothing playing at all: a slow caret so the panel never looks dead.
    s.setTextFont(1);
    s.setTextSize(1);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(ink, INK_OFF);
    s.drawString(status ? status : "", SCR_W / 2, SCR_H / 2 - 10);
    if (((ms / 500) & 1) == 0) s.fillRect(SCR_W / 2 - 5, SCR_H / 2 + 6, 10, 3, ink);
    return;
  }


  int textTop = 10;
  Layout T;
  layoutFit(s, track, FACE_SANS, SCR_W - 8, SCR_H - textTop - 3, T);
  applyRung(s, T.rung);
  s.setTextDatum(TC_DATUM);
  s.setTextColor(ink, INK_OFF);
  for (int i = 0; i < T.nRows; i++)
    s.drawString(T.rows[i], SCR_W / 2, textTop + i * T.lineH);

  // The artist only fits when the cover is not taking the top half.
  if (artist && *artist) {
    int ay2 = textTop + T.blockH + 8;
    if (ay2 < SCR_H - 12) {
      s.fillRect(24, ay2 - 5, 80, 2, ink);
      Layout A;
      layoutFit(s, artist, FACE_PIXEL, SCR_W - 10, SCR_H - ay2 - 2, A);
      applyRung(s, A.rung);
      for (int i = 0; i < A.nRows; i++)
        s.drawString(A.rows[i], SCR_W / 2, ay2 + i * A.lineH);
    }
  }

  s.setTextFont(1);
  s.setTextSize(1);
}

// ---------------------------------------------------------------- notes
// Music glyphs drawn from primitives. The Unicode symbols cannot be used --
// see the note in typeview.h -- and drawing them means they can be scaled and
// animated freely instead of being stuck at whatever a font offers.
static void noteHead(TFT_eSprite& s, int x, int y, int sc, uint16_t c) {
  s.fillEllipse(x, y, 2 * sc + 1, (3 * sc) / 2, c);
}

// kind: 0 quarter, 1 eighth, 2 beamed pair, 3 sharp, 4 flat
static void drawNote(TFT_eSprite& s, int kind, int x, int y, int sc, uint16_t c) {
  int stem = 9 * sc;
  switch (kind) {
    case 0:
      noteHead(s, x, y, sc, c);
      s.fillRect(x + 2 * sc, y - stem, sc, stem, c);
      break;
    case 1:
      noteHead(s, x, y, sc, c);
      s.fillRect(x + 2 * sc, y - stem, sc, stem, c);
      for (int i = 0; i < 3 * sc; i++)                     // flag
        s.drawFastVLine(x + 3 * sc + i, y - stem + i, 2 * sc + i / 2, c);
      break;
    case 2: {
      int dx = 8 * sc;
      noteHead(s, x, y, sc, c);
      noteHead(s, x + dx, y + sc, sc, c);
      s.fillRect(x + 2 * sc, y - stem, sc, stem, c);
      s.fillRect(x + dx + 2 * sc, y + sc - stem, sc, stem, c);
      s.fillRect(x + 2 * sc, y - stem, dx + sc, (3 * sc) / 2 + 1, c);   // beam
      break;
    }
    case 3:                                                // sharp
      s.fillRect(x - sc, y - 5 * sc, sc, 11 * sc, c);
      s.fillRect(x + 2 * sc, y - 6 * sc, sc, 11 * sc, c);
      s.fillRect(x - 3 * sc, y - 2 * sc, 8 * sc, sc, c);
      s.fillRect(x - 3 * sc, y + 2 * sc, 8 * sc, sc, c);
      break;
    default:                                               // flat
      s.fillRect(x - sc, y - 8 * sc, sc, 11 * sc, c);
      s.drawEllipse(x + sc, y, 2 * sc, 2 * sc, c);
      s.fillRect(x, y - 2 * sc, sc, 4 * sc, c);
      break;
  }
}

void typeDrawNotes(TFT_eSprite& s, const char* track, const char* artist,
                   float tSec, uint32_t seed) {
  s.fillSprite(INK_OFF);

  // Notes rise and sway, respawning at the bottom, so a long instrumental
  // still reads as "playing" rather than as a dead panel.
  const int N = 7;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(seed ^ (uint32_t)i, 41);
    float ph   = ((h & 1023) / 1024.0f);
    float spd  = 0.055f + ((h >> 10) & 15) * 0.007f;
    float life = fmodf(ph + tSec * spd, 1.0f);
    int   kind = (int)((h >> 14) % 5);
    int   sc   = 1 + (int)((h >> 18) % 2);
    float bx   = 12.0f + ((h >> 20) & 127) * 0.80f;

    float y = 96.0f - life * 96.0f;
    float x = bx + sinf(tSec * 1.1f + i * 1.7f) * 9.0f;

    // Fade in at the bottom and out at the top rather than popping.
    float k = life < 0.15f ? life / 0.15f : (life > 0.8f ? (1.0f - life) / 0.2f : 1.0f);
    int lum = (int)(31 * k);
    if (lum < 3) continue;
    uint16_t c = (uint16_t)((lum << 11) | ((lum << 1) << 5) | lum);
    drawNote(s, kind, (int)x, (int)y, sc, c);
  }

  // Track and artist along the bottom.
  s.setTextDatum(TC_DATUM);
  s.setTextFont(1);
  s.setTextSize(1);
  char buf[48];
  if (track && *track) {
    strncpy(buf, track, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4) buf[strlen(buf) - 1] = 0;
    s.setTextColor(INK_ON);
    s.drawString(buf, SCR_W / 2, SCR_H - 22);
  }
  if (artist && *artist) {
    strncpy(buf, artist, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4) buf[strlen(buf) - 1] = 0;
    s.setTextColor(0x9CF3);
    s.drawString(buf, SCR_W / 2, SCR_H - 11);
  }
}

// ---------------------------------------------------------------- raster
// Measures the real extent of what was drawn, which is the only trustworthy
// guide here.
static void inkBounds(TFT_eSprite& m, int& x0, int& y0, int& x1, int& y1, int& lit) {
  const uint8_t* p = (const uint8_t*)m.getPointer();
  x0 = SCR_W; y0 = SCR_H; x1 = -1; y1 = -1; lit = 0;
  for (int y = 0; y < SCR_H; y++) {
    const uint8_t* row = p + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      if (!row[x]) continue;
      lit++;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
}

// Renders `text` into the 8bpp mask as large as it will go WITHOUT clipping.
//
// GFX font metrics are not a reliable guide: fontHeight() reports the line
// advance, which for several of these faces is smaller than the actual ink
// once descenders are involved. A word can therefore "fit" by the numbers and
// still lose its lower half -- which is exactly what was happening. So render,
// measure the ink that actually landed, recentre on it, and step down a size
// only if it genuinely does not fit.
int typeRasterise(TFT_eSprite& mask, const char* text) {
  mask.fillSprite(0);
  if (!text || !*text) return 0;

  const Face& face = FACE_SANS;
  Layout probe;
  layoutFit(mask, text, face, SCR_W - 8, SCR_H - 16, probe);
  int rung0 = rungIndexOf(face, probe.rung);

  char rows[ROW_MAX][ROW_CHARS];
  int  bestLit = 0;

  for (int attempt = 0; attempt < 5; attempt++) {
    int use = rung0 + attempt;
    if (use > face.n - 1) use = face.n - 1;

    applyRung(mask, face.rungs[use]);
    mask.setTextDatum(MC_DATUM);
    mask.setTextColor(TFT_WHITE);

    int lineH = mask.fontHeight() + 1;
    bool ovf = false;
    int n = wrapText(mask, text, rows, ROW_MAX, SCR_W - 8, &ovf);
    if (n < 1) return 0;
    int y0 = SCR_H / 2 - (n - 1) * lineH / 2;

    mask.fillSprite(0);
    for (int i = 0; i < n; i++)
      mask.drawString(rows[i], SCR_W / 2, y0 + i * lineH);

    int ix0, iy0, ix1, iy1, lit;
    inkBounds(mask, ix0, iy0, ix1, iy1, lit);
    if (lit == 0) continue;
    bestLit = lit;

    int inkW = ix1 - ix0 + 1, inkH = iy1 - iy0 + 1;
    if (inkW <= SCR_W - 4 && inkH <= SCR_H - 4) {
      // Redraw shifted so the measured ink is centred, not the nominal box.
      int dx = (SCR_W / 2) - ((ix0 + ix1) / 2);
      int dy = (SCR_H / 2) - ((iy0 + iy1) / 2);
      mask.fillSprite(0);
      for (int i = 0; i < n; i++)
        mask.drawString(rows[i], SCR_W / 2 + dx, y0 + i * lineH + dy);
      inkBounds(mask, ix0, iy0, ix1, iy1, lit);
      Serial.printf("[raster] rung %d  ink %d,%d..%d,%d  %d lit%s\n",
                    use, ix0, iy0, ix1, iy1, lit,
                    (ix0 < 1 || iy0 < 1 || ix1 > SCR_W - 2 || iy1 > SCR_H - 2)
                      ? "  CLIPPED" : "");
      return lit;
    }
    if (use == face.n - 1) break;      // nothing smaller to fall back to
  }
  return bestLit;
}
