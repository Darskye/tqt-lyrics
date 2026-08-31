#include "albumart.h"
#include "net.h"

#if HAVE_SECRETS

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

#define ART_MAX_BYTES 32768        // a 64x64 Spotify thumbnail is a few KB
#define BUCKETS       512          // 3 bits per channel

// TJpg_Decoder takes a plain function pointer, so the accumulator lives here
// rather than being captured.
static uint32_t g_bucket[BUCKETS];
static uint32_t g_pixels;
static uint32_t g_lumaSum;
static uint32_t g_colourful;

static bool jpegOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  (void)x; (void)y;
  uint32_t n = (uint32_t)w * (uint32_t)h;
  for (uint32_t i = 0; i < n; i++) {
    uint16_t c = bmp[i];
    int r = ((c >> 11) & 0x1F) << 3;
    int g = ((c >> 5)  & 0x3F) << 2;
    int b = ( c        & 0x1F) << 3;

    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int luma = (r * 77 + g * 151 + b * 28) >> 8;

    g_pixels++;
    g_lumaSum += (uint32_t)luma;

    int sat = mx ? ((mx - mn) * 255 / mx) : 0;
    // Near-black and washed-out pixels say nothing about the sleeve's colour.
    if (luma < 28 || sat < 60) continue;

    g_colourful++;
    int idx = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
    g_bucket[idx] += (uint32_t)sat;     // colourful pixels count for more
  }
  return true;                          // keep decoding
}

// Normalise brightness so the colour reads on black without shifting its hue.
static uint16_t normalise(int r, int g, int b) {
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  if (mx < 1) return 0xFFFF;
  int scale = (235 * 256) / mx;
  r = (r * scale) >> 8; if (r > 255) r = 255;
  g = (g * scale) >> 8; if (g > 255) g = 255;
  b = (b * scale) >> 8; if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool artFetchInk(const char* imageUrl, uint16_t& outInk) {
  if (!imageUrl || !*imageUrl) return false;
  Serial.printf("[art] %s\n", imageUrl);

  uint8_t* buf = (uint8_t*)malloc(ART_MAX_BYTES);
  if (!buf) { Serial.println("[art] no memory for image buffer"); return false; }

  size_t len = 0;
  {
    WiFiClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start);
    client.setTimeout(12000);

    HTTPClient http;
    if (!http.begin(client, imageUrl)) {
      Serial.println("[art] begin failed");
      free(buf);
      return false;
    }
    int code = http.GET();
    if (code != 200) {
      Serial.printf("[art] fetch -> %d\n", code);
      http.end();
      free(buf);
      return false;
    }

    // Read the body ourselves rather than via getString(): this is binary and
    // a String would stop at the first zero byte.
    WiFiClient* st = http.getStreamPtr();
    uint32_t deadline = millis() + 8000;
    while (http.connected() && len < ART_MAX_BYTES && millis() < deadline) {
      size_t avail = st->available();
      if (!avail) {
        if (http.getSize() > 0 && len >= (size_t)http.getSize()) break;
        delay(2);
        continue;
      }
      size_t take = avail;
      if (len + take > ART_MAX_BYTES) take = ART_MAX_BYTES - len;
      int got = st->readBytes(buf + len, take);
      if (got <= 0) break;
      len += (size_t)got;
      if (http.getSize() > 0 && len >= (size_t)http.getSize()) break;
    }
    http.end();
  }

  if (len < 128) {
    Serial.printf("[art] short read (%u bytes)\n", (unsigned)len);
    free(buf);
    return false;
  }

  memset(g_bucket, 0, sizeof(g_bucket));
  g_pixels = g_lumaSum = g_colourful = 0;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);          // we read RGB565 fields directly
  TJpgDec.setCallback(jpegOut);
  JRESULT res = TJpgDec.drawJpg(0, 0, buf, len);
  free(buf);

  if (res != JDR_OK || g_pixels == 0) {
    Serial.printf("[art] decode failed (%d)\n", (int)res);
    return false;
  }

  uint32_t meanLuma = g_lumaSum / g_pixels;
  uint32_t colourPct = (g_colourful * 100) / g_pixels;

  int best = -1;
  uint32_t bestW = 0;
  for (int i = 0; i < BUCKETS; i++)
    if (g_bucket[i] > bestW) { bestW = g_bucket[i]; best = i; }

  // A sleeve that is essentially black, white or greyscale has no colour to
  // borrow. White is both the honest answer and the right-looking one.
  if (best < 0 || colourPct < 3) {
    Serial.printf("[art] %ux px, mean luma %u, colourful %u%% -> white\n",
                  (unsigned)g_pixels, (unsigned)meanLuma, (unsigned)colourPct);
    return false;
  }

  // Bucket centre, back to 8-bit.
  int r = (((best >> 6) & 7) << 5) | 16;
  int g = (((best >> 3) & 7) << 5) | 16;
  int b = (( best       & 7) << 5) | 16;

  outInk = normalise(r, g, b);
  Serial.printf("[art] %u px, mean luma %u, colourful %u%%, rgb(%d,%d,%d) -> 0x%04X\n",
                (unsigned)g_pixels, (unsigned)meanLuma, (unsigned)colourPct,
                r, g, b, outInk);
  return true;
}

#else

bool artFetchInk(const char* imageUrl, uint16_t& outInk) {
  (void)imageUrl; (void)outInk;
  return false;
}

#endif
