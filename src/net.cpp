#include "net.h"

#if HAVE_SECRETS

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Root CA bundle shipped with the Arduino core. If this symbol resolves we get
// real certificate validation with nothing to maintain; see applyTls().
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

static char     accessToken[256] = {0};
static uint32_t tokenExpiresAt   = 0;      // millis() deadline
static char     statusLine[48]   = "starting";

// PKCE means no client secret lives in this firmware. The trade is that
// Spotify rotates the refresh token on every refresh, so the current one has
// to survive reboots -- the value in secrets.h is only the first-boot seed.
static Preferences prefs;
static String      refreshToken;

String urlEncode(const char* s);

// A Spotify refresh token is a long opaque string beginning "AQ". Pasting the
// client ID or client secret here instead is an easy slip, and the only
// symptom is a bare HTTP 400 from the token endpoint -- so say so plainly.
static void warnIfNotRefreshToken(const String& t) {
  if (t.length() >= 80 && t.startsWith("AQ")) return;
  Serial.println("[spotify] WARNING: SPOTIFY_REFRESH_TOKEN does not look like a");
  Serial.printf ("          refresh token (length %u, expected ~130 starting \"AQ\").\n",
                 (unsigned)t.length());
  Serial.println("          A 32-char hex value is your client ID or secret, not");
  Serial.println("          the token. Re-run tools/spotify_auth.py and copy the");
  Serial.println("          SPOTIFY_REFRESH_TOKEN line it prints.");
}

bool netEnabled() { return true; }

static void setStatus(const char* s) {
  strncpy(statusLine, s, sizeof(statusLine) - 1);
  statusLine[sizeof(statusLine) - 1] = '\0';
}
const char* netStatus() { return statusLine; }

// Single place the TLS trust policy is decided. The access token and refresh
// token both cross these connections, so the chain is validated against the
// core's root bundle rather than accepted blindly.
static void applyTls(WiFiClientSecure& c) {
  c.setCACertBundle(rootca_crt_bundle_start);
  c.setTimeout(12000);
}

static void loadRefreshToken() {
  prefs.begin("spotify", false);
  // isKey() first: getString() on a missing key logs at ERROR level, which
  // looks alarming on a perfectly normal first boot.
  refreshToken = prefs.isKey("refresh") ? prefs.getString("refresh", "") : String();
  if (refreshToken.isEmpty()) {
    refreshToken = SPOTIFY_REFRESH_TOKEN;          // first boot: seed from secrets.h
    Serial.println("[spotify] seeded refresh token from secrets.h");
  } else {
    Serial.println("[spotify] refresh token loaded from NVS");
  }
  warnIfNotRefreshToken(refreshToken);
}

static void saveRefreshToken(const char* t) {
  if (!t || !*t || refreshToken == t) return;
  refreshToken = t;
  prefs.putString("refresh", refreshToken);
  Serial.println("[spotify] refresh token rotated, saved to NVS");
}

