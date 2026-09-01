#include "viz.h"
#include "typeview.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- fast trig
// A libm sinf() per particle per frame would dominate the budget; a masked
// table wraps for free and needs no range reduction.
#define SIN_N    512
#define SIN_MASK (SIN_N - 1)
#define SIN_SCALE (SIN_N / 6.2831853f)

static float sinTab[SIN_N];
static bool  tabReady = false;

static void trigInit() {
  if (tabReady) return;
  for (int i = 0; i < SIN_N; i++) sinTab[i] = sinf(i * 6.2831853f / SIN_N);
  tabReady = true;
}
static inline float fsin(float a) { return sinTab[((int)(a * SIN_SCALE)) & SIN_MASK]; }
static inline float fcos(float a) { return sinTab[(((int)(a * SIN_SCALE)) + SIN_N / 4) & SIN_MASK]; }

// ---------------------------------------------------------------- helpers
#define RGB(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static uint32_t mix(uint32_t seed, uint32_t salt) {
  uint32_t h = (seed + salt * 0x9E3779B9u) * 2654435761u;
  h ^= h >> 15;
  return h;
}

// Direct buffer write: drawPixel per particle costs more in bounds-checking
// than the plot itself at these counts.
static inline void plot(uint16_t* fb, int x, int y, uint16_t c) {
  if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) fb[y * SCR_W + x] = c;
}

// A 2x2 dot reads as a particle rather than dust on a 128px panel; the single
// pixel is kept for the densest fields.
static inline void plot2(uint16_t* fb, int x, int y, uint16_t c) {
  plot(fb, x, y, c);  plot(fb, x + 1, y, c);
  plot(fb, x, y + 1, c);  plot(fb, x + 1, y + 1, c);
}

// Greyscale ramp: these are monochrome fields, and brightness is what gives
// them depth. v is 0..255.
static inline uint16_t shade(int v) {
  if (v < 0) v = 0; else if (v > 255) v = 255;
  return RGB(v, v, v);
}

static const char* kVizNames[VIZ_COUNT] = {
  "spiral", "starfield", "rain", "orbits", "wave", "bloom"
};

const char* vizName(uint32_t seed) { return kVizNames[mix(seed, 11) % VIZ_COUNT]; }

// ---------------------------------------------------------------- visualisers

// Phyllotaxis spiral: points placed at the golden angle with radius ~ sqrt(i)
// spread evenly, and rotating the whole field makes the arms appear to swirl
// inward. This is the classic "spiraly" pattern.
static void vizSpiral(uint16_t* fb, float t, uint32_t seed) {
  const int N = 260;
  const float golden = 2.39996323f;
  float spin = t * (0.45f + (mix(seed, 1) % 40) * 0.01f);
  float pulse = 1.0f + 0.10f * fsin(t * 1.3f);

  for (int i = 0; i < N; i++) {
    float a = i * golden + spin;
    float r = 3.6f * sqrtf((float)i) * pulse;
    if (r > 66.0f) continue;
    int x = (int)(64.0f + r * fcos(a));
    int y = (int)(64.0f + r * fsin(a));
    int v = 255 - (int)(r * 2.4f);            // fade toward the rim
    if (i % 7 == 0) v = 255;                  // sparkle along one arm
    plot2(fb, x, y, shade(v));
  }
}

// Particles streaming outward, respawning at the centre. Depth comes from
// speed and brightness rising together as they approach the edge.
static void vizStarfield(uint16_t* fb, float t, uint32_t seed) {
  const int N = 200;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(seed ^ i, 2);
    float ang = (h & 1023) * 0.006135f;             // 0..2pi
    float spd = 14.0f + (h >> 10 & 31) * 1.6f;
    float ph  = (h >> 15 & 255) / 255.0f;
    float z   = fmodf(ph + t * spd * 0.012f, 1.0f);
    float r   = z * z * 78.0f;                       // accelerate outward
    int x = (int)(64.0f + r * fcos(ang));
    int y = (int)(64.0f + r * fsin(ang));
    int v = (int)(40 + z * 215);
    if (z > 0.55f) plot2(fb, x, y, shade(v));
    else           plot(fb, x, y, shade(v));
  }
}

// Columns of falling pixels at independent speeds, brightest at the head.
static void vizRain(uint16_t* fb, float t, uint32_t seed) {
  for (int col = 0; col < 32; col++) {
    uint32_t h = mix(seed ^ col, 3);
    float spd = 26.0f + (h & 63) * 1.4f;
    float off = (h >> 6 & 255) / 255.0f * 128.0f;
    int len = 6 + (h >> 14 & 7);
    float head = fmodf(off + t * spd, 160.0f) - 16.0f;
    int x = col * 4 + 1;
    for (int k = 0; k < len; k++) {
      int y = (int)head - k * 3;
      int v = 255 - k * (200 / len);
      plot(fb, x, y, shade(v));
      plot(fb, x + 1, y, shade(v));
    }
  }
}

