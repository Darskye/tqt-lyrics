// LilyGo T-QT Pro -- Spotify lyrics as the whole picture
//
// Stark monochrome kinetic typography on a 0.85" 128x128 panel. There is no
// background layer: each scene owns the entire frame, and all the interest
// comes from scale, motion and layout.
//
// Two sources, because Spotify exposes no lyrics API: the Web API for the
// track and playback position, LRCLIB for synced LRC.
//
// The present path uses ordinary blocking startWrite/pushSprite/endWrite.
// Driving the panel through initDMA() instead hands SPI3 to the ESP-IDF driver
// while TFT_eSPI still writes the same peripheral's registers directly; the
// first frame lands and nothing after it does.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#include "lyrics.h"
#include "typeview.h"
#include "net.h"
#include "viz.h"

#define PIN_BTN_L  0    // cycle style (lyrics) or visualiser (visuals)
#define PIN_BTN_R  47   // toggle visuals <-> lyrics
#define PIN_LCD_BL 10   // active low

#define SPOTIFY_POLL_MS 5000

// How long a gap between lyric lines has to run before the panel gives up
// holding the last line and shows the floating notes instead.
#define NOTES_AFTER_MS 4000

// Panel orientation. 0 and 2 are the two portrait orientations, 180 apart.
// Cycle live with 'o' over serial to find yours, then set it here so it sticks
// across reflashes.
#define SCREEN_ROTATION 2

static TFT_eSPI    tft;
static TFT_eSprite spr[2] = {TFT_eSprite(&tft), TFT_eSprite(&tft)};
static int         cur = 0;

static bool     showStatus = false;
static uint32_t sceneSalt  = 0;      // shifts scene choice without touching timing
static int      fps = 0;
// Per-track seed: picks which visualiser a song gets, and its character.
// Derived from the Spotify track id so a given song always looks the same.
static volatile uint32_t gTrackSeed = 0;

// Right button toggles between the two. Left button re-rolls the look of the
// line currently on screen in MODE_LYRICS, and cycles the visualiser in
// MODE_VIZ. MODE_LYRICS shows lyrics and nothing else: short gaps hold the
// last line, long ones float music notes.
enum { MODE_LYRICS = 0, MODE_VIZ = 1 };
static int gMode = MODE_LYRICS;
static int gVizOverride   = -1;      // -1 = whichever the track hashes to
static uint8_t  rotation = SCREEN_ROTATION;

// Brief on-screen confirmation after a button press, so a press that does
// nothing is distinguishable from a press that was never seen.
static char     gToast[28] = {0};
static uint32_t gToastUntil = 0;
static bool     gStylePending = false;
static void toast(const char* fmt, const char* arg) {
  snprintf(gToast, sizeof(gToast), fmt, arg);
  gToastUntil = millis() + 1400;
}

// ------------------------------------------------------------------ playback
static Lyrics     lyrics;
static NowPlaying np;
static char       loadedTrack[100] = {0};
static bool       haveLyrics  = false;
static int        lastLineIdx = -2;

static volatile uint32_t progressBaseMs = 0;
static volatile uint32_t progressAtMs   = 0;
static volatile bool     isPlaying      = false;

