# tqt-lyrics

Spotify lyrics as the whole picture, on a LilyGo T-QT Pro (ESP32-S3FN4R2,
0.85" 128x128). Stark monochrome kinetic typography — no background layer, no
colour. Every scene owns the entire frame.

Measured on hardware: **114–129 fps**, 0.27–1.31 ms of drawing per frame.

## The idea

At roughly 16 characters per row there is no room for a lyric sheet, so this
does not try to be one. It shows one line at a time and lets the *presentation*
carry it: scale, motion, inversion and layout instead of colour.

## Scenes

The scene is picked deterministically from the line index, so it holds for that
line and changes on the next. Scenes that cannot fit the text fall back rather
than clip.

| scene | what it does |
|---|---|
| hero | the longest word set enormous, rest of the line small beneath |
| stack | words stacked and left-aligned, each sliding in after the last |
| invert | black on white — the strongest contrast move monochrome has |
| wipe | type is present from frame one; a black shutter retreats off it |
| scroll | long lines travel across as one big row, between two rules |
| type | typewriter reveal with a blinking block cursor |
| flash | slams in inverted, then settles — reads as an accent |
| rule | heavy bars drive in from both edges, type held between them |
| box | a white card grows from the centre, type knocked out of it |
| split | line broken in two, halves arriving from opposite sides |

Type is TFT_eSPI's built-in font 1 — a 6x8 cell — scaled by integer multiples.
That keeps it crisp at any size, which is what makes large blocky type look
deliberate rather than stretched. `fitSize()` picks the largest multiple that
still fits the box.

## Lyrics

Two sources, because Spotify has no lyrics API — their in-app lyrics are
licensed from Musixmatch and are not exposed:

- **Spotify Web API** (`/v1/me/player/currently-playing`) for track, artist,
  album and playback position. Polled every 5s; position is interpolated
  locally in between so timing stays tight without hammering the API.
- **[LRCLIB](https://lrclib.net)** for synced LRC lyrics. Free, no API key, no
  account. Fetched once per track change.

Unofficial endpoints that scrape Spotify's internal lyrics service with a
session cookie are deliberately not used: they break their terms, and they break.

TLS validates against the Arduino core's root CA bundle
(`setCACertBundle(rootca_crt_bundle_start)`), not `setInsecure()` — a refresh
token crosses that connection.

Without `src/secrets.h` the firmware still builds and runs, showing a demo
sequence so every scene is visible.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3FN4R2, 4 MB flash + 2 MB PSRAM, 240 MHz |
| Panel | 128x128, GC9A01 driver with CGRAM offset (colstart 2, rowstart 1) |
| SPI | HSPI @ 40 MHz — MOSI 2, SCLK 3, CS 5, DC 6, RST 1 |
| Backlight | GPIO 10, **active low** |
| Buttons | GPIO 0 (left), GPIO 47 (right) — active low, internal pullup |
| Battery sense | GPIO 4 |

## Two traps worth knowing

**Sprites must be forced into internal SRAM.** This SDK sets
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, so a 32KB sprite goes to PSRAM by
default and costs real time every frame. TFT_eSPI's
`setAttribute(PSRAM_ENABLE, false)` does *not* fix it — it only chooses between
`ps_calloc` and `calloc`, and plain `calloc` lands externally too. The fix is
`heap_caps_malloc_extmem_enable()` around `createSprite()`, restored afterwards
so the TLS stack can still use PSRAM.

**Do not use TFT_eSPI's DMA path on the S3.** `initDMA()` calls
`spi_bus_initialize()`, handing SPI3 to the ESP-IDF driver while TFT_eSPI keeps
writing that same peripheral's registers directly. The first frame lands and
nothing after it does — and because the peripheral stays poisoned, a blocking
fallback fails too while `initDMA()` is in effect, which makes it look like a
rendering bug. Each frame gets its own `startWrite`/`endWrite` instead.

## Build

    python -m platformio run -t upload
    python -m platformio device monitor

## Connecting Spotify

1. Create an app at https://developer.spotify.com/dashboard and add
   `http://127.0.0.1:8888/callback` as a Redirect URI.
2. `python tools/spotify_auth.py` — authorises in your browser and prints a
   refresh token. Runs entirely on your machine; nothing is sent anywhere but
   Spotify, and the token is not written to disk.
3. `cp src/secrets.h.example src/secrets.h`, fill in WiFi plus the three
   Spotify values, reflash.

`src/secrets.h` is gitignored. Scopes requested are read-only
(`user-read-currently-playing`, `user-read-playback-state`).

## Controls

| input | action |
|---|---|
| GPIO 0 button | re-roll the scene for the current line |
| GPIO 47 button | toggle the status readout |
| serial `r` | re-roll scene |
| serial `s` | toggle status |
| serial `o` | cycle panel rotation (prints the value for `SCREEN_ROTATION`) |

The panel mounts upside down relative to TFT_eSPI's rotation 0, so
`SCREEN_ROTATION` in `src/main.cpp` defaults to **2**. If yours differs, press
`o` until it looks right and put that number in the define.

## Panel revisions

These boards ship with two panels needing different init sequences. The
vendored TFT_eSPI is set up for the **new** panel, confirmed correct on this
unit by a flat-fill test. For an older board:

    cp panels/GC9A01_Init.old_panel.h     lib/TFT_eSPI/TFT_Drivers/GC9A01_Init.h
    cp panels/GC9A01_Rotation.old_panel.h lib/TFT_eSPI/TFT_Drivers/GC9A01_Rotation.h

## Credits

`lib/TFT_eSPI/` is vendored verbatim from
[LilyGo's T-QT repo](https://github.com/Xinyuan-LilyGO/T-QT) (TFT_eSPI 2.5.43,
by Bodmer, originally derived from Adafruit_ILI9341 — see
`lib/TFT_eSPI/license.txt`). It is included rather than pulled as a dependency
because LilyGo patch the GC9A01 init sequence and ship
`Setup211_LilyGo_T_QT_Pro_S3.h`; stock upstream will not bring this panel up.

The LRC parser and Spotify/LRCLIB client began life in
[tqt-shaders](https://github.com/Darskye/tqt-shaders). Everything in `src/` is
original.
