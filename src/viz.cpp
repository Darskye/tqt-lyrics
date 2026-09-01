#include "viz.h"
#include "typeview.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- fast trig
// A libm sinf() per particle per frame would dominate the budget; a masked
// table wraps for free and needs no range reduction.
#define SIN_N    1024
#define SIN_MASK (SIN_N - 1)
#define SIN_SCALE (SIN_N / 6.2831853f)
#define TAU 6.2831853f

static float sinTab[SIN_N];
static bool  tabReady = false;

static void trigInit() {
  if (tabReady) return;
  for (int i = 0; i < SIN_N; i++) sinTab[i] = sinf(i * TAU / SIN_N);
  tabReady = true;
}
static inline float fsin(float a) { return sinTab[((int)(a * SIN_SCALE)) & SIN_MASK]; }
static inline float fcos(float a) { return sinTab[(((int)(a * SIN_SCALE)) + SIN_N / 4) & SIN_MASK]; }

static inline uint32_t mix(uint32_t seed, uint32_t salt) {
  uint32_t h = (seed + salt * 0x9E3779B9u) * 2654435761u;
  h ^= h >> 15;
  return h;
}

// ---------------------------------------------------------------- plotting
// Everything is white. Brightness is not colour here -- it is coverage and
// depth, which is what keeps a dense field readable rather than a solid blob.
// Values accumulate and saturate, so overlapping particles build up.
static inline void addPix(uint16_t* fb, int x, int y, int v) {
  if ((unsigned)x >= (unsigned)SCR_W || (unsigned)y >= (unsigned)SCR_H) return;
  if (v <= 0) return;
  uint16_t c = fb[y * SCR_W + x];
  int lum = ((c >> 11) & 0x1F) + v;
  if (lum > 31) lum = 31;
  fb[y * SCR_W + x] = (uint16_t)((lum << 11) | ((lum << 1) << 5) | lum);
}

// Bilinear splat at fractional coordinates. This is the single biggest quality
// win over integer plotting: particles glide instead of stepping, and slow
// motion stops looking like a stutter.
static inline void splat(uint16_t* fb, float fx, float fy, int amp) {
  if (fx < -1.0f || fy < -1.0f || fx > (float)SCR_W || fy > (float)SCR_H) return;
  int xi = (int)(fx + 256.0f) - 256;          // floor, without floorf()
  int yi = (int)(fy + 256.0f) - 256;
  int ax = (int)((fx - xi) * 32.0f), ay = (int)((fy - yi) * 32.0f);
  int iax = 32 - ax, iay = 32 - ay;
  addPix(fb, xi,     yi,     (amp * iax * iay) >> 10);
  addPix(fb, xi + 1, yi,     (amp * ax  * iay) >> 10);
  addPix(fb, xi,     yi + 1, (amp * iax * ay ) >> 10);
  addPix(fb, xi + 1, yi + 1, (amp * ax  * ay ) >> 10);
}

static const char* kNames[VIZ_COUNT] = {
  "spiral", "vortex", "starfield", "tunnel", "rain",   "orbits",
  "lattice", "ripple", "lissajous", "helix",  "swarm",  "bloom"
};

const char* vizNameAt(int i) { return kNames[((i % VIZ_COUNT) + VIZ_COUNT) % VIZ_COUNT]; }
int vizIndexForSeed(uint32_t seed) { return (int)(mix(seed, 11) % VIZ_COUNT); }

// ---------------------------------------------------------------- fields

// Phyllotaxis: points at the golden angle with radius ~ sqrt(i) spread evenly,
// and rotating the whole field makes the arms appear to swirl inward.
static void fSpiral(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1100;
  const float golden = 2.39996323f;
  float spin  = t * (0.35f + (mix(sd, 1) % 30) * 0.012f);
  float pulse = 1.0f + 0.09f * fsin(t * 1.1f);
  for (int i = 0; i < N; i++) {
    float a = i * golden + spin;
    float r = 2.05f * sqrtf((float)i) * pulse;
    if (r > 70.0f) break;
    splat(fb, 64.0f + r * fcos(a), 64.0f + r * fsin(a), (i % 9 == 0) ? 31 : 20);
  }
}

