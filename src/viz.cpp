#include "viz.h"
#include "typeview.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- fast trig
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

// ---------------------------------------------------------------- noise
// Value noise with smoothstep interpolation. This is what breaks up the
// mechanical regularity: sine fields repeat because they are periodic, and no
// amount of tuning fixes that. Noise does not repeat.
static inline uint32_t hash2(int x, int y, uint32_t s) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
static inline float h2f(int x, int y, uint32_t s) {
  return (float)(hash2(x, y, s) >> 8) * (1.0f / 16777216.0f);   // 0..1
}
static float vnoise(float x, float y, uint32_t s) {
  int xi = (int)(x + 4096.0f) - 4096, yi = (int)(y + 4096.0f) - 4096;
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3.0f - 2.0f * fx);
  fy = fy * fy * (3.0f - 2.0f * fy);
  float a = h2f(xi, yi, s),     b = h2f(xi + 1, yi, s);
  float c = h2f(xi, yi + 1, s), d = h2f(xi + 1, yi + 1, s);
  float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
  return ab + (cd - ab) * fy;                                   // 0..1
}
// Two octaves is enough turbulence at this scale and half the cost of three.
static inline float fbm(float x, float y, uint32_t s) {
  return vnoise(x, y, s) * 0.65f + vnoise(x * 2.17f, y * 2.17f, s ^ 0x9E37u) * 0.35f;
}

// ---------------------------------------------------------------- colour
// Additive RGB, saturating per channel. Overlapping particles mix into new
// hues on their own, which is where most of the colour range comes from --
// far more than assigning each particle a colour and leaving it there.
static inline void addPix(uint16_t* fb, int x, int y, int r, int g, int b) {
  if ((unsigned)x >= (unsigned)SCR_W || (unsigned)y >= (unsigned)SCR_H) return;
  uint16_t c = fb[y * SCR_W + x];
  int cr = ((c >> 11) & 0x1F) + r;  if (cr > 31) cr = 31;
  int cg = ((c >>  5) & 0x3F) + g;  if (cg > 63) cg = 63;
  int cb = ( c        & 0x1F) + b;  if (cb > 31) cb = 31;
  fb[y * SCR_W + x] = (uint16_t)((cr << 11) | (cg << 5) | cb);
}

// h: 0..191 around the wheel. amp: 0..31.
static inline void hue2rgb(int h, int amp, int& r, int& g, int& b) {
  h = ((h % 192) + 192) % 192;
  int seg = h >> 5, f = h & 31;
  int up = (amp * f) >> 5, dn = amp - up;
  switch (seg) {
    case 0:  r = amp; g = up;  b = 0;   break;
    case 1:  r = dn;  g = amp; b = 0;   break;
    case 2:  r = 0;   g = amp; b = up;  break;
    case 3:  r = 0;   g = dn;  b = amp; break;
    case 4:  r = up;  g = 0;   b = amp; break;
    default: r = amp; g = 0;   b = dn;  break;
  }
}

// Bilinear splat: particles glide at fractional coordinates instead of
// stepping between whole pixels, which is what stops slow motion stuttering.
static inline void splatRGB(uint16_t* fb, float fx, float fy, int r, int g, int b) {
  if (fx < -1.0f || fy < -1.0f || fx > (float)SCR_W || fy > (float)SCR_H) return;

  int xi = (int)(fx + 256.0f) - 256;
  int yi = (int)(fy + 256.0f) - 256;
  int ax = (int)((fx - xi) * 32.0f), ay = (int)((fy - yi) * 32.0f);
  int iax = 32 - ax, iay = 32 - ay;

  int w00 = (iax * iay) >> 5, w10 = (ax * iay) >> 5;
  int w01 = (iax * ay ) >> 5, w11 = (ax * ay ) >> 5;
  addPix(fb, xi,     yi,     (r * w00) >> 5, (g * w00) >> 5, (b * w00) >> 5);
  addPix(fb, xi + 1, yi,     (r * w10) >> 5, (g * w10) >> 5, (b * w10) >> 5);
  addPix(fb, xi,     yi + 1, (r * w01) >> 5, (g * w01) >> 5, (b * w01) >> 5);
  addPix(fb, xi + 1, yi + 1, (r * w11) >> 5, (g * w11) >> 5, (b * w11) >> 5);
}