// FNV-1a over the track id: stable across reboots, well spread across songs.
static uint32_t trackSeed(const char* id) {
  uint32_t h = 2166136261u;
  for (const char* p = id; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  return h ? h : 1;
}

static uint32_t playbackMs() {
  if (!isPlaying) return progressBaseMs;
  return progressBaseMs + (millis() - progressAtMs);
}

// Placeholder copy, not lyrics: varied lengths and word counts so every scene
// and the wrapping get exercised when no credentials are configured.
static const char* kDemoLrc =
    "[00:00.00]lyrics\n"
    "[00:02.20]as the whole picture\n"
    "[00:05.00]one line at a time\n"
    "[00:08.00]a different scene each line\n"
    "[00:11.50]stacked\n"
    "[00:13.50]wiped, typed, split apart\n"
    "[00:17.00]this line is long enough that it has to travel across instead\n"
    "[00:21.50]\n"
    "[00:23.50]add src slash secrets dot h\n"
    "[00:27.00]then it follows spotify\n"
    "[00:30.00]\n";



// ------------------------------------------------------------------ network
static void netTask(void*) {
  netBegin();
  for (;;) {
    if (netConnected()) {
      NowPlaying fresh;
      memset(&fresh, 0, sizeof(fresh));
      if (netPollSpotify(fresh)) {
        if (fresh.valid) {
          progressBaseMs = fresh.progressMs;
          progressAtMs   = millis();
          isPlaying      = fresh.playing;

          if (strcmp(fresh.track, loadedTrack) != 0) {
            Serial.printf("[track] %s -- %s\n", fresh.track, fresh.artist);
            String body;
            if (netFetchLyrics(fresh, body)) {
              int n = lyrics.parse(body.c_str());
              haveLyrics = n > 0;
              Serial.printf("[lyrics] %d synced lines\n", n);
            } else {
              lyrics.clear();
              haveLyrics = false;
              Serial.println("[lyrics] none on lrclib for this track");
            }
            strncpy(loadedTrack, fresh.track, sizeof(loadedTrack) - 1);
            loadedTrack[sizeof(loadedTrack) - 1] = 0;
            gTrackSeed  = trackSeed(fresh.trackId);
            Serial.printf("[viz] %s\n", vizNameAt(vizIndexForSeed(gTrackSeed)));
            lastLineIdx = -2;
          }
          np = fresh;
        } else {
          isPlaying = false;
          np.valid  = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(SPOTIFY_POLL_MS));
  }
}

// ------------------------------------------------------------------ buttons
struct Btn { uint8_t pin; bool last; uint32_t tEdge; };
static Btn btnL = {PIN_BTN_L, true, 0};
static Btn btnR = {PIN_BTN_R, true, 0};

static bool clicked(Btn& b) {
  bool now = digitalRead(b.pin);
  uint32_t ms = millis();
  if (now != b.last && (ms - b.tEdge) > 30) {
    b.tEdge = ms;
    b.last  = now;
    Serial.printf("[btn] gpio%d -> %s\n", b.pin, now ? "released" : "PRESSED");
    if (!now) return true;
  }
  return false;
}

// ------------------------------------------------------------------ selftest
static void selfTestLyrics() {
  static const char* sample =
      "[ar:Placeholder Artist]\n"
      "[length:03:00]\n"
      "[00:01.50]alpha\n"
      "[00:10.25]bravo\n"
      "[01:05.00]charlie\n"
      "[00:30.00][02:00.00]delta\n"
      "[00:45.00]\n";

  // Heap, not stack: a Lyrics is ~14KB and loopTask only gets 8KB.
  Lyrics* tp = new Lyrics();
  if (!tp) { Serial.println("[selftest] alloc failed"); return; }
  Lyrics& t = *tp;

  int n = t.parse(sample);
  int pass = 0, total = 0;
  auto check = [&](const char* what, bool ok) {
    total++;
    if (ok) pass++;
    else Serial.printf("  FAIL %s\n", what);
  };

  check("metadata skipped, 6 entries", n == 6);
  check("first stamp 1500ms",     t.timeAt(0) == 1500);
  check("centisecond scaling",    t.timeAt(1) == 10250);
  check("multi-stamp sorted",     t.timeAt(2) == 30000);
  check("empty body kept",        t.timeAt(3) == 45000);
  check("minutes carry",          t.timeAt(4) == 65000);
  check("second stamp of pair",   t.timeAt(5) == 120000);
  check("text parsed",            strcmp(t.text(0), "alpha") == 0);
  check("repeat text duplicated", strcmp(t.text(5), "delta") == 0);
  check("gap line empty",         t.text(3)[0] == 0);
  check("before first is -1",     t.indexAt(0) == -1);
  check("exact boundary hits",    t.indexAt(1500) == 0);
  check("between stamps holds",   t.indexAt(9000) == 0);
  check("past last stays last",   t.indexAt(999999) == 5);

  Serial.printf("[selftest] lyrics %d/%d passed\n", pass, total);
  delete tp;
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  Serial.println();
  Serial.println("=== T-QT Pro lyric type ===");

  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);

  selfTestLyrics();
  Serial.printf("[btn] idle levels: gpio%d=%d gpio%d=%d (1 = not pressed)\n",
                PIN_BTN_L, digitalRead(PIN_BTN_L),
                PIN_BTN_R, digitalRead(PIN_BTN_R));

  tft.init();
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);          // TFT_BACKLIGHT_ON == 0

  // This SDK sets CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096, so a 32KB sprite
  // goes to PSRAM by default and costs real time every frame. TFT_eSPI's
  // PSRAM_ENABLE attribute does not help -- it only picks ps_calloc vs calloc,
  // and plain calloc lands externally too. Raise the threshold across
  // createSprite, then restore it so the TLS stack can still use PSRAM.
  heap_caps_malloc_extmem_enable(1 << 20);
  for (int i = 0; i < 2; i++) {
    spr[i].setColorDepth(16);
    void* p = spr[i].createSprite(SCR_W, SCR_H);
    if (!p) {
      Serial.printf("FATAL: sprite %d alloc failed\n", i);
      while (true) delay(1000);
    }
    Serial.printf("sprite[%d] @ %p  %s\n", i, p,
                  esp_ptr_external_ram(p) ? "PSRAM (slow!)" : "internal SRAM");
  }
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);

  Serial.printf("free internal heap: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (netEnabled()) {
    // 20KB: an mbedTLS handshake plus a JPEG decode are both stack-hungry.
    xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
    Serial.println("[net] credentials present, polling Spotify");
  } else {
    Serial.println("[net] no src/secrets.h -- demo mode");
  }

  lyrics.parse(kDemoLrc);
  haveLyrics = true;

  Serial.println("BTN_L (GPIO0) cycle style/visualiser   BTN_R (GPIO47) visuals <-> lyrics");
  Serial.println("serial: m mode, n next viz, A auto viz, s status, o rotate");
}