// Particles falling inward while turning faster as they close on the centre,
// respawning at the rim. Reads as a drain rather than a static spiral.
static void fVortex(uint16_t* fb, float t, uint32_t sd) {
  const int N = 900;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 2);
    float ph  = (h & 1023) / 1024.0f;
    float ang = ((h >> 10) & 1023) * (TAU / 1024.0f);
    float z   = fmodf(ph + t * 0.11f, 1.0f);
    float r   = (1.0f - z) * 68.0f;
    float a   = ang + t * (0.7f + 2.4f / (r * 0.06f + 1.0f));
    splat(fb, 64.0f + r * fcos(a), 64.0f + r * fsin(a), 8 + (int)(z * 23));
  }
}

// Perspective stars: constant motion in z, projected, so they accelerate
// outward and brighten as they approach.
static void fStarfield(uint16_t* fb, float t, uint32_t sd) {
  const int N = 850;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 3);
    float ang = (h & 2047) * (TAU / 2048.0f);
    float rad = 4.0f + ((h >> 11) & 255) * 0.28f;
    float ph  = ((h >> 19) & 511) / 512.0f;
    float z   = 1.0f - fmodf(ph + t * 0.22f, 1.0f);
    if (z < 0.03f) continue;
    float k = 0.9f / z;
    float r = rad * k;
    if (r > 92.0f) continue;
    splat(fb, 64.0f + r * fcos(ang), 64.0f + r * fsin(ang), 4 + (int)((1.0f - z) * 27));
  }
}

// Square rings receding down a corridor, each rotating a little, so the walls
// appear to twist as they come at you.
static void fTunnel(uint16_t* fb, float t, uint32_t sd) {
  const int RINGS = 16, PER = 56;
  float twist = (mix(sd, 4) % 20) * 0.01f;
  for (int ring = 0; ring < RINGS; ring++) {
    float z = fmodf(ring / (float)RINGS + t * 0.16f, 1.0f);
    if (z < 0.04f) continue;
    float k = 1.0f / z;
    float rr = 7.0f * k;
    if (rr > 100.0f) continue;
    float spin = t * 0.5f + z * twist * 20.0f;
    int amp = 3 + (int)((1.0f - z) * 28);
    for (int i = 0; i < PER; i++) {
      float a = (TAU * i) / PER + spin;
      splat(fb, 64.0f + rr * fcos(a), 64.0f + rr * fsin(a), amp);
    }
  }
}

// Columns of falling pixels at independent speeds, brightest at the head and
// tapering behind, at sub-pixel positions so they slide rather than jump.
static void fRain(uint16_t* fb, float t, uint32_t sd) {
  const int COLS = 42;
  for (int col = 0; col < COLS; col++) {
    uint32_t h = mix(sd ^ col, 5);
    float spd = 26.0f + (h & 63) * 1.7f;
    float off = ((h >> 6) & 511) / 512.0f * 180.0f;
    int len = 10 + ((h >> 15) & 11);
    float head = fmodf(off + t * spd, 190.0f) - 30.0f;
    float x = col * (128.0f / COLS) + 1.0f;
    for (int k = 0; k < len; k++) {
      float y = head - k * 2.4f;
      splat(fb, x, y, 31 - (k * 28) / len);
    }
  }
}

// Concentric rings each turning at its own rate, so the field shears into
// moving arms without any point actually travelling.
static void fOrbits(uint16_t* fb, float t, uint32_t sd) {
  int rings = 13 + (int)(mix(sd, 6) % 4);
  for (int ring = 1; ring <= rings; ring++) {
    float rr = ring * (68.0f / rings);
    int n = 10 + ring * 5;
    float rate = (ring & 1 ? 1.0f : -1.0f) * (0.85f - ring * 0.035f);
    int amp = 10 + ring;
    for (int i = 0; i < n; i++) {
      float a = (TAU * i) / n + t * rate;
      splat(fb, 64.0f + rr * fcos(a), 64.0f + rr * fsin(a), amp);
    }
  }
}