static inline void splat(uint16_t* fb, float fx, float fy, int hue, int amp) {
  if (amp <= 0) return;
  if (amp > 31) amp = 31;
  int r, g, b;
  hue2rgb(hue, amp, r, g, b);
  splatRGB(fb, fx, fy, r, g << 1, b);
}

static const char* kNames[VIZ_COUNT] = {
  "spiral", "vortex", "starfield", "tunnel",  "rain",   "clifford",
  "turbulence", "ripple", "dejong", "helix",  "swarm",  "bloom",
  "lyricform"
};
const char* vizNameAt(int i) { return kNames[((i % VIZ_COUNT) + VIZ_COUNT) % VIZ_COUNT]; }
int vizIndexForSeed(uint32_t seed) { return (int)(mix(seed, 11) % VIZ_COUNT); }

// ---------------------------------------------------------------- fields

// Phyllotaxis, then shoved around by turbulence so the arms wander instead of
// sitting on perfect logarithmic curves.
static void fSpiral(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1200;
  const float golden = 2.39996323f;
  float spin = t * (0.30f + (mix(sd, 1) % 30) * 0.010f);
  int hue0 = (int)(t * 22.0f);
  for (int i = 0; i < N; i++) {
    float a = i * golden + spin;
    float r = 2.0f * sqrtf((float)i);
    float px = 64.0f + r * fcos(a), py = 64.0f + r * fsin(a);
    float n = fbm(px * 0.020f, py * 0.020f + t * 0.30f, sd);
    float wob = (n - 0.5f) * 26.0f;
    splat(fb, px + wob * fcos(a * 1.7f), py + wob * fsin(a * 1.7f),
          hue0 + (int)(r * 2.1f) + (int)(n * 70.0f), 14 + (int)(n * 17.0f));
  }
}

// A drain, with the inflow broken up by noise so no two revolutions match.
static void fVortex(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1100;
  int hue0 = (int)(t * 30.0f);
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 2);
    float ph  = (h & 1023) / 1024.0f;
    float ang = ((h >> 10) & 1023) * (TAU / 1024.0f);
    float z   = fmodf(ph + t * 0.10f, 1.0f);
    float r   = (1.0f - z) * 70.0f;
    float a   = ang + t * (0.6f + 2.2f / (r * 0.06f + 1.0f));
    float px = 64.0f + r * fcos(a), py = 64.0f + r * fsin(a);
    float n = fbm(px * 0.03f + t * 0.4f, py * 0.03f, sd);
    splat(fb, px + (n - 0.5f) * 20.0f, py + (n - 0.5f) * 20.0f,
          hue0 + (int)(z * 130.0f) + (int)(n * 50.0f), 8 + (int)(z * 22));
  }
}

// Perspective stars, each keeping its own hue, drifting off-axis with noise.
static void fStarfield(uint16_t* fb, float t, uint32_t sd) {
  const int N = 900;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 3);
    float ang = (h & 2047) * (TAU / 2048.0f);
    float rad = 4.0f + ((h >> 11) & 255) * 0.30f;
    float ph  = ((h >> 19) & 511) / 512.0f;
    float z   = 1.0f - fmodf(ph + t * 0.20f, 1.0f);
    if (z < 0.03f) continue;
    float r = rad * (0.9f / z);
    if (r > 95.0f) continue;
    float px = 64.0f + r * fcos(ang), py = 64.0f + r * fsin(ang);
    float n = vnoise(px * 0.05f, py * 0.05f + t, sd);
    splat(fb, px + (n - 0.5f) * 9.0f, py + (n - 0.5f) * 9.0f,
          (int)(h >> 24) + (int)(t * 14.0f), 5 + (int)((1.0f - z) * 26));
  }
}

// A corridor whose rings are warped by noise, so the walls buckle rather than
// staying perfect circles.
static void fTunnel(uint16_t* fb, float t, uint32_t sd) {
  const int RINGS = 18, PER = 62;
  for (int ring = 0; ring < RINGS; ring++) {
    float z = fmodf(ring / (float)RINGS + t * 0.15f, 1.0f);
    if (z < 0.04f) continue;
    float rr = 7.0f / z;
    if (rr > 105.0f) continue;
    float spin = t * 0.45f + ring * 0.21f;
    int amp = 3 + (int)((1.0f - z) * 27);
    int hue = (int)(z * 150.0f + t * 26.0f);
    for (int i = 0; i < PER; i++) {
      float a = (TAU * i) / PER + spin;
      float n = vnoise(fcos(a) * 2.0f + ring, fsin(a) * 2.0f + t * 0.7f, sd);
      float r2 = rr * (0.72f + n * 0.55f);
      splat(fb, 64.0f + r2 * fcos(a), 64.0f + r2 * fsin(a), hue + (int)(n * 60), amp);
    }
  }
}

