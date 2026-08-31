#include "typeview.h"
#include <string.h>
#include <stdio.h>

#define ROW_MAX   8
#define ROW_CHARS 48

enum Scene {
  SC_HERO, SC_STACK, SC_INVERT, SC_WIPE, SC_SCROLL,
  SC_TYPE, SC_FLASH, SC_RULE, SC_BOX, SC_SPLIT, SC_COUNT
};

static const char* kSceneNames[SC_COUNT] = {
  "hero", "stack", "invert", "wipe", "scroll",
  "type", "flash", "rule", "box", "split"
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

// The longest word carries the line visually; HERO builds the frame around it.
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

// Largest integer text size that still fits the box. Font 1 is a 6x8 cell, so
// scaling it stays crisp at any multiple -- which is what makes big blocky type
// look deliberate here rather than stretched.
static int fitSize(TFT_eSprite& s, const char* txt, int font,
                   int maxW, int maxH, int hi) {
  s.setTextFont(font);
  for (int sz = hi; sz >= 1; sz--) {
    s.setTextSize(sz);
    if (s.textWidth(txt) <= maxW && s.fontHeight() <= maxH) return sz;
  }
  s.setTextSize(1);
  return 1;
}

static int wrapText(TFT_eSprite& s, const char* txt,
                    char rows[][ROW_CHARS], int maxRows, int maxW) {
  int n = 0;
  const char* p = txt;
  char cur[ROW_CHARS];
  cur[0] = 0;

  while (*p && n < maxRows) {
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
      strncpy(rows[n], cur, ROW_CHARS - 1); rows[n][ROW_CHARS - 1] = 0; n++;
      strncpy(cur, word, ROW_CHARS - 1);    cur[ROW_CHARS - 1] = 0;
    }
  }
  if (cur[0] && n < maxRows) {
    strncpy(rows[n], cur, ROW_CHARS - 1); rows[n][ROW_CHARS - 1] = 0; n++;
  }
  return n;
}

static Scene sceneFromSeed(uint32_t seed) {
  uint32_t h = seed * 2654435761u;
  h ^= h >> 15;
  return (Scene)(h % SC_COUNT);
}

const char* typeSceneName(uint32_t seed) { return kSceneNames[sceneFromSeed(seed)]; }

