#include "albumart.h"
#include "net.h"

#if HAVE_SECRETS

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

#define ART_MAX_BYTES 32768        // a 64x64 Spotify thumbnail is a few KB

// TJpg_Decoder takes a plain function pointer, so the destination lives here
// rather than being captured.
static uint16_t* g_dst = nullptr;

static bool jpegToBuf(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!g_dst) return false;
  for (int row = 0; row < h; row++) {
    int dy = y + row;
    if (dy < 0 || dy >= ART_H) continue;
    for (int col = 0; col < w; col++) {
      int dx = x + col;
      if (dx < 0 || dx >= ART_W) continue;
      g_dst[dy * ART_W + dx] = bmp[row * w + col];
    }
  }
  return true;                     // keep decoding
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

  memset(dst, 0, ART_PX * sizeof(uint16_t));
  g_dst = dst;

  // setSwapBytes(false) so the buffer comes out in native RGB565, which is
  // what TFT_eSprite::pushImage expects with the sprite's own swap flag off.
  // The documented pairing is setSwapBytes(true), but that is for callbacks
  // that push straight to a display already configured to swap; here it
  // reverses the bytes and scrambles the channels. Verified against the source
  // image: the centre pixel must read 0x2040, not 0x4020.
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpegToBuf);
  JRESULT res = TJpgDec.drawJpg(0, 0, buf, len);

  g_dst = nullptr;
  free(buf);

  if (res != JDR_OK) {
    Serial.printf("[art] decode failed (%d)\n", (int)res);
    return false;
  }

  // Centre pixel, so the byte order can be checked against the source image
  // rather than guessed at from how it looks.
  uint16_t c = dst[(ART_H / 2) * ART_W + (ART_W / 2)];
  Serial.printf("[art] decoded %u bytes, centre px 0x%04X = rgb(%d,%d,%d)\n",
                (unsigned)len, c,
                ((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3);
  return true;
}

#else

bool artFetchBitmap(const char* imageUrl, uint16_t* dst) {
  (void)imageUrl; (void)dst;
  return false;
}

#endif