// Columns that sway on a noise field and shift hue down their length.
static void fRain(uint16_t* fb, float t, uint32_t sd) {
  const int COLS = 46;
  for (int col = 0; col < COLS; col++) {
    uint32_t h = mix(sd ^ col, 5);
    float spd = 24.0f + (h & 63) * 2.0f;
    float off = ((h >> 6) & 511) / 512.0f * 190.0f;
    int len = 12 + ((h >> 15) & 13);
    float head = fmodf(off + t * spd, 200.0f) - 34.0f;
    float bx = col * (128.0f / COLS) + 1.0f;
    int hue0 = (int)(h >> 24) + (int)(t * 18.0f);
    for (int k = 0; k < len; k++) {
      float y = head - k * 2.3f;
      float n = vnoise(bx * 0.08f, y * 0.05f + t * 0.8f, sd);
      splat(fb, bx + (n - 0.5f) * 11.0f, y, hue0 + k * 3, 31 - (k * 27) / len);
    }
  }
}

// Clifford attractor. Genuinely chaotic rather than merely irregular: the
// parameters drift, so the structure keeps folding into shapes it has not
// held before instead of cycling.
static void fClifford(uint16_t* fb, float t, uint32_t sd) {
  const int N = 2200;
  float a = -1.7f + 0.45f * fsin(t * 0.11f + (mix(sd, 6) % 100) * 0.06f);
  float b =  1.8f + 0.40f * fcos(t * 0.083f);
  float c = -1.9f + 0.35f * fsin(t * 0.061f + 1.7f);
  float d = -0.8f + 0.40f * fcos(t * 0.047f + 0.6f);
  float x = 0.1f, y = 0.0f;
  int hue0 = (int)(t * 20.0f);
  for (int i = 0; i < N; i++) {
    float nx = fsin(a * y) + c * fcos(a * x);
    float ny = fsin(b * x) + d * fcos(b * y);
    x = nx; y = ny;
    if (i < 24) continue;                       // let the orbit settle
    splat(fb, 64.0f + x * 25.0f, 64.0f + y * 25.0f,
          hue0 + (int)((x + y) * 26.0f), 11);
  }
}

// Pure fractal noise, rendered as a field of coloured motes drifting through
// it. No geometry at all -- the least "designed"-looking of the set.
static void fTurbulence(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1500;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 7);
    float bx = (float)(h & 127), by = (float)((h >> 7) & 127);
    float n1 = fbm(bx * 0.022f + t * 0.16f, by * 0.022f, sd);
    float n2 = fbm(bx * 0.022f, by * 0.022f - t * 0.13f, sd ^ 0x5Au);
    float x = bx + (n1 - 0.5f) * 46.0f;
    float y = by + (n2 - 0.5f) * 46.0f;
    splat(fb, x, y, (int)(n1 * 200.0f + t * 25.0f), 6 + (int)(n2 * 25.0f));
  }
}

// Interference of moving wavefronts whose sources wander on noise paths, so
// the crests never settle into a standing pattern.
static void fRipple(uint16_t* fb, float t, uint32_t sd) {
  float sx[3], sy[3];
  for (int k = 0; k < 3; k++) {
    float n1 = fbm(t * 0.22f + k * 5.0f, k * 3.0f, sd);
    float n2 = fbm(k * 7.0f, t * 0.19f + k * 2.0f, sd ^ 0x33u);
    sx[k] = 18.0f + n1 * 92.0f;
    sy[k] = 18.0f + n2 * 92.0f;
  }
  int hue0 = (int)(t * 28.0f);
  for (int y = 0; y < 128; y += 3) {
    for (int x = 0; x < 128; x += 3) {
      float v = 0;
      for (int k = 0; k < 3; k++) {
        float dx = x - sx[k], dy = y - sy[k];
        v += fsin(sqrtf(dx * dx + dy * dy) * 0.27f - t * 2.6f);
      }
      if (v > 1.15f)
        splat(fb, (float)x, (float)y, hue0 + (int)(v * 46.0f), (int)((v - 1.15f) * 20.0f) + 7);
    }
  }
}