// A dense lattice displaced by crossing travelling waves; the grid stays
// legible while the surface breathes.
static void fLattice(uint16_t* fb, float t, uint32_t sd) {
  const int G = 30;
  float k1 = 0.05f + (mix(sd, 7) % 16) * 0.003f;
  for (int gy = 0; gy < G; gy++) {
    float by = gy * (128.0f / G) + 2.0f;
    float wy = fcos(by * 0.06f - t * 1.0f);
    for (int gx = 0; gx < G; gx++) {
      float bx = gx * (128.0f / G) + 2.0f;
      float d = fsin(bx * k1 + t * 1.5f) + wy;
      splat(fb, bx + d * 3.2f, by + d * 3.2f, 12 + (int)(d * 9.0f));
    }
  }
}

// Interference of three expanding wavefronts, sampled on a grid. Crests light
// up where the sources agree, so the pattern roils without any particle.
static void fRipple(uint16_t* fb, float t, uint32_t sd) {
  float sx[3], sy[3];
  for (int k = 0; k < 3; k++) {
    uint32_t h = mix(sd ^ k, 8);
    float a = t * (0.3f + (h & 15) * 0.02f) + k * 2.1f;
    sx[k] = 64.0f + 34.0f * fcos(a);
    sy[k] = 64.0f + 34.0f * fsin(a * 0.8f + k);
  }
  // Step 3 rather than 2: nine times fewer samples than per-pixel, and the
  // splat covers the gaps. At step 2 this was the one field that could not
  // hold framerate.
  for (int y = 0; y < 128; y += 3) {
    for (int x = 0; x < 128; x += 3) {
      float v = 0;
      for (int k = 0; k < 3; k++) {
        float dx = x - sx[k], dy = y - sy[k];
        v += fsin(sqrtf(dx * dx + dy * dy) * 0.28f - t * 3.0f);
      }
      if (v > 1.4f) splat(fb, (float)x, (float)y, (int)((v - 1.4f) * 22.0f) + 8);
    }
  }
}

// A dense parametric curve whose frequency ratio drifts, so the figure keeps
// folding into new shapes instead of looping.
static void fLissajous(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1500;
  float a = 3.0f + (mix(sd, 9) % 4);
  float b = a + 1.0f + 0.35f * fsin(t * 0.13f);
  float ph = t * 0.5f;
  for (int i = 0; i < N; i++) {
    float u = (TAU * i) / N;
    splat(fb, 64.0f + 56.0f * fsin(a * u + ph),
              64.0f + 56.0f * fsin(b * u), 16);
  }
}

// Two counter-rotating helices seen side-on, with depth from the z term.
static void fHelix(uint16_t* fb, float t, uint32_t sd) {
  const int N = 460;
  float tw = 0.10f + (mix(sd, 10) % 10) * 0.008f;
  for (int strand = 0; strand < 2; strand++) {
    float off = strand * 3.14159f;
    for (int i = 0; i < N; i++) {
      float y = (i / (float)N) * 150.0f - 11.0f;
      float a = y * tw + t * 1.4f + off;
      float z = fcos(a);
      float x = 64.0f + 44.0f * fsin(a);
      splat(fb, x, y, 8 + (int)((z + 1.0f) * 11.0f));
    }
  }
}

// Particles advected by a slowly turning flow field, respawning on a cycle.
// The closest thing here to something organic.
static void fSwarm(uint16_t* fb, float t, uint32_t sd) {
  const int N = 900;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 12);
    float life = fmodf(((h & 1023) / 1024.0f) + t * 0.09f, 1.0f);
    float x0 = (float)((h >> 10) & 127);
    float y0 = (float)((h >> 17) & 127);
    float age = life * 34.0f;
    // Two octaves of a sine flow field, integrated crudely over `age`.
    float ang = fsin(x0 * 0.045f + t * 0.4f) * 2.2f +
                fcos(y0 * 0.037f - t * 0.31f) * 2.2f;
    float x = x0 + fcos(ang) * age;
    float y = y0 + fsin(ang) * age;
    int amp = (int)(30.0f * (1.0f - fabsf(life - 0.5f) * 2.0f)) + 3;
    splat(fb, x, y, amp);
  }
}