void netBegin() {
  loadRefreshToken();
  setStatus("wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // sleep adds latency to every poll
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool netConnected() { return WiFi.status() == WL_CONNECTED; }

// ------------------------------------------------------------------ token
static bool refreshAccessToken() {
  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    setStatus("token: begin failed");
    return false;
  }

  // PKCE refresh: client_id in the body, no Authorization header, no secret.
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" +
                urlEncode(refreshToken.c_str()) +
                "&client_id=" + String(SPOTIFY_CLIENT_ID);

  int code = http.POST(body);
  if (code != 200) {
    char buf[48];
    snprintf(buf, sizeof(buf), "token http %d", code);
    setStatus(buf);
    // Spotify explains itself in the body ("invalid_grant", "Refresh token
    // revoked", ...). Printing it turns a bare 400 into something actionable;
    // the response never echoes the token back.
    Serial.printf("[spotify] token endpoint %d: %s\n", code,
                  http.getString().c_str());
    // 400 usually means a rotation that never got saved. Fall back to the
    // seed -- but only if that is actually something different, otherwise we
    // would just churn NVS retrying the same dead value.
    if (code == 400 && refreshToken != SPOTIFY_REFRESH_TOKEN) {
      if (prefs.isKey("refresh")) prefs.remove("refresh");
      refreshToken = SPOTIFY_REFRESH_TOKEN;
      Serial.println("[spotify] stored token rejected, reverting to secrets.h seed");
      warnIfNotRefreshToken(refreshToken);
    }
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["access_token"]  = true;
  filter["expires_in"]    = true;
  filter["refresh_token"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setStatus("token json"); return false; }

  const char* tok = doc["access_token"];
  if (!tok) { setStatus("no token"); return false; }
  strncpy(accessToken, tok, sizeof(accessToken) - 1);
  accessToken[sizeof(accessToken) - 1] = '\0';

  // Persist the rotated token before it is needed, not after.
  saveRefreshToken(doc["refresh_token"] | (const char*)nullptr);

  uint32_t ttl = doc["expires_in"] | 3600;
  tokenExpiresAt = millis() + (ttl - 60) * 1000u;   // refresh a minute early
  return true;
}

// ------------------------------------------------------------------ polling
bool netPollSpotify(NowPlaying& out) {
  if (!netConnected()) { setStatus("wifi..."); return false; }

  if (accessToken[0] == '\0' || (int32_t)(millis() - tokenExpiresAt) >= 0) {
    if (!refreshAccessToken()) return false;
  }

  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) return false;

  char authHdr[300];
  snprintf(authHdr, sizeof(authHdr), "Bearer %s", accessToken);
  http.addHeader("Authorization", authHdr);

  int code = http.GET();

  if (code == 204) {                 // nothing playing
    http.end();
    out.valid = false;
    setStatus("nothing playing");
    return true;
  }
  if (code == 401) {                 // token died early
    http.end();
    accessToken[0] = '\0';
    return false;
  }
  if (code != 200) {
    char buf[48];
    snprintf(buf, sizeof(buf), "spotify http %d", code);
    setStatus(buf);
    http.end();
    return false;
  }

  // The full payload is several KB of album art URLs and market lists; the
  // filter keeps only these fields so the document stays small.
  JsonDocument filter;
  filter["progress_ms"]              = true;
  filter["is_playing"]               = true;
  filter["item"]["name"]             = true;
  filter["item"]["duration_ms"]      = true;
  filter["item"]["id"]               = true;
  filter["item"]["album"]["name"]    = true;
  filter["item"]["artists"][0]["name"] = true;
  // Spotify lists covers largest first (640/300/64). We want the one nearest
  // 300: big enough to downscale into a sharp 128px panel image, without
  // pulling a 640px JPEG over TLS every track change.
  filter["item"]["album"]["images"][0]["url"]    = true;
  filter["item"]["album"]["images"][0]["height"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setStatus("spotify json"); return false; }

  JsonObject item = doc["item"];
  if (item.isNull()) { out.valid = false; setStatus("no track"); return true; }

  strncpy(out.track,  item["name"] | "", sizeof(out.track) - 1);
  out.track[sizeof(out.track) - 1] = '\0';
  strncpy(out.album,  item["album"]["name"] | "", sizeof(out.album) - 1);
  out.album[sizeof(out.album) - 1] = '\0';
  strncpy(out.artist, item["artists"][0]["name"] | "", sizeof(out.artist) - 1);
  out.artist[sizeof(out.artist) - 1] = '\0';

  strncpy(out.trackId, item["id"] | "", sizeof(out.trackId) - 1);
  out.trackId[sizeof(out.trackId) - 1] = 0;

  out.artUrl[0] = 0;
  JsonArray imgs = item["album"]["images"];
  int bestDelta = 1 << 30;
  for (JsonObject im : imgs) {
    int h = im["height"] | 0;
    const char* u = im["url"];
    if (!u || !*u || h <= 0) continue;
    int delta = h > 300 ? h - 300 : 300 - h;
    if (delta < bestDelta) {
      bestDelta = delta;
      strncpy(out.artUrl, u, sizeof(out.artUrl) - 1);
      out.artUrl[sizeof(out.artUrl) - 1] = 0;
    }
  }

  out.progressMs = doc["progress_ms"] | 0;
  out.durationMs = item["duration_ms"] | 0;
  out.playing    = doc["is_playing"]  | false;
  out.valid      = true;
  setStatus("ok");
  return true;
}

int netProbeAudioFeatures(const char* trackId) {
  if (!netConnected() || !trackId || !*trackId) return -1;
  if (accessToken[0] == 0 && !refreshAccessToken()) return -2;

  String url = "https://api.spotify.com/v1/audio-features/" + String(trackId);
  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, url)) return -3;

  char authHdr[300];
  snprintf(authHdr, sizeof(authHdr), "Bearer %s", accessToken);
  http.addHeader("Authorization", authHdr);

  int code = http.GET();
  String body = http.getString();
  http.end();
  Serial.printf("[probe] audio-features -> %d : %s\n", code, body.c_str());
  return code;
}