// ------------------------------------------------------------------ loop
void loop() {
  static uint32_t fpsMark   = millis();
  static int      frames    = 0;
  static uint32_t drawAcc   = 0;
  static uint32_t demoStart = millis();

  if (clicked(btnL)) {
    if (gMode == MODE_VIZ) {
      gVizOverride = (gVizOverride + 1) % VIZ_COUNT;
      Serial.printf("[viz] %s (locked)\n", vizNameAt(gVizOverride));
      toast("%s", vizNameAt(gVizOverride));
    } else {
      // Re-roll the look of whatever line is on screen right now. Shifting the
      // seed changes scene AND typeface together, so every press is a visible
      // change. (Locking one style instead pinned every line to the same look,
      // which is a different thing and reads as nothing happening.)
      sceneSalt += 7;
      gStylePending = true;      // named in the toast once the frame has drawn
    }
  }
  if (clicked(btnR)) {
    gMode = (gMode == MODE_LYRICS) ? MODE_VIZ : MODE_LYRICS;
    Serial.printf("[mode] %s\n", gMode == MODE_VIZ ? "visuals" : "lyrics");
    toast("%s", gMode == MODE_VIZ ? "visuals" : "lyrics");
  }

  while (Serial.available()) {
    int c = Serial.read();
    if      (c == 'r') { sceneSalt += 7; lastLineIdx = -2; }
    else if (c == 's') showStatus = !showStatus;
    // 'm' toggles; 'V'/'L' set absolutely, so a test harness never has to
    // guess which state a toggle landed in.
    else if (c == 'm' || c == 'V' || c == 'L') {
      if      (c == 'V') gMode = MODE_VIZ;
      else if (c == 'L') gMode = MODE_LYRICS;
      else               gMode = (gMode == MODE_LYRICS) ? MODE_VIZ : MODE_LYRICS;
      Serial.printf("[mode] %s\n", gMode == MODE_VIZ ? "visuals" : "lyrics");
    }
    else if (c == 'n') {
      if (gMode == MODE_VIZ) {
        gVizOverride = (gVizOverride + 1) % VIZ_COUNT;
        Serial.printf("[viz] %s (locked)\n", vizNameAt(gVizOverride));
      } else { sceneSalt += 7; gStylePending = true; }
    }
    else if (c == 'A') { gVizOverride = -1; }
    else if (c == 'l') {
      // Force a lyrics re-fetch on the next poll by forgetting which track is
      // loaded. Makes the LRCLIB path testable without changing songs.
      loadedTrack[0] = 0;
      Serial.println("[lrclib] forcing re-fetch on next poll");
    }
    else if (c == 'v') netProbeAudioFeatures(np.trackId);
    else if (c == 'F') {
      // Dump the last completed frame as hex so it can be reconstructed and
      // inspected off-device -- the only way to check what the panel is
      // actually showing without eyes on it.
      TFT_eSprite& d = spr[cur ^ 1];
      uint16_t* fbp = (uint16_t*)d.getPointer();
      Serial.printf("[framedump] begin %d %d\n", SCR_W, SCR_H);
      for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) Serial.printf("%04X", fbp[y * SCR_W + x]);
        Serial.println();
      }
      Serial.println("[framedump] end");
    }
    else if (c == 'o') {
      rotation = (rotation + 1) & 3;
      tft.setRotation(rotation);
      Serial.printf("[rotation] %d  <- set SCREEN_ROTATION to this\n", rotation);
    }
  }

  TFT_eSprite& s = spr[cur];

  bool     live  = netEnabled() && np.valid;
  uint32_t posMs = live ? playbackMs() : ((millis() - demoStart) % 33000u);

  uint32_t dStart = micros();

  int idx = (haveLyrics && !lyrics.empty()) ? lyrics.indexAt(posMs) : -1;

  bool showLyric = (gMode == MODE_LYRICS) && idx >= 0 && lyrics.text(idx)[0];


  if (showLyric) {
    uint32_t startMs = lyrics.timeAt(idx);
    uint32_t nextMs  = (idx + 1 < lyrics.count()) ? lyrics.timeAt(idx + 1)
                                                  : startMs + 4000;
    uint32_t age  = posMs > startMs ? posMs - startMs : 0;
    uint32_t hold = nextMs > startMs ? nextMs - startMs : 3000;
    typeDraw(s, lyrics.text(idx), age, hold, (uint32_t)idx + sceneSalt);
  } else if (gMode == MODE_LYRICS) {
    // A short gap holds the last line, which reads as the singer pausing.
    // Only a prolonged one gives up and floats notes instead.
    int held = -1;
    for (int k = idx; k >= 0; k--)
      if (lyrics.text(k)[0]) { held = k; break; }

    uint32_t gapStart = (idx >= 0) ? lyrics.timeAt(idx) : 0;
    uint32_t gapAge   = posMs > gapStart ? posMs - gapStart : 0;

    if (!haveLyrics || held < 0 || gapAge > NOTES_AFTER_MS) {
      typeDrawNotes(s, live ? np.track : "", live ? np.artist : "",
                    (float)millis() * 0.001f, gTrackSeed);
    } else {
      // Redraw the last line fully settled: a big age skips the entry
      // animation so it sits still rather than replaying.
      uint32_t st = lyrics.timeAt(held);
      typeDraw(s, lyrics.text(held), 100000,
               gapStart > st ? gapStart - st : 3000,
               (uint32_t)held + sceneSalt);
    }
  } else if (live) {
    // Visuals mode: the visualiser with the seek bar and track details.
    int vi = (gVizOverride >= 0) ? gVizOverride : vizIndexForSeed(gTrackSeed);
    float prog = np.durationMs ? (float)posMs / (float)np.durationMs : 0.0f;
    vizDraw(s, vi, gTrackSeed, (float)millis() * 0.001f, prog,
            np.track, np.artist);
  } else {
    // Nothing playing at all -- no track, no progress to show.
    typeDrawCard(s, "", "", netStatus(), millis(), sceneSalt);
  }

  if (idx != lastLineIdx) {
    if (idx >= 0 && lyrics.text(idx)[0])
    {
      int fr; bool clipped;
      typeLastFit(fr, clipped);
      uint32_t sd = (uint32_t)idx + sceneSalt;
      // Report the style actually drawn, not the one the seed would have
      // picked -- otherwise a locked style is invisible in the log.
      const char* styleNm; const char* faceNm;
      typeLastDrawn(styleNm, faceNm);
      int ci, cn;
      typeLastChunk(ci, cn);
      Serial.printf("[line %d] %-8s %-8s %3d chars  %d rows  %d phrase%s%s\n",
                    idx, styleNm, faceNm,
                    (int)strlen(lyrics.text(idx)), fr, cn, cn == 1 ? "" : "s",
                    clipped ? "   *** CLIPPED ***" : "");
    }
    lastLineIdx = idx;
  }

  // Name the style that actually got drawn, once the frame has resolved it.
  if (gStylePending) {
    const char* sn; const char* fn;
    typeLastDrawn(sn, fn);
    Serial.printf("[style] %s / %s\n", sn, fn);
    toast("%s", sn);
    gStylePending = false;
  }

  if (gToast[0] && (int32_t)(millis() - gToastUntil) < 0) {
    s.setTextFont(1);
    s.setTextSize(1);
    s.setTextDatum(TC_DATUM);
    s.fillRect(0, 0, SCR_W, 11, INK_OFF);
    s.setTextColor(INK_ON);
    s.drawString(gToast, SCR_W / 2, 2);
  }

  if (showStatus) {
    s.setTextFont(1);
    s.setTextSize(1);
    s.setTextDatum(TL_DATUM);
    s.setTextColor(INK_ON, INK_OFF);
    char buf[40];
    snprintf(buf, sizeof(buf), "%d %s", fps, netStatus());
    s.drawString(buf, 2, 2);
  }

  drawAcc += micros() - dStart;

  tft.startWrite();
  s.pushSprite(0, 0);
  tft.endWrite();

  cur ^= 1;

  frames++;
  uint32_t ms = millis();
  if (ms - fpsMark >= 1000) {
    fps = frames;
    float drawMs = frames ? (float)drawAcc / (float)frames / 1000.0f : 0.0f;
    Serial.printf("%3d fps  draw %.2f ms  %s\n", fps, drawMs, netStatus());
    frames  = 0;
    drawAcc = 0;
    fpsMark = ms;
  }
}