// De Jong attractor: the other classic chaotic map, denser and lacier than
// Clifford, and it folds differently as the parameters drift.
static void fDeJong(uint16_t* fb, float t, uint32_t sd) {
  const int N = 2400;
  float a = 1.64f + 0.55f * fsin(t * 0.071f + (mix(sd, 9) % 100) * 0.06f);
  float b = 1.90f + 0.50f * fcos(t * 0.059f);
  float c = 0.90f + 0.60f * fsin(t * 0.043f + 2.2f);
  float d = 1.10f + 0.55f * fcos(t * 0.037f + 1.1f);
  float x = 0.05f, y = 0.12f;
  int hue0 = (int)(t * 24.0f);
  for (int i = 0; i < N; i++) {
    float nx = fsin(a * y) - fcos(b * x);
    float ny = fsin(c * x) - fcos(d * y);
    x = nx; y = ny;
    if (i < 24) continue;
    splat(fb, 64.0f + x * 29.0f, 64.0f + y * 29.0f,
          hue0 + (int)((x - y) * 30.0f), 10);
  }
}

// Two strands, noise-perturbed so they fray rather than reading as a diagram.
static void fHelix(uint16_t* fb, float t, uint32_t sd) {
  const int N = 520;
  float tw = 0.09f + (mix(sd, 10) % 10) * 0.008f;
  for (int strand = 0; strand < 2; strand++) {
    float off = strand * 3.14159f;
    for (int i = 0; i < N; i++) {
      float y = (i / (float)N) * 152.0f - 12.0f;
      float a = y * tw + t * 1.2f + off;
      float z = fcos(a);
      float n = vnoise(y * 0.06f, t * 0.8f + strand * 9.0f, sd);
      float x = 64.0f + 42.0f * fsin(a) + (n - 0.5f) * 22.0f;
      splat(fb, x, y + (n - 0.5f) * 7.0f,
            (int)(t * 20.0f) + strand * 90 + (int)(z * 34.0f), 7 + (int)((z + 1.0f) * 11.0f));
    }
  }
}

// Particles advected by a fractal flow field -- the most organic motion here,
// because the field itself is noise rather than a sum of sines.
static void fSwarm(uint16_t* fb, float t, uint32_t sd) {
  const int N = 1300;
  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ i, 12);
    float life = fmodf(((h & 1023) / 1024.0f) + t * 0.075f, 1.0f);
    float x = (float)((h >> 10) & 127), y = (float)((h >> 17) & 127);
    float age = life * 30.0f;
    // Two crude integration steps: enough to curve the paths convincingly.
    for (int k = 0; k < 2; k++) {
      float ang = fbm(x * 0.017f, y * 0.017f + t * 0.22f, sd) * TAU * 2.0f;
      x += fcos(ang) * age * 0.5f;
      y += fsin(ang) * age * 0.5f;
    }
    int amp = (int)(30.0f * (1.0f - fabsf(life - 0.5f) * 2.0f)) + 3;
    splat(fb, x, y, (int)(life * 150.0f + t * 22.0f), amp);
  }
}

// Rings expanding from wandering centres, each radius chewed by noise so they
// arrive as ragged shockwaves rather than clean circles.
static void fBloom(uint16_t* fb, float t, uint32_t sd) {
  const int RINGS = 10;
  for (int b = 0; b < RINGS; b++) {
    float ph = fmodf(t * 0.27f + b / (float)RINGS, 1.0f);
    float r = ph * 86.0f;
    int amp = (int)(31 * (1.0f - ph));
    if (amp < 2) continue;
    float cx = 64.0f + (fbm(b * 4.0f, t * 0.2f, sd) - 0.5f) * 34.0f;
    float cy = 64.0f + (fbm(t * 0.17f, b * 6.0f, sd) - 0.5f) * 34.0f;
    int n = 30 + (int)(r * 2.2f);
    int hue = (int)(t * 26.0f) + b * 17;
    for (int i = 0; i < n; i++) {
      float a = (TAU * i) / n + t * 0.2f;
      float nn = vnoise(fcos(a) * 3.0f + b, fsin(a) * 3.0f + t * 0.5f, sd);
      float rr = r * (0.80f + nn * 0.42f);
      splat(fb, cx + rr * fcos(a), cy + rr * fsin(a), hue + (int)(nn * 55), amp);
    }
  }
}