// ------------------------------------------------------------------ lyrics
String urlEncode(const char* s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

#define LRCLIB_UA "tqt-lyrics (github.com/Darskye/tqt-lyrics)"

// LRCLIB replies with Transfer-Encoding: chunked and no Content-Length.
// HTTPClient::getStream() hands back the raw socket with the chunk-size
// markers still in it, so parsing from the stream feeds hex chunk headers to
// the JSON parser. getString() de-chunks, which is why every request here
// reads the body first and parses from that.
static bool lrclibTry(const String& url, String& body, const char* what) {
  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.printf("[lrclib] %s: begin failed\n", what);
    return false;
  }
  http.addHeader("User-Agent", LRCLIB_UA);

  int code = http.GET();
  if (code != 200) {
    // Negative codes are TLS or connection failures, not "no such track".
    Serial.printf("[lrclib] %s -> %d\n", what, code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["syncedLyrics"] = true;
  filter["instrumental"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[lrclib] %s json: %s\n", what, err.c_str());
    return false;
  }

  if (doc["instrumental"] | false) {
    Serial.printf("[lrclib] %s: marked instrumental\n", what);
    return false;
  }
  const char* synced = doc["syncedLyrics"];
  if (!synced || !*synced) {
    Serial.printf("[lrclib] %s: no synced lyrics in result\n", what);
    return false;
  }
  body = synced;
  Serial.printf("[lrclib] %s: matched\n", what);
  return true;
}

// LRCLIB matches on track, artist, album and duration together, and the album
// string is the fragile part: Spotify reports "The Slow Rush (CD)" or a
// deluxe/remaster edition where the lyrics were filed under the plain album,
// and the lookup misses. So widen the query in stages rather than give up --
// each response is only a few KB, unlike /api/search which returns ~150KB and
// will not fit in RAM.
bool netFetchLyrics(const NowPlaying& np, String& body) {
  if (!netConnected() || !np.valid) return false;
  body = "";

  const String base = "https://lrclib.net/api/get?track_name=" + urlEncode(np.track) +
                      "&artist_name=" + urlEncode(np.artist);
  const String dur  = "&duration=" + String(np.durationMs / 1000);

  // Most precise first: everything Spotify told us.
  if (np.album[0] &&
      lrclibTry(base + "&album_name=" + urlEncode(np.album) + dur, body, "exact"))
    return true;

  // Drop the album. This is the one that usually rescues it.
  if (lrclibTry(base + dur, body, "no-album")) return true;

  // Last resort: let LRCLIB pick the version. Risks matching a live or
  // extended cut, but a slightly wrong timing beats a blank panel.
  if (lrclibTry(base, body, "any-duration")) return true;

  return false;
}

#else   // ---------------------------------------------------- no credentials

bool  netEnabled()    { return false; }
void  netBegin()      {}
bool  netConnected()  { return false; }
const char* netStatus() { return "demo mode"; }
bool  netPollSpotify(NowPlaying& out) { (void)out; return false; }
bool  netFetchLyrics(const NowPlaying& np, String& body) { (void)np; (void)body; return false; }
int   netProbeAudioFeatures(const char* trackId) { (void)trackId; return -1; }

#endif
