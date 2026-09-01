#include "typeview.h"
#include "albumart.h"
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
  SC_TYPE, SC_FLASH, SC_RULE, SC_BOX, SC_SPLIT, SC_COUNT
};

static const char* kSceneNames[SC_COUNT] = {
  "hero", "stack", "invert", "wipe", "scroll",
  "type", "flash", "rule", "box", "split"
};

struct Layout {
  Rung rung;
  int  nRows, lineH, blockH;
  char rows[ROW_MAX][ROW_CHARS];
};

static int  g_lastRows = 0;
static bool g_lastClipped = false;
void typeLastFit(int& rows, bool& clipped) { rows = g_lastRows; clipped = g_lastClipped; }

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

uint16_t    typeInk(uint32_t seed)       { return kInks[mix(seed, 3) % kInkCount]; }
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

// ---------------------------------------------------------------- draw
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed) {
  s.fillSprite(INK_OFF);
  if (!text || !*text) return;

  Scene sc = sceneFromSeed(seed);
  const Face& face = faceFromSeed(seed);
  uint16_t ink = typeInk(seed);

  int wc = wordCount(text);
  if (sc == SC_HERO  && wc < 2) sc = SC_STACK;
  if (sc == SC_SPLIT && wc < 2) sc = SC_BOX;
  if (sc == SC_SCROLL && strlen(text) < 18) sc = SC_RULE;

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

    default: break;
  }

  s.setTextFont(1);
  s.setTextSize(1);
}

// ---------------------------------------------------------------- card
void typeDrawCard(TFT_eSprite& s, const uint16_t* art, bool haveArt,
                  const char* track, const char* artist, const char* status,
                  uint32_t ms, uint32_t seed) {
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

  if (haveArt && art) {
    // Cover is exactly half the panel width, sat above the title.
    const int ax = (SCR_W - ART_W) / 2, ay = 6;
    s.pushImage(ax, ay, ART_W, ART_H, art);
    s.drawRect(ax - 1, ay - 1, ART_W + 2, ART_H + 2, ink);
    textTop = ay + ART_H + 7;
  }

  Layout T;
  layoutFit(s, track, FACE_SANS, SCR_W - 8, SCR_H - textTop - 3, T);
  applyRung(s, T.rung);
  s.setTextDatum(TC_DATUM);
  s.setTextColor(ink, INK_OFF);
  for (int i = 0; i < T.nRows; i++)
    s.drawString(T.rows[i], SCR_W / 2, textTop + i * T.lineH);

  // The artist only fits when the cover is not taking the top half.
  if (!haveArt && artist && *artist) {
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