// ---------------------------------------------------------------- scenes
void typeDraw(TFT_eSprite& s, const char* text,
              uint32_t ageMs, uint32_t holdMs, uint32_t seed) {
  s.fillSprite(INK_OFF);
  if (!text || !*text) return;

  Scene sc = sceneFromSeed(seed);
  int len = (int)strlen(text);
  if (sc == SC_HERO  && wordCount(text) < 2) sc = SC_STACK;
  if (sc == SC_SCROLL && len < 18)           sc = SC_RULE;
  if (sc == SC_SPLIT  && wordCount(text) < 2) sc = SC_BOX;

  float in = easeOutCubic((float)ageMs / 300.0f);
  char rows[ROW_MAX][ROW_CHARS];
  uint16_t fg = INK_ON, bg = INK_OFF;

  switch (sc) {
    // One word enormous, the rest of the line small beneath it.
    case SC_HERO: {
      char key[ROW_CHARS];
      longestWord(text, key, sizeof(key));
      int sz = fitSize(s, key, 1, SCR_W - 6, 56, 6);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(sz);
      int yc = 54 + (int)((1.0f - in) * 10.0f);
      s.drawString(key, SCR_W / 2, yc);

      s.setTextSize(1);
      s.setTextFont(2);
      int n = wrapText(s, text, rows, 3, SCR_W - 8);
      int lh = s.fontHeight() + 1;
      for (int i = 0; i < n; i++)
        s.drawString(rows[i], SCR_W / 2, 92 + i * lh);
      break;
    }

    // Words stacked and left-aligned, each sliding in slightly after the last.
    case SC_STACK: {
      s.setTextDatum(TL_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 5, SCR_W - 8);
      int lh = s.fontHeight() + 4;
      int y0 = (SCR_H - n * lh) / 2;
      for (int i = 0; i < n; i++) {
        float li = easeOutCubic(((float)ageMs - i * 55.0f) / 300.0f);
        int x = 6 - (int)((1.0f - li) * 60.0f);
        s.drawString(rows[i], x, y0 + i * lh);
      }
      break;
    }

    // Black on white. The strongest contrast move available in monochrome.
    case SC_INVERT: {
      s.fillSprite(INK_ON);
      fg = INK_OFF; bg = INK_ON;
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 5, SCR_W - 10);
      int lh = s.fontHeight() + 3;
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      break;
    }

    // Text is there from the first frame; a black shutter retreats off it.
    case SC_WIPE: {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 5, SCR_W - 10);
      int lh = s.fontHeight() + 3;
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      int cut = (int)(in * SCR_W);
      if (cut < SCR_W) {
        s.fillRect(cut, 0, SCR_W - cut, SCR_H, INK_OFF);
        s.fillRect(cut, 0, 2, SCR_H, INK_ON);      // leading edge
      }
      break;
    }

    // Long lines get one big row that tracks across, ticker style.
    case SC_SCROLL: {
      s.setTextDatum(TL_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(3);
      int w = s.textWidth(text);
      uint32_t span = holdMs ? holdMs : 3000;
      float t = span ? (float)ageMs / (float)span : 0.0f;
      if (t > 1.0f) t = 1.0f;
      int travel = w + SCR_W;
      int x = SCR_W - (int)(t * travel);
      int y = (SCR_H - s.fontHeight()) / 2;
      s.drawString(text, x, y);
      s.fillRect(0, y - 10, SCR_W, 2, INK_ON);
      s.fillRect(0, y + s.fontHeight() + 8, SCR_W, 2, INK_ON);
      break;
    }

    // Typewriter with a block cursor, monospaced by construction (font 1).
    case SC_TYPE: {
      s.setTextDatum(TL_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      uint32_t span = holdMs ? (holdMs * 55 / 100) : 900;
      if (span < 150) span = 150;
      int total = len;
      int reveal = (int)((uint64_t)ageMs * (uint64_t)total / (uint64_t)span);
      if (reveal > total) reveal = total;

      char partial[ROW_CHARS * 2];
      int take = reveal;
      if (take > (int)sizeof(partial) - 1) take = (int)sizeof(partial) - 1;
      memcpy(partial, text, take);
      partial[take] = 0;

      int n = wrapText(s, partial, rows, 5, SCR_W - 8);
      int lh = s.fontHeight() + 3;
      int y0 = (SCR_H - n * lh) / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], 5, y0 + i * lh);

      if (n > 0 && ((ageMs / 300) & 1) == 0) {
        int cw = s.textWidth(rows[n - 1]);
        s.fillRect(5 + cw + 2, y0 + (n - 1) * lh, 10, s.fontHeight(), INK_ON);
      }
      break;
    }

    // Slams in inverted, then settles. Reads as an accent on the beat.
    case SC_FLASH: {
      bool flip = ageMs < 90 || (ageMs > 140 && ageMs < 200);
      if (flip) { s.fillSprite(INK_ON); fg = INK_OFF; bg = INK_ON; }
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 5, SCR_W - 10);
      int lh = s.fontHeight() + 3;
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      break;
    }

    // Heavy rules driving in from both edges, type held between them.
    case SC_RULE: {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 4, SCR_W - 12);
      int lh = s.fontHeight() + 3;
      int blockH = n * lh;
      int y0 = SCR_H / 2 - (n - 1) * lh / 2;
      int barW = (int)(in * SCR_W);
      s.fillRect(0, SCR_H / 2 - blockH / 2 - 12, barW, 4, INK_ON);
      s.fillRect(SCR_W - barW, SCR_H / 2 + blockH / 2 + 8, barW, 4, INK_ON);
      for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      break;
    }

    // A white card grows from the centre; type is knocked out of it.
    case SC_BOX: {
      s.setTextFont(1);
      s.setTextSize(2);
      int n = wrapText(s, text, rows, 4, SCR_W - 20);
      int lh = s.fontHeight() + 3;
      int blockH = n * lh;
      int boxH = blockH + 16;
      int h = (int)(in * boxH);
      int y = SCR_H / 2 - h / 2;
      s.fillRect(0, y, SCR_W, h, INK_ON);
      if (h >= boxH - 1) {
        s.setTextDatum(MC_DATUM);
        s.setTextColor(INK_OFF, INK_ON);
        int y0 = SCR_H / 2 - (n - 1) * lh / 2;
        for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);
      }
      break;
    }

    // Line broken in two, halves arriving from opposite sides.
    case SC_SPLIT: {
      int wc = wordCount(text);
      int half = wc / 2;
      char a[ROW_CHARS * 2] = {0}, b[ROW_CHARS * 2] = {0};
      char w[ROW_CHARS];
      for (int i = 0; i < wc; i++) {
        nthWord(text, i, w, sizeof(w));
        char* dst = (i < half) ? a : b;
        if (*dst) strncat(dst, " ", 2);
        strncat(dst, w, ROW_CHARS - 1);
      }
      s.setTextDatum(MC_DATUM);
      s.setTextColor(fg, bg);
      int szA = fitSize(s, a, 1, SCR_W - 8, 34, 4);
      int offs = (int)((1.0f - in) * 130.0f);
      s.drawString(a, SCR_W / 2 - offs, 46);
      int szB = fitSize(s, b, 1, SCR_W - 8, 34, 4);
      (void)szA; (void)szB;
      s.drawString(b, SCR_W / 2 + offs, 84);
      break;
    }

    default: break;
  }

  s.setTextSize(1);
}

// ---------------------------------------------------------------- idle
void typeDrawIdle(TFT_eSprite& s, const char* track, const char* artist,
                  const char* status, uint32_t ms) {
  s.fillSprite(INK_OFF);
  char rows[ROW_MAX][ROW_CHARS];

  s.setTextDatum(MC_DATUM);
  s.setTextColor(INK_ON, INK_OFF);

  if (track && *track) {
    s.setTextFont(1);
    s.setTextSize(2);
    int n = wrapText(s, track, rows, 4, SCR_W - 8);
    int lh = s.fontHeight() + 3;
    int y0 = 52 - (n - 1) * lh / 2;
    for (int i = 0; i < n; i++) s.drawString(rows[i], SCR_W / 2, y0 + i * lh);

    s.fillRect(24, 84, 80, 2, INK_ON);

    s.setTextFont(1);
    s.setTextSize(1);
    int na = wrapText(s, artist, rows, 2, SCR_W - 8);
    for (int i = 0; i < na; i++) s.drawString(rows[i], SCR_W / 2, 96 + i * 10);
  } else {
    // No track: a slow blinking caret so the panel never looks dead.
    s.setTextFont(1);
    s.setTextSize(1);
    s.drawString(status ? status : "", SCR_W / 2, SCR_H / 2 - 10);
    if (((ms / 500) & 1) == 0) s.fillRect(SCR_W / 2 - 5, SCR_H / 2 + 6, 10, 3, INK_ON);
  }
  s.setTextSize(1);
}