// Concentric rings of points, each ring turning at its own rate, so the field
// shears into moving spiral arms without any of the points actually spiralling.
static void vizOrbits(uint16_t* fb, float t, uint32_t seed) {
  int rings = 7 + (int)(mix(seed, 4) % 3);
  for (int ring = 1; ring <= rings; ring++) {
    float rr = ring * (62.0f / rings);
    int n = 6 + ring * 4;
    float rate = (ring & 1 ? 1.0f : -1.0f) * (0.9f - ring * 0.06f);
    for (int i = 0; i < n; i++) {
      float a = (6.2831853f * i) / n + t * rate;
      int x = (int)(64.0f + rr * fcos(a));
      int y = (int)(64.0f + rr * fsin(a));
      plot2(fb, x, y, shade(90 + ring * 22));
    }
  }
}

// A lattice of dots displaced by two crossing travelling waves -- the grid
// stays legible while the surface appears to breathe.
static void vizWave(uint16_t* fb, float t, uint32_t seed) {
  float k1 = 0.055f + (mix(seed, 5) % 20) * 0.002f;
  for (int gy = 0; gy < 16; gy++) {
    for (int gx = 0; gx < 16; gx++) {
      float bx = gx * 8.0f + 4.0f, by = gy * 8.0f + 4.0f;
      float d = fsin(bx * k1 + t * 1.6f) + fcos(by * 0.07f - t * 1.1f);
      int x = (int)(bx + d * 3.4f);
      int y = (int)(by + d * 3.4f);
      int v = (int)(150 + d * 52);
      plot2(fb, x, y, shade(v));
    }
  }
}

// Rings expanding out of the centre and fading, staggered so there is always
// one arriving as another dissolves.
static void vizBloom(uint16_t* fb, float t, uint32_t seed) {
  const int RINGS = 5;
  for (int b = 0; b < RINGS; b++) {
    float ph = fmodf(t * 0.42f + b / (float)RINGS, 1.0f);
    float r = ph * 74.0f;
    int v = (int)(255 * (1.0f - ph));
    if (v < 12) continue;
    int n = 18 + (int)(r * 1.5f);
    float spin = t * 0.3f + b * 1.1f + (mix(seed, 6) % 100) * 0.01f;
    for (int i = 0; i < n; i++) {
      float a = (6.2831853f * i) / n + spin;
      plot(fb, (int)(64.0f + r * fcos(a)), (int)(64.0f + r * fsin(a)), shade(v));
    }
  }
}

// ---------------------------------------------------------------- frame
void vizDraw(TFT_eSprite& s, uint32_t seed, float tSec, float progress,
             const char* track, const char* artist) {
  trigInit();
  s.fillSprite(INK_OFF);
  uint16_t* fb = (uint16_t*)s.getPointer();

  switch (mix(seed, 11) % VIZ_COUNT) {
    case 0: vizSpiral(fb, tSec, seed);    break;
    case 1: vizStarfield(fb, tSec, seed); break;
    case 2: vizRain(fb, tSec, seed);      break;
    case 3: vizOrbits(fb, tSec, seed);    break;
    case 4: vizWave(fb, tSec, seed);      break;
    default: vizBloom(fb, tSec, seed);    break;
  }

  // --- bottom band: darken so the type reads over whatever is moving under it
  const int bandH = 30;
  const int bandY = SCR_H - bandH;
  for (int y = bandY; y < SCR_H; y++) {
    uint16_t* row = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      uint16_t c = row[x];
      row[x] = (uint16_t)(((((c >> 11) & 0x1F) >> 2) << 11) |
                          ((((c >> 5)  & 0x3F) >> 2) <<  5) |
                          (((c & 0x1F) >> 2)));
    }
  }

  // --- track and artist
  s.setTextDatum(TC_DATUM);
  s.setTextColor(INK_ON);
  s.setTextFont(1);
  s.setTextSize(1);

  char buf[40];
  if (track && *track) {
    strncpy(buf, track, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4)
      buf[strlen(buf) - 1] = 0;                    // trim, do not overflow
    s.drawString(buf, SCR_W / 2, bandY + 4);
  }
  if (artist && *artist) {
    strncpy(buf, artist, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4)
      buf[strlen(buf) - 1] = 0;
    s.setTextColor(shade(150));
    s.drawString(buf, SCR_W / 2, bandY + 14);
  }

  // --- seek bar: this one is genuinely accurate, straight from progress_ms
  const int barY = SCR_H - 5, barX = 6, barW = SCR_W - 12;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  s.drawFastHLine(barX, barY, barW, shade(70));
  s.drawFastHLine(barX, barY, (int)(barW * progress), INK_ON);
  s.fillRect(barX + (int)(barW * progress) - 1, barY - 2, 3, 5, INK_ON);

  s.setTextFont(1);
  s.setTextSize(1);
}
