#include "albumart.h"
#include "net.h"

#if HAVE_SECRETS

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

#define ART_MAX_BYTES 65536        // a ~300px Spotify cover runs 20-40KB

// TJpg_Decoder takes a plain function pointer, so the decode target lives here
// rather than being captured. It receives the image at whatever size the
// decoder produced; the fit to 128x128 happens afterwards.
static uint16_t* g_dst = nullptr;
static int       g_dstW = 0, g_dstH = 0;

static bool jpegToBuf(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!g_dst) return false;
  for (int row = 0; row < h; row++) {
    int dy = y + row;
    if (dy < 0 || dy >= g_dstH) continue;
    for (int col = 0; col < w; col++) {
      int dx = x + col;
      if (dx < 0 || dx >= g_dstW) continue;
      g_dst[dy * g_dstW + dx] = bmp[row * w + col];
    }
  }
  return true;                     // keep decoding
}

// Bilinear resample into the panel-sized buffer. Cropping to 128 threw away a
// quarter of the sleeve -- covers put their artwork and lettering right up to
// the edge, so a centre crop reads as a zoomed-in mistake. Interpolating keeps
// the whole cover and is markedly smoother than decoding at the next scale
// down, which would only give ~75px to work with.
static void resampleTo(uint16_t* dst, const uint16_t* src, int sw, int sh) {
  for (int y = 0; y < ART_H; y++) {
    int32_t sy = (y * sh << 8) / ART_H;
    int y0 = sy >> 8, fy = sy & 0xFF;
    int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
    for (int x = 0; x < ART_W; x++) {
      int32_t sx = (x * sw << 8) / ART_W;
      int x0 = sx >> 8, fx = sx & 0xFF;
      int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;

      uint16_t a = src[y0 * sw + x0], b = src[y0 * sw + x1];
      uint16_t c = src[y1 * sw + x0], d = src[y1 * sw + x1];

      int r = ((((a >> 11) & 0x1F) * (256 - fx) + (((b >> 11) & 0x1F)) * fx) * (256 - fy) +
               (((c >> 11) & 0x1F) * (256 - fx) + (((d >> 11) & 0x1F)) * fx) * fy) >> 16;
      int g = ((((a >> 5) & 0x3F) * (256 - fx) + (((b >> 5) & 0x3F)) * fx) * (256 - fy) +
               (((c >> 5) & 0x3F) * (256 - fx) + (((d >> 5) & 0x3F)) * fx) * fy) >> 16;
      int bl = (((a & 0x1F) * (256 - fx) + (b & 0x1F) * fx) * (256 - fy) +
                ((c & 0x1F) * (256 - fx) + (d & 0x1F) * fx) * fy) >> 16;

      dst[y * ART_W + x] = (uint16_t)((r << 11) | (g << 5) | bl);
    }
  }
}

bool artFetchBitmap(const char* imageUrl, uint16_t* dst) {
  if (!imageUrl || !*imageUrl || !dst) return false;
  Serial.printf("[art] fetching %s\n", imageUrl);

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

    // Read the body directly rather than via getString(): this is binary and
    // a String would stop at the first zero byte.
    WiFiClient* st = http.getStreamPtr();
    int expect = http.getSize();
    uint32_t deadline = millis() + 8000;
    while (http.connected() && len < ART_MAX_BYTES && millis() < deadline) {
      size_t avail = st->available();
      if (!avail) {
        if (expect > 0 && len >= (size_t)expect) break;
        delay(2);
        continue;
      }
      size_t take = avail;
      if (len + take > ART_MAX_BYTES) take = ART_MAX_BYTES - len;
      int got = st->readBytes(buf + len, take);
      if (got <= 0) break;
      len += (size_t)got;
      if (expect > 0 && len >= (size_t)expect) break;
    }
    http.end();
  }

  if (len < 128) {
    Serial.printf("[art] short read (%u bytes)\n", (unsigned)len);
    free(buf);
    return false;
  }

  // Largest power-of-two reduction that still covers the panel, so a ~300px
  // cover decodes to 150 (scale 2) rather than 75. The remaining 150 -> 128 is
  // done by interpolation below, which keeps the whole sleeve.
  uint16_t jw = 0, jh = 0;
  TJpgDec.getJpgSize(&jw, &jh, buf, len);
  if (jw == 0 || jh == 0) { Serial.println("[art] not a readable jpeg"); free(buf); return false; }

  uint8_t scale = 1;
  while (scale < 8 && (jw / (scale * 2)) >= ART_W && (jh / (scale * 2)) >= ART_H)
    scale *= 2;

  int outW = jw / scale, outH = jh / scale;

  // Decode at native size into scratch, then fit. The scratch is large enough
  // that malloc will serve it from PSRAM, which is fine: it is written once,
  // read once, and freed.
  uint16_t* tmp = (uint16_t*)malloc((size_t)outW * outH * sizeof(uint16_t));
  if (!tmp) { Serial.println("[art] no memory for decode scratch"); free(buf); return false; }
  memset(tmp, 0, (size_t)outW * outH * sizeof(uint16_t));

  memset(dst, 0, ART_PX * sizeof(uint16_t));
  g_dst  = tmp;
  g_dstW = outW;
  g_dstH = outH;

  // setSwapBytes(false) so the buffer comes out in native RGB565, which is
  // what TFT_eSprite::pushImage expects with the sprite's own swap flag off.
  // The documented pairing is setSwapBytes(true), but that is for callbacks
  // that push straight to a display already configured to swap; here it
  // reverses the bytes and scrambles the channels. Verified against the source
  // image: for one cover the centre pixel must read 0x2040, not 0x4020.
  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpegToBuf);
  JRESULT res = TJpgDec.drawJpg(0, 0, buf, len);

  g_dst = nullptr;
  free(buf);

  if (res != JDR_OK) {
    Serial.printf("[art] decode failed (%d)\n", (int)res);
    free(tmp);
    return false;
  }

  resampleTo(dst, tmp, outW, outH);
  free(tmp);

  // Centre pixel, so the byte order can be checked against the source image
  // rather than guessed at from how it looks.
  uint16_t c = dst[(ART_H / 2) * ART_W + (ART_W / 2)];
  Serial.printf("[art] %ux%u jpeg, %u bytes, /%u -> %dx%d, fit to %dx%d, "
                "centre px 0x%04X = rgb(%d,%d,%d)\n",
                jw, jh, (unsigned)len, scale, outW, outH, ART_W, ART_H, c,
                ((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3);
  return true;
}

#else

bool artFetchBitmap(const char* imageUrl, uint16_t* dst) {
  (void)imageUrl; (void)dst;
  return false;
}

#endif