// ---------------------------------------------------------------- lyricform
// Particles drift chaotically and assemble into the current lyric line, then
// scatter again between lines. Targets are the lit pixels of the line
// rasterised offscreen, sampled evenly down to the particle count.
#define MORPH_MAX 1100

static int16_t g_mtx[MORPH_MAX], g_mty[MORPH_MAX];
static int     g_mn = 0;
static float   g_morph = 0.0f;

void vizMorphAmount(float m) { g_morph = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m); }

void vizMorphSet(TFT_eSprite& mask, const char* text) {
  g_mn = 0;
  int lit = typeRasterise(mask, text);
  if (lit <= 0) return;

  // Sample evenly rather than taking the first MORPH_MAX, which would fill
  // only the top rows and leave the rest of the line unformed.
  const uint8_t* p = (const uint8_t*)mask.getPointer();
  int step = lit > MORPH_MAX ? lit / MORPH_MAX : 1;
  int seen = 0;
  for (int y = 0; y < SCR_H && g_mn < MORPH_MAX; y++) {
    for (int x = 0; x < SCR_W && g_mn < MORPH_MAX; x++) {
      if (!p[y * SCR_W + x]) continue;
      if (seen % step == 0) { g_mtx[g_mn] = (int16_t)x; g_mty[g_mn] = (int16_t)y; g_mn++; }
      seen++;
    }
  }
}

static void fMorph(uint16_t* fb, float t, uint32_t sd) {
  const int N = MORPH_MAX;
  float m = g_morph;
  // Ease the assembly so particles arrive rather than snapping into place.
  float e = m * m * (3.0f - 2.0f * m);

  for (int i = 0; i < N; i++) {
    uint32_t h = mix(sd ^ (uint32_t)i, 77);
    float bx = (float)(h & 127), by = (float)((h >> 7) & 127);

    // Chaotic home: a noise flow field, same family as the swarm.
    float n1 = fbm(bx * 0.020f + t * 0.13f, by * 0.020f, sd);
    float n2 = fbm(bx * 0.020f, by * 0.020f - t * 0.11f, sd ^ 0x77u);
    float cx = bx + (n1 - 0.5f) * 54.0f;
    float cy = by + (n2 - 0.5f) * 54.0f;

    float px = cx, py = cy;
    if (g_mn > 0 && e > 0.001f) {
      int ti = i % g_mn;
      px = cx + ((float)g_mtx[ti] - cx) * e;
      py = cy + ((float)g_mty[ti] - cy) * e;
    }

    // Colour while scattered, white once formed: chaos gets to be colourful,
    // but text has to be legible.
    int amp = 9 + (int)(e * 20.0f);
    int r, g, b;
    hue2rgb((int)(n1 * 190.0f + t * 20.0f), amp, r, g, b);
    g <<= 1;
    int w2 = (int)(e * 32.0f);
    r += ((amp - r) * w2) >> 5;
    g += (((amp << 1) - g) * w2) >> 5;
    b += ((amp - b) * w2) >> 5;
    splatRGB(fb, px, py, r, g, b);
  }
}

// ---------------------------------------------------------------- dispatch
void vizField(TFT_eSprite& s, int index, uint32_t seed, float t) {
  trigInit();
  uint16_t* fb = (uint16_t*)s.getPointer();
  switch (((index % VIZ_COUNT) + VIZ_COUNT) % VIZ_COUNT) {
    case 0:  fSpiral(fb, t, seed);     break;
    case 1:  fVortex(fb, t, seed);     break;
    case 2:  fStarfield(fb, t, seed);  break;
    case 3:  fTunnel(fb, t, seed);     break;
    case 4:  fRain(fb, t, seed);       break;
    case 5:  fClifford(fb, t, seed);   break;
    case 6:  fTurbulence(fb, t, seed); break;
    case 7:  fRipple(fb, t, seed);     break;
    case 8:  fDeJong(fb, t, seed);     break;
    case 9:  fHelix(fb, t, seed);      break;
    case 10: fSwarm(fb, t, seed);      break;
    case 11: fBloom(fb, t, seed);      break;
    default: fMorph(fb, t, seed);      break;
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
      row[x] = (uint16_t)(((((c >> 11) & 0x1F) >> 2) << 11) |
                          ((((c >>  5) & 0x3F) >> 2) <<  5) |
                          (( (c        & 0x1F) >> 2)));
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
    s.setTextColor(0xC618);
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
