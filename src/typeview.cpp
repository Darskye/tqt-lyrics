#include "typeview.h"
#include <string.h>
#include <stdio.h>

// Room for the worst case: font 1 at size 1 is a 6x8 cell, so 21 characters
// across and 16 rows down. That holds ~330 characters, far longer than any
// lyric line, which is what lets layoutFit() promise never to clip.
#define ROW_MAX   16
#define ROW_CHARS 64

enum Scene {
  SC_HERO, SC_STACK, SC_INVERT, SC_WIPE, SC_SCROLL,
  SC_TYPE, SC_FLASH, SC_RULE, SC_BOX, SC_SPLIT, SC_COUNT
};

static const char* kSceneNames[SC_COUNT] = {
  "hero", "stack", "invert", "wipe", "scroll",
  "type", "flash", "rule", "box", "split"
};

struct Layout {
  int  size;
  int  nRows;
  int  lineH;
  int  blockH;
  char rows[ROW_MAX][ROW_CHARS];
};

static float easeOutCubic(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

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

// Greedy word wrap against the font currently set on the sprite.
// Sets *overflow when text remained after maxRows -- the caller must not
// simply draw what it got, because the rest would be silently lost.
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

// Last fit result, for the serial readout -- proves the auto-fit is actually
// stepping down rather than quietly clipping.
static int  g_lastSize = 0, g_lastRows = 0, g_lastAsked = 0;
static bool g_lastClipped = false;

void typeLastFit(int& size, int& rows, int& asked, bool& clipped) {
  size = g_lastSize; rows = g_lastRows; asked = g_lastAsked; clipped = g_lastClipped;
}

// Largest text size at which the ENTIRE string fits inside maxW x maxH.
// Steps down instead of clipping. Size 1 always "succeeds" as the floor, so a
// line is never dropped even in the pathological case.
static void layoutFit(TFT_eSprite& s, const char* txt, int font,
                      int maxW, int maxH, int maxSize, Layout& L) {
  for (int sz = (maxSize < 1 ? 1 : maxSize); sz >= 1; sz--) {
    s.setTextFont(font);
    s.setTextSize(sz);

    bool ovf = false;
    int n = wrapText(s, txt, L.rows, ROW_MAX, maxW, &ovf);
    int lineH = s.fontHeight() + (sz > 1 ? 2 : 1);

    bool tooWide = false;
    for (int i = 0; i < n; i++)
      if (s.textWidth(L.rows[i]) > maxW) { tooWide = true; break; }

    bool fits = !ovf && n > 0 && !tooWide && (n * lineH) <= maxH;
    if (fits || sz == 1) {
      L.size   = sz;
      L.nRows  = n;
      L.lineH  = lineH;
      L.blockH = n * lineH;
      g_lastSize = sz; g_lastRows = n;
      g_lastAsked = (maxSize < 1 ? 1 : maxSize);
      g_lastClipped = !fits;      // only true if even size 1 could not fit
      return;
    }
  }
}

static void applyLayout(TFT_eSprite& s, int font, const Layout& L) {
  s.setTextFont(font);
  s.setTextSize(L.size);
}

static Scene sceneFromSeed(uint32_t seed) {
  uint32_t h = seed * 2654435761u;
  h ^= h >> 15;
  return (Scene)(h % SC_COUNT);
}

const char* typeSceneName(uint32_t seed) { return kSceneNames[sceneFromSeed(seed)]; }

static Scene chooseScene(const char* text, uint32_t seed) {
  Scene sc = sceneFromSeed(seed);
  int wc = wordCount(text);
  // These two only make sense with something to divide or emphasise.
  if (sc == SC_HERO  && wc < 2) sc = SC_STACK;
  if (sc == SC_SPLIT && wc < 2) sc = SC_BOX;
  // Scrolling a short line looks broken -- it is over before it reads.
  if (sc == SC_SCROLL && strlen(text) < 18) sc = SC_RULE;
  return sc;
}

// ---------------------------------------------------------------- scenes
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed, uint16_t ink) {
  s.fillSprite(INK_OFF);
  if (!text || !*text) return;

  Scene sc = chooseScene(text, seed);
  float in = easeOutCubic((float)ageMs / 300.0f);
  Layout L;

  switch (sc) {
    // One word set as large as it will go, the whole line small beneath it.
    case SC_HERO: {
      char key[ROW_CHARS];
      longestWord(text, key, sizeof(key));

      Layout K;
      layoutFit(s, key, 1, SCR_W - 6, 58, 6, K);
      applyLayout(s, 1, K);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int yc = 50 + (int)((1.0f - in) * 10.0f);
      s.drawString(K.rows[0], SCR_W / 2, yc);

      layoutFit(s, text, 1, SCR_W - 8, 44, 2, L);
      applyLayout(s, 1, L);
      int y0 = 94 - L.blockH / 2 + L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    // Rows stacked left-aligned, each sliding in just after the one above.
    case SC_STACK: {
      layoutFit(s, text, 1, SCR_W - 10, SCR_H - 12, 3, L);
      applyLayout(s, 1, L);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = (SCR_H - L.blockH) / 2;
      for (int i = 0; i < L.nRows; i++) {
        float li = easeOutCubic(((float)ageMs - i * 55.0f) / 300.0f);
        int x = 6 - (int)((1.0f - li) * 60.0f);
        s.drawString(L.rows[i], x, y0 + i * L.lineH);
      }
      break;
    }

    // Knocked out of a filled field -- the strongest contrast move available.
    case SC_INVERT: {
      s.fillSprite(ink);
      layoutFit(s, text, 1, SCR_W - 14, SCR_H - 16, 3, L);
      applyLayout(s, 1, L);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(INK_OFF, ink);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    // Type is there from frame one; a shutter retreats off it.
    case SC_WIPE: {
      layoutFit(s, text, 1, SCR_W - 14, SCR_H - 16, 3, L);
      applyLayout(s, 1, L);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(ink, INK_OFF);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      int cut = (int)(in * SCR_W);
      if (cut < SCR_W) {
        s.fillRect(cut, 0, SCR_W - cut, SCR_H, INK_OFF);
        s.fillRect(cut, 0, 2, SCR_H, ink);            // leading edge
      }
      break;
    }

    // One big row travelling across, between two rules.
    case SC_SCROLL: {
      s.setTextFont(1);
      s.setTextSize(3);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);
      int w = s.textWidth(text);
      uint32_t span = holdMs ? holdMs : 3000;
      float t = span ? (float)ageMs / (float)span : 0.0f;
      if (t > 1.0f) t = 1.0f;
      int x = SCR_W - (int)(t * (w + SCR_W));
      int y = (SCR_H - s.fontHeight()) / 2;
      s.drawString(text, x, y);
      s.fillRect(0, y - 10, SCR_W, 2, ink);
      s.fillRect(0, y + s.fontHeight() + 8, SCR_W, 2, ink);
      break;
    }

    // Typewriter with a block cursor. The layout is measured on the FULL line
    // so the block does not jump around as characters arrive.
    case SC_TYPE: {
      layoutFit(s, text, 1, SCR_W - 10, SCR_H - 12, 3, L);
      int size = L.size;

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

      s.setTextFont(1);
      s.setTextSize(size);
      s.setTextDatum(TL_DATUM);
      s.setTextColor(ink, INK_OFF);

      Layout P;
      bool ovf = false;
      P.nRows = wrapText(s, partial, P.rows, ROW_MAX, SCR_W - 10, &ovf);
      P.lineH = L.lineH;

      int y0 = (SCR_H - L.blockH) / 2;
      for (int i = 0; i < P.nRows; i++)
        s.drawString(P.rows[i], 5, y0 + i * P.lineH);

      if (P.nRows > 0 && ((ageMs / 300) & 1) == 0) {
        int cw = s.textWidth(P.rows[P.nRows - 1]);
        s.fillRect(5 + cw + 2, y0 + (P.nRows - 1) * P.lineH,
                   3 * size, s.fontHeight(), ink);
      }
      break;
    }

    // Slams in inverted, then settles. Reads as an accent.
    case SC_FLASH: {
      bool flip = ageMs < 90 || (ageMs > 140 && ageMs < 200);
      uint16_t fg = flip ? INK_OFF : ink;
      uint16_t bg = flip ? ink : INK_OFF;
      if (flip) s.fillSprite(ink);
      layoutFit(s, text, 1, SCR_W - 14, SCR_H - 16, 3, L);
      applyLayout(s, 1, L);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
      for (int i = 0; i < L.nRows; i++)
        s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      break;
    }

    // Heavy rules driving in from opposite edges, type held between them.
    case SC_RULE: {
      layoutFit(s, text, 1, SCR_W - 16, SCR_H - 40, 3, L);
      applyLayout(s, 1, L);
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

    // A card grows from the centre; type is knocked out of it.
    case SC_BOX: {
      layoutFit(s, text, 1, SCR_W - 24, SCR_H - 32, 3, L);
      int boxH = L.blockH + 16;
      int h = (int)(in * boxH);
      int y = SCR_H / 2 - h / 2;
      s.fillRect(0, y, SCR_W, h, ink);
      if (h >= boxH - 1) {
        applyLayout(s, 1, L);
        s.setTextDatum(MC_DATUM);
        s.setTextColor(INK_OFF, ink);
        int y0 = SCR_H / 2 - (L.nRows - 1) * L.lineH / 2;
        for (int i = 0; i < L.nRows; i++)
          s.drawString(L.rows[i], SCR_W / 2, y0 + i * L.lineH);
      }
      break;
    }

    // Line broken in two, halves arriving from opposite sides. Each half is
    // fitted independently so a lopsided split still fits.
    case SC_SPLIT: {
      int wc = wordCount(text);
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
      layoutFit(s, a, 1, SCR_W - 8, 54, 4, A);
      applyLayout(s, 1, A);
      int ay = 34 - (A.nRows - 1) * A.lineH / 2;
      for (int i = 0; i < A.nRows; i++)
        s.drawString(A.rows[i], SCR_W / 2 - offs, ay + i * A.lineH);

      layoutFit(s, b, 1, SCR_W - 8, 54, 4, B);
      applyLayout(s, 1, B);
      int by = 92 - (B.nRows - 1) * B.lineH / 2;
      for (int i = 0; i < B.nRows; i++)
        s.drawString(B.rows[i], SCR_W / 2 + offs, by + i * B.lineH);
      break;
    }

    default: break;
  }

  s.setTextSize(1);
}

// ---------------------------------------------------------------- idle
void typeDrawIdle(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms, uint16_t ink) {
  s.fillSprite(INK_OFF);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(ink, INK_OFF);

  if (track && *track) {
    Layout T;
    layoutFit(s, track, 1, SCR_W - 10, 60, 3, T);
    applyLayout(s, 1, T);
    int y0 = 50 - (T.nRows - 1) * T.lineH / 2;
    for (int i = 0; i < T.nRows; i++)
      s.drawString(T.rows[i], SCR_W / 2, y0 + i * T.lineH);

    int ruleY = 50 + T.blockH / 2 + 8;
    s.fillRect(24, ruleY, 80, 2, ink);

    Layout A;
    layoutFit(s, artist, 1, SCR_W - 10, 34, 1, A);
    applyLayout(s, 1, A);
    int ay = ruleY + 10;
    for (int i = 0; i < A.nRows; i++)
      s.drawString(A.rows[i], SCR_W / 2, ay + i * A.lineH);
  } else {
    s.setTextFont(1);
    s.setTextSize(1);
    s.drawString(status ? status : "", SCR_W / 2, SCR_H / 2 - 10);
    if (((ms / 500) & 1) == 0) s.fillRect(SCR_W / 2 - 5, SCR_H / 2 + 6, 10, 3, ink);
  }
  s.setTextSize(1);
}
