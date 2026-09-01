# tqt-lyrics

Spotify lyrics as the whole picture, on a LilyGo T-QT Pro (ESP32-S3FN4R2,
0.85" 128x128). Stark monochrome kinetic typography — no background layer, no
colour. Every scene owns the entire frame.

Measured on hardware: **114–129 fps**, 0.27–1.31 ms of drawing per frame.

## The idea

At roughly 16 characters per row there is no room for a lyric sheet, so this
does not try to be one. It shows one line at a time and lets the *presentation*
carry it: scale, motion, inversion and layout instead of colour.

## Colour and type

Each line gets its own **colour** and its own **typeface**, drawn independently
from the line index so they change together but never in lockstep.

Colour comes from a curated set of twelve high-value inks rather than a random
RGB triple: arbitrary values land on muddy or too-dark colours often enough to
matter on a black field.

Five type families are in rotation -- **sans, serif, mono, oblique** and the
built-in **pixel** cell. Each is a ladder of faces from 24pt down, ending at the
6x8 GLCD cell. `layoutFit()` walks the ladder and takes the first rung where the
whole line fits, so a long line simply arrives in a smaller face instead of
being clipped. The last rung is accepted unconditionally as the floor: 21
columns by 16 rows holds ~330 characters, longer than any lyric line.

The free fonts come with TFT_eSPI itself -- `Fonts/GFXFF/gfxfont.h` is an
aggregator that includes the whole set. Do not `#include` individual font
headers on top of that: they carry no include guards and you get redefinition
errors.

## Visualisers

Twelve chaotic colour fields. Which one a song gets comes from an FNV-1a hash
of its Spotify track id, so every track has its own and keeps it.

| | | fps |
|---|---|---|
| spiral | phyllotaxis shoved off its curves by turbulence | 90 |
| vortex | a drain whose inflow noise breaks up every revolution | 86 |
| starfield | perspective stars drifting off-axis, each its own hue | 102 |
| tunnel | a corridor whose rings buckle instead of staying circular | 97 |
| rain | columns swaying on a noise field, hue shifting down their length | 110 |
| **clifford** | chaotic attractor, parameters drifting so it never recycles | 95 |
| **turbulence** | pure fractal noise, no geometry at all | 75 |
| ripple | interference from sources wandering on noise paths | 93 |
| **dejong** | the other classic chaotic map, lacier than Clifford | 90 |
| helix | two strands frayed by noise rather than drawn as a diagram | 102 |
| swarm | particles advected by a fractal flow field | 75 |
| bloom | ragged shockwaves from wandering centres | 97 |

### Why the earlier set looked repetitive

Everything was a closed-form sum of sines and circles. Those are *periodic* --
they repeat by definition, and no amount of parameter tuning fixes it. Two
things change that:

**Value noise with fbm.** Every field is now displaced by two octaves of noise,
so arms wander, rings buckle and columns sway instead of tracing exact curves.

**Chaotic attractors.** Clifford and De Jong are genuinely chaotic maps, not
merely irregular ones. Their parameters drift with time, so the structure keeps
folding into shapes it has not held before rather than cycling.

### Colour

Full spectrum, and most of the range comes from **additive RGB accumulation**:
particles are splatted with per-particle hue and the channels sum and saturate,
so overlaps mix into new colours by themselves. Assigning each particle a fixed
colour gives far less variety than letting them blend.

Hue is driven from radius, depth, noise value and time depending on the field,
so the palette drifts continuously rather than sitting still.

### lyricform

Particles drift on a noise flow field and assemble into **one word at a time**,
scattering and re-forming for each. Words share the line's time in proportion
to their length.

One word rather than the whole line, because a full line has to shrink to fit
128px and ends up clipped -- a single word can be set large and stay legible.

The word is rasterised offscreen into an 8bpp mask and its lit pixels become
particle targets, sampled evenly down to the particle count. Taking the first N
instead would fill only the top rows and leave the rest of the word unformed.

**A quarter of the field never settles.** Without strays the whole screen
freezes the instant a word forms, which kills any sense of a living particle
system. Converged particles also keep a small wander, so the word breathes
rather than turning into a frozen bitmap.

Colour while scattered, white once assembled: chaos gets to be colourful, but
text has to be legible. Rasterising happens on word change only, never per
frame.

### They do not react to the audio

There is no audio signal on this device. Spotify deprecated `/audio-features`
and `/audio-analysis` in November 2024; this app returns **403** from both,
verified with an on-device probe against the live API. The board has no
microphone. Motion comes from playback position and the per-track seed -- real,
but not audio. The seek bar, by contrast, is genuinely accurate.

Real reactivity needs an I2S microphone (an INMP441, a few dollars) on the
breakout pads feeding an FFT. No API will provide it.

## Scenes

The scene is picked deterministically from the line index, so it holds for that
line and changes on the next. Scenes that cannot fit the text fall back rather
than clip.

| scene | what it does |
|---|---|
| hero | the longest word set enormous, rest of the line small beneath |
| stack | words stacked and left-aligned, each sliding in after the last |
| invert | knocked out of a filled field |
| wipe | type present from frame one; a shutter retreats off it |
| scroll | long lines travel across as one big row, between two rules |
| type | typewriter reveal with a blinking block cursor |
| flash | slams in inverted, then settles |
| rule | heavy bars drive in from both edges, type held between |
| box | a card grows from the centre, type knocked out of it |
| split | line broken in two, halves arriving from opposite sides |
| zoom | grows into place, stepping up the ladder as it lands |
| wipeup | shutter retreating downward instead of across |
| glitch | rows tear sideways, with occasional bigger slips |
| shadow | a dim offset copy behind, closing in as the line lands |
| mirror | the block sits high with a dimmed echo beneath it |
| decode | characters settle out of noise, left to right |
| rowfade | rows arrive one at a time from alternating sides |
| stamp | lands oversized for a beat, then settles onto the fitted size |

Type is TFT_eSPI's built-in font 1 — a 6x8 cell — scaled by integer multiples.
That keeps it crisp at any size, which is what makes large blocky type look
deliberate rather than stretched.

**Long lines split into phrases rather than shrinking.** A line that will not
fit at a large face is broken at word boundaries into the fewest phrases that
each fit in at most 3 rows, shown in sequence across the line's own duration.
Two constraints matter and only one is obvious: capping the *height* is not
enough, because at a small face five rows still fit and the line never splits.
Capping rows per phrase is what actually forces big type.

Each phrase gets time in proportion to its length, which tracks the singing
better than dividing evenly. LRC carries per-line timestamps only -- there is
no word-level timing in what LRCLIB serves -- so within a line this is an
approximation however it is done.

**Nothing is ever clipped.** `layoutFit()` steps the size down until the whole
line fits its box, and `wrapText()` reports overflow rather than silently
dropping rows. The floor is size 1: 21 columns by 16 rows, ~330 characters,
comfortably longer than any lyric line. Scenes declare a preferred maximum size
and get a smaller one automatically — on real lyrics, a 6-character line takes
size 3 while a 61-character line drops to size 1 across 4 rows.

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

**Auth uses PKCE, so there is no client secret in this firmware.** A device
that only reads playback state has no business carrying a secret that could be
pulled out of its flash by anyone holding the board. The trade is that Spotify
rotates the refresh token on every refresh, so the firmware persists each new
one to NVS; the value in `secrets.h` is only a first-boot seed and going stale
is expected. If a stored token is ever rejected (HTTP 400) it is dropped and
the seed is retried, so the device recovers on its own.

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

## Three traps worth knowing

**LRCLIB replies chunked, so you cannot parse from the stream.** Its responses
carry `Transfer-Encoding: chunked` with no `Content-Length`, and
`HTTPClient::getStream()` hands back the raw socket with the chunk-size markers
still embedded — so a JSON parser reading that stream is fed hex chunk headers
and quietly produces nothing. `getString()` de-chunks; every LRCLIB request
here reads the body first and parses from that. Spotify sends `Content-Length`,
which is why only the lyrics half broke, and why it looked like a lookup
problem rather than a transport one.

Lookups also widen in stages, because the album string is the fragile part:
Spotify reports things like `The Slow Rush (CD)` where the lyrics were filed
under the plain album name. So it tries album+duration, then drops the album,
then drops the duration. Each response is a few KB — `/api/search` returns
~150KB and will not fit in RAM.

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
   `http://127.0.0.1:8888/callback` as a Redirect URI. Enable **Web API**.
   Copy the Client ID — the client secret is not needed and is not used.
2. `python tools/spotify_auth.py` — authorises in your browser and prints the
   two values to keep. Runs entirely on your machine; nothing is sent anywhere
   but Spotify, and nothing is written to disk.
3. `cp src/secrets.h.example src/secrets.h`, fill in WiFi plus the two Spotify
   values, reflash.

`src/secrets.h` is gitignored. Scopes requested are read-only
(`user-read-currently-playing`, `user-read-playback-state`), and with PKCE
there is no long-lived secret on the device at all.

## Controls

| input | action |
|---|---|
| GPIO 0 button | re-roll the current line's style (lyrics) / cycle visualiser (visuals) |
| GPIO 47 button | switch visuals <-> lyrics |

Lyrics mode shows lyrics and nothing else -- no visualiser underneath. A short
gap holds the last line, which reads as the singer pausing. Only a prolonged
one (over `NOTES_AFTER_MS`, 4s) gives up and floats music notes with the track
and artist beneath, which is also what a track with no synced lyrics gets.

**The note glyphs are drawn, not typed.** TFT_eSPI's built-in fonts and every
GFX free font are ASCII only, so U+2669..U+266F -- the music symbols -- are
simply not in them and would render as garbage. Drawing them from primitives
also means they scale and animate freely.
In lyrics mode the left button re-rolls the look of the line on screen right
now: it shifts the seed, which changes scene and typeface together, so every
press is a visible change. Measured: 8 presses gave 8 distinct scene/face
pairings.
| serial `m` | switch visuals <-> lyrics |
| serial `n` | next visualiser (visuals) / re-roll style (lyrics) |
| serial `V` / `L` | set visuals / lyrics mode absolutely |
| serial `A` | back to the per-track visualiser |
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
