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
  "spiral", "vortex", "starfield", "tunnel", "rain",  "turbulence",
  "ripple", "helix",  "swarm",     "bloom",  "matrix", "fire", "boids"
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


static inline uint16_t rgb16(int r, int g, int b) {
  if (r < 0) r = 0;
  if (r > 255) r = 255;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------------------------------------------------------------- fire
// Heat buffer at half resolution, upscaled bilinearly. Simulating at 64x64 and
// interpolating up looks softer and more liquid than simulating at full res,
// and costs a quarter as much.
#define FW 64
#define FH 64
static uint8_t g_heat[FW * FH];

static uint16_t firePal(int h) {
  if (h <= 0) return 0;
  if (h < 70)  return rgb16(h * 255 / 70, 0, h / 6);
  if (h < 130) return rgb16(255, (h - 70) * 130 / 60, 0);
  if (h < 200) return rgb16(255, 130 + (h - 130) * 90 / 70, (h - 130) * 40 / 70);
  return rgb16(255, 220 + (h - 200) * 35 / 55, 40 + (h - 200) * 180 / 55);
}

static void fFire(uint16_t* fb, float t, uint32_t sd, bool reset) {
  if (reset) memset(g_heat, 0, sizeof(g_heat));

  // A wandering hot band rather than uniform noise at the base: that is what
  // gives the slow lava roll instead of a flat sheet of flame.
  for (int x = 0; x < FW; x++) {
    float bias = fbm((float)x * 0.05f, t * 0.35f, sd);
    uint32_t h = mix(sd ^ (uint32_t)x, (uint32_t)(t * 30.0f));
    // Squaring the bias digs cool gaps between hot columns. A uniform base
    // rises as a flat sheet; the gaps are what make flames read as tongues.
    int v = 30 + (int)(bias * bias * 240.0f) + (int)(h & 25);
    if (v > 255) v = 255;
    g_heat[(FH - 1) * FW + x] = (uint8_t)v;
    g_heat[(FH - 2) * FW + x] = (uint8_t)(v * 3 / 4);
  }

  // Rows are written top-down while reading the rows below, which still hold
  // last frame values. That lag is what makes the heat climb.
  for (int y = 0; y < FH - 2; y++) {
    uint8_t* dst = g_heat + y * FW;
    const uint8_t* a = g_heat + (y + 1) * FW;
    const uint8_t* b = g_heat + (y + 2) * FW;
    for (int x = 0; x < FW; x++) {
      int xl = (x + FW - 1) & (FW - 1), xr = (x + 1) & (FW - 1);
      int sum = a[xl] + a[x] + a[xr] + b[x];
      int cool = 1 + (int)(mix((uint32_t)(x * 31 + y * 17),
                               (uint32_t)(t * 21.0f)) & 5);
      int v = (sum >> 2) - cool;
      dst[x] = v < 0 ? 0 : (uint8_t)v;
    }
  }

  for (int y = 0; y < SCR_H; y++) {
    float sy = (float)y * (FH - 1) / (float)(SCR_H - 1);
    int y0 = (int)sy;
    int y1 = (y0 + 1 < FH) ? y0 + 1 : y0;
    float fy = sy - (float)y0;
    const uint8_t* r0 = g_heat + y0 * FW;
    const uint8_t* r1 = g_heat + y1 * FW;
    uint16_t* out = fb + y * SCR_W;
    for (int x = 0; x < SCR_W; x++) {
      float sx = (float)x * (FW - 1) / (float)(SCR_W - 1);
      int x0 = (int)sx;
      int x1 = (x0 + 1 < FW) ? x0 + 1 : x0;
      float fx = sx - (float)x0;
      float top = (float)r0[x0] + ((float)r0[x1] - (float)r0[x0]) * fx;
      float bot = (float)r1[x0] + ((float)r1[x1] - (float)r1[x0]) * fx;
      out[x] = firePal((int)(top + (bot - top) * fy));
    }
  }
}

// ---------------------------------------------------------------- boids
#define NBOID 90
static float g_bx[NBOID], g_by[NBOID], g_bvx[NBOID], g_bvy[NBOID];

static void fBoids(uint16_t* fb, float t, uint32_t sd, bool reset, float dt) {
  if (reset) {
    for (int i = 0; i < NBOID; i++) {
      uint32_t h = mix(sd ^ (uint32_t)i, 131);
      g_bx[i] = (float)(h & 127);
      g_by[i] = (float)((h >> 7) & 127);
      float a = (float)((h >> 14) & 1023) * (TAU / 1024.0f);
      g_bvx[i] = fcos(a) * 26.0f;
      g_bvy[i] = fsin(a) * 26.0f;
    }
  }

  for (int i = 0; i < NBOID; i++) {
    float sx = 0, sy = 0, ax = 0, ay = 0, cx = 0, cy = 0;
    int nNear = 0, nSep = 0;
    // Cap the neighbours considered. The full O(N^2) scan costs most exactly
    // when the flock tightens -- more pairs fall inside the radius -- so the
    // framerate dipped hardest at the most interesting moment. Capping makes
    // the cost near-constant. The scan starts at a different neighbour for
    // each bird so the sample is not always the same subset.
    for (int k = 1; k < NBOID && nNear < 14; k++) {
      int j = i + k;
      if (j >= NBOID) j -= NBOID;
      float dx = g_bx[j] - g_bx[i], dy = g_by[j] - g_by[i];
      float d2 = dx * dx + dy * dy;
      if (d2 > 900.0f) continue;
      cx += g_bx[j];  cy += g_by[j];
      ax += g_bvx[j]; ay += g_bvy[j];
      nNear++;
      if (d2 < 64.0f && d2 > 0.01f) {
        sx -= dx / d2; sy -= dy / d2;
        nSep++;
      }
    }
    if (nNear) {
      cx = cx / (float)nNear - g_bx[i];
      cy = cy / (float)nNear - g_by[i];
      ax = ax / (float)nNear - g_bvx[i];
      ay = ay / (float)nNear - g_bvy[i];
      g_bvx[i] += cx * 0.55f * dt + ax * 1.5f * dt;
      g_bvy[i] += cy * 0.55f * dt + ay * 1.5f * dt;
    }
    if (nSep) {
      g_bvx[i] += sx * 260.0f * dt;
      g_bvy[i] += sy * 260.0f * dt;
    }
    // A slow noise field underneath keeps the flock wandering rather than
    // settling into one steady orbit.
    float w = fbm(g_bx[i] * 0.02f, g_by[i] * 0.02f + t * 0.2f, sd) * TAU;
    g_bvx[i] += fcos(w) * 22.0f * dt;
    g_bvy[i] += fsin(w) * 22.0f * dt;

    float sp = sqrtf(g_bvx[i] * g_bvx[i] + g_bvy[i] * g_bvy[i]);
    if (sp < 0.001f) sp = 0.001f;
    g_bvx[i] = g_bvx[i] / sp * 34.0f;
    g_bvy[i] = g_bvy[i] / sp * 34.0f;

    g_bx[i] += g_bvx[i] * dt;
    g_by[i] += g_bvy[i] * dt;
    if (g_bx[i] < -6.0f) g_bx[i] += 140.0f; else if (g_bx[i] > 134.0f) g_bx[i] -= 140.0f;
    if (g_by[i] < -6.0f) g_by[i] += 140.0f; else if (g_by[i] > 134.0f) g_by[i] -= 140.0f;
  }

  for (int i = 0; i < NBOID; i++) {
    float sp = sqrtf(g_bvx[i] * g_bvx[i] + g_bvy[i] * g_bvy[i]);
    if (sp < 0.001f) sp = 0.001f;
    float ux = g_bvx[i] / sp, uy = g_bvy[i] / sp;
    // A chevron rather than a dot: swept-back wings show heading, which is
    // what makes a flock legible at five pixels a bird.
    float nx = -uy, ny = ux;
    float tx = g_bx[i] + ux * 3.4f,  ty = g_by[i] + uy * 3.4f;
    float lx = g_bx[i] - ux * 2.0f + nx * 2.6f, ly = g_by[i] - uy * 2.0f + ny * 2.6f;
    float rx = g_bx[i] - ux * 2.0f - nx * 2.6f, ry = g_by[i] - uy * 2.0f - ny * 2.6f;
    // Hue per bird rather than per heading: a flock aligns, so colouring by
    // direction turns the whole flock one colour.
    int hue = (int)(mix(sd ^ (uint32_t)i, 517) % 192) + (int)(t * 9.0f);
    // Sub-pixel splats rather than AA capsules. A capsule costs ~160 pixel
    // ops with a sqrt each; at five pixels a bird that detail is invisible,
    // and 180 of them a frame was the whole cost of this field.
    splat(fb, tx, ty, hue, 30);
    splat(fb, (tx + lx) * 0.5f, (ty + ly) * 0.5f, hue, 26);
    splat(fb, lx, ly, hue, 20);
    splat(fb, (tx + rx) * 0.5f, (ty + ry) * 0.5f, hue, 26);
    splat(fb, rx, ry, hue, 20);
  }
}

// ---------------------------------------------------------------- matrix
// Half-width katakana, drawn as 8x8 bitmaps rather than typed. TFT_eSPI's
// built-in fonts and every GFX free font are ASCII only (0x20..0x7E), so
// U+FF66..U+FF9D simply are not in them. Drawing the glyphs also gives an
// exact 8px cell, which is what makes the 16x16 rain grid line up.
#define KANA_W 8
#define KANA_H 8

static const uint8_t kKana[][KANA_H] = {
  {0xFE,0x02,0x04,0x08,0x18,0x28,0x48,0x08}, // a
  {0x04,0x08,0x10,0x30,0x50,0x10,0x10,0x10}, // i
  {0x10,0x7C,0x44,0x44,0x04,0x08,0x10,0x20}, // u
  {0xFE,0x10,0x10,0x10,0x10,0x10,0x10,0xFE}, // e
  {0x10,0xFE,0x10,0x3C,0x54,0x14,0x24,0x48}, // o
  {0x10,0xFE,0x12,0x12,0x22,0x24,0x48,0x30}, // ka
  {0x08,0x7E,0x08,0xFF,0x08,0x08,0x10,0x20}, // ki
  {0x7C,0x04,0x08,0x08,0x10,0x20,0x40,0x00}, // ku
  {0x08,0x7E,0x88,0x08,0x10,0x10,0x20,0x40}, // ke
  {0xFE,0x02,0x02,0x02,0x02,0x02,0xFE,0x00}, // ko
  {0x24,0xFF,0x24,0x24,0x04,0x08,0x10,0x20}, // sa
  {0x40,0x04,0x40,0x08,0x02,0x02,0x04,0x38}, // shi
  {0xFE,0x04,0x08,0x18,0x28,0x48,0x88,0x08}, // su
  {0x20,0xFC,0x24,0x24,0x20,0x20,0x22,0x1C}, // se
  {0x44,0x04,0x08,0x08,0x10,0x20,0x40,0x00}, // so
  {0x7C,0x44,0x48,0xFE,0x10,0x20,0x40,0x00}, // ta
  {0x1C,0x60,0xFE,0x10,0x10,0x10,0x20,0x40}, // chi
  {0x44,0x44,0x04,0x08,0x08,0x10,0x20,0x40}, // tsu
  {0x7C,0x00,0xFE,0x10,0x10,0x10,0x20,0x40}, // te
  {0x20,0x20,0x3C,0x22,0x20,0x20,0x20,0x20}, // to
  {0x10,0xFE,0x10,0x10,0x10,0x10,0x20,0x40}, // na
  {0x7C,0x00,0x00,0x00,0x00,0x00,0xFE,0x00}, // ni
  {0x44,0x44,0x44,0x44,0x82,0x82,0x82,0x00}, // ha
  {0x40,0x7C,0x40,0x40,0x7E,0x40,0x40,0x3E}, // hi
  {0xFE,0x02,0x04,0x08,0x10,0x20,0x40,0x00}, // fu
  {0x00,0x00,0x18,0x24,0x42,0x81,0x00,0x00}, // he
  {0x10,0xFE,0x10,0x54,0x54,0x92,0x10,0x10}, // ho
  {0xFE,0x02,0x7C,0x10,0x10,0x20,0x40,0x00}, // ma
  {0x7C,0x00,0x3E,0x00,0x1F,0x00,0x00,0x00}, // mi
  {0x08,0x08,0x10,0x24,0x42,0x42,0xFE,0x00}, // mu
  {0x02,0x44,0x28,0x10,0x28,0x44,0x82,0x00}, // me
  {0x7C,0x10,0xFE,0x10,0x10,0x12,0x0C,0x00}, // mo
  {0x10,0x92,0x54,0x38,0x10,0x10,0x10,0x00}, // ya
  {0x7C,0x04,0x04,0x04,0x04,0xFE,0x00,0x00}, // yu
  {0xFE,0x02,0x7E,0x02,0x02,0xFE,0x00,0x00}, // yo
  {0x7C,0x00,0xFE,0x02,0x04,0x08,0x30,0x00}, // ra
  {0x44,0x44,0x44,0x44,0x44,0x48,0x50,0x60}, // ri
  {0x44,0x44,0x44,0x44,0x44,0x4A,0x52,0x62}, // ru
  {0x40,0x40,0x40,0x40,0x42,0x44,0x38,0x00}, // re
  {0xFE,0x82,0x82,0x82,0x82,0x82,0xFE,0x00}, // ro
  {0xFE,0x82,0x82,0x02,0x04,0x08,0x30,0x00}, // wa
  {0x40,0x04,0x40,0x08,0x02,0x04,0x08,0x30}, // n
};
#define KANA_COUNT ((int)(sizeof(kKana) / sizeof(kKana[0])))

// Green throughout. The leading cell is washed toward white, which is what
// gives the rain its glowing head; the tail stays pure green.
static void drawKana(uint16_t* fb, int px, int py, int gi, int lum, bool head) {
  if (lum <= 0) return;
  if (lum > 31) lum = 31;
  const uint8_t* g = kKana[gi];
  int r = head ? (lum * 7) / 10 : 0;
  int b = head ? (lum * 7) / 10 : lum / 7;
  for (int y = 0; y < KANA_H; y++) {
    uint8_t bits = g[y];
    if (!bits) continue;
    int Y = py + y;
    if ((unsigned)Y >= (unsigned)SCR_H) continue;
    for (int x = 0; x < KANA_W; x++) {
      if (!(bits & (0x80 >> x))) continue;
      addPix(fb, px + x, Y, r, lum << 1, b);
    }
  }
}

// Columns of falling katakana at independent speeds. Glyphs re-roll as they
// fall, which is the flicker that makes the rain read as characters rather
// than a texture.
static void fMatrix(uint16_t* fb, float t, uint32_t sd) {
  const int COLS = SCR_W / KANA_W;          // 16
  const int ROWS = SCR_H / KANA_H;          // 16

  for (int c = 0; c < COLS; c++) {
    uint32_t h = mix(sd ^ (uint32_t)c, 91);
    float spd  = 5.0f + (float)(h & 15) * 1.5f;         // cells per second
    float off  = (float)((h >> 4) & 255) / 256.0f * 30.0f;
    int   len  = 5 + (int)((h >> 12) & 9);
    float span = ROWS + len + 4.0f;
    float head = fmodf(off + t * spd, span) - (float)len;

    for (int k = 0; k < len; k++) {
      int row = (int)head - k;
      if (row < 0 || row >= ROWS) continue;

      // Re-roll on a per-cell clock so the whole column does not flip at once.
      uint32_t frame = (uint32_t)(t * (4.0f + (float)(h & 7)));
      uint32_t gh = mix((uint32_t)(c * 131 + row * 17), frame ^ (h >> 3));
      int gi = (int)(gh % (uint32_t)KANA_COUNT);

      int lum = (k == 0) ? 31 : 25 - (k * 23) / len;
      drawKana(fb, c * KANA_W, row * KANA_H, gi, lum, k == 0);
    }
  }
}

// ---------------------------------------------------------------- dispatch
void vizField(TFT_eSprite& s, int index, uint32_t seed, float t) {
  trigInit();
  uint16_t* fb = (uint16_t*)s.getPointer();
  int idx = ((index % VIZ_COUNT) + VIZ_COUNT) % VIZ_COUNT;

  // The closed-form fields are pure functions of t, but fire, boids, face and
  // runner carry state: they need a real dt to stay frame-rate independent,
  // and a reset when switched to, or they inherit whatever the last one left.
  static float lastT = 0.0f;
  static int   lastIdx = -1;
  float dt = t - lastT;
  lastT = t;
  if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;
  bool reset = (idx != lastIdx);
  lastIdx = idx;

  switch (idx) {
    case 0:  fSpiral(fb, t, seed);     break;
    case 1:  fVortex(fb, t, seed);     break;
    case 2:  fStarfield(fb, t, seed);  break;
    case 3:  fTunnel(fb, t, seed);     break;
    case 4:  fRain(fb, t, seed);       break;
    case 5:  fTurbulence(fb, t, seed); break;
    case 6:  fRipple(fb, t, seed);     break;
    case 7:  fHelix(fb, t, seed);      break;
    case 8:  fSwarm(fb, t, seed);      break;
    case 9:  fBloom(fb, t, seed);      break;
    case 10: fMatrix(fb, t, seed);     break;
    case 11: fFire(fb, t, seed, reset);      break;
    default: fBoids(fb, t, seed, reset, dt); break;
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