// Rings expanding from the centre and fading, staggered so one is always
// arriving as another dissolves.
static void fBloom(uint16_t* fb, float t, uint32_t sd) {
  const int RINGS = 9;
  float jitter = (mix(sd, 13) % 100) * 0.01f;
  for (int b = 0; b < RINGS; b++) {
    float ph = fmodf(t * 0.30f + b / (float)RINGS, 1.0f);
    float r = ph * 82.0f;
    int amp = (int)(31 * (1.0f - ph));
    if (amp < 2) continue;
    int n = 26 + (int)(r * 2.1f);
    float spin = t * 0.22f + b * 1.1f + jitter;
    for (int i = 0; i < n; i++) {
      float a = (TAU * i) / n + spin;
      splat(fb, 64.0f + r * fcos(a), 64.0f + r * fsin(a), amp);
    }
  }
}

// ---------------------------------------------------------------- dispatch
void vizField(TFT_eSprite& s, int index, uint32_t seed, float t) {
  trigInit();
  uint16_t* fb = (uint16_t*)s.getPointer();
  switch (((index % VIZ_COUNT) + VIZ_COUNT) % VIZ_COUNT) {
    case 0:  fSpiral(fb, t, seed);    break;
    case 1:  fVortex(fb, t, seed);    break;
    case 2:  fStarfield(fb, t, seed); break;
    case 3:  fTunnel(fb, t, seed);    break;
    case 4:  fRain(fb, t, seed);      break;
    case 5:  fOrbits(fb, t, seed);    break;
    case 6:  fLattice(fb, t, seed);   break;
    case 7:  fRipple(fb, t, seed);    break;
    case 8:  fLissajous(fb, t, seed); break;
    case 9:  fHelix(fb, t, seed);     break;
    case 10: fSwarm(fb, t, seed);     break;
    default: fBloom(fb, t, seed);     break;
  }
}

void vizDraw(TFT_eSprite& s, int index, uint32_t seed, float tSec, float progress,
             const char* track, const char* artist) {
  s.fillSprite(INK_OFF);
  vizField(s, index, seed, tSec);

  uint16_t* fb = (uint16_t*)s.getPointer();

  // Darken a band so the type reads over whatever is moving beneath it.
  const int bandH = 30, bandY = SCR_H - bandH;
  for (int y = bandY; y < SCR_H; y++) {
    uint16_t* row = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      uint16_t c = row[x];
      int lum = ((c >> 11) & 0x1F) >> 2;
      row[x] = (uint16_t)((lum << 11) | ((lum << 1) << 5) | lum);
    }
  }

  s.setTextDatum(TC_DATUM);
  s.setTextFont(1);
  s.setTextSize(1);

  char buf[48];
  if (track && *track) {
    strncpy(buf, track, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4) buf[strlen(buf) - 1] = 0;
    s.setTextColor(INK_ON);
    s.drawString(buf, SCR_W / 2, bandY + 4);
  }
  if (artist && *artist) {
    strncpy(buf, artist, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    while (s.textWidth(buf) > SCR_W - 6 && strlen(buf) > 4) buf[strlen(buf) - 1] = 0;
    s.setTextColor(0xC618);                 // a touch under white, still neutral
    s.drawString(buf, SCR_W / 2, bandY + 14);
  }

  // Seek bar. This one is genuinely accurate -- straight from progress_ms.
  const int barY = SCR_H - 5, barX = 6, barW = SCR_W - 12;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  int done = (int)(barW * progress);
  s.drawFastHLine(barX, barY, barW, 0x4208);
  s.drawFastHLine(barX, barY, done, INK_ON);
  s.fillRect(barX + done - 1, barY - 2, 3, 5, INK_ON);

  s.setTextFont(1);
  s.setTextSize(1);
}
