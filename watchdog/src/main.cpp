// ESP32-B : watchdog. Lives at home and watches every other node via RTDB.
//
// Why a separate location matters: A sits at the pond. If the watchdog shared
// that site's power and network, a pond outage would take both down and no
// alert would ever fire. Watching from home means a pond failure is visible.
//
// What it does, every CHECK_INTERVAL:
//   1. GET /devices -- every node, not a hardcoded list, so adding node C
//      later needs no firmware change here
//   2. for each node other than itself, compare latest.ts against now;
//      a node is stale once it has missed STALE_MULTIPLE of its own
//      meta.interval (so the threshold follows A's cadence automatically)
//   3. write /alerts/<id>/{active,firedAt,lastSeen,reason} on a state change
//   4. publish its own /devices/watchdog/latest heartbeat, so a future node C
//      can watch this one in turn
//
// This device hung in the field after ~9 days: it answered ping but its HTTP
// and OTA ports were dead, so it had stopped watching and could not even be
// updated remotely. A hardware watchdog now reboots it if the main loop stalls
// past WDT_TIMEOUT_S -- a watchdog that cannot recover itself is not one.
//
// Discord is spoken to directly from this device -- both the alert and the
// acknowledgement. Re-notifying until someone acknowledges would normally want
// a server to receive Discord interactions, which an ESP32 cannot do; the way
// round it is to poll the reactions on our own message instead of being pushed
// them, so no always-on host is needed.
//
// It also configures its own WiFi over HTTP, because the boards get moved
// between sites whose networks are not known in advance and nobody on site can
// be asked to operate a tool. See the wifi section for the order and why.

#include <ArduinoOTA.h>
#include <Arduino.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <rom/rtc.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "pull_ota.h"
#include "secrets.h"

// ---- configuration ----------------------------------------------------
static const int LED_PIN = 48;  // onboard WS2812

static const char *DEVICE_ID = "watchdog";
static const char *DEVICE_NAME = "home-watchdog";
static const char *FW_VERSION = "b40-2026.09.03";
// Monotonic; RTDB /firmware/watchdog/version is compared against this to
// decide whether a pull-based update is due. Bump on every release.
static const uint32_t FW_VERSION_CODE = 40;

// Two timed samples of the same 8-byte raw flash read, one from a global
// constructor (before initArduino() runs psramInit()) and one from the top of
// setup() (after). The read crosses a 32-byte boundary, which is the exact
// shape the misread corrupts, so comparing the two says whether the corrupted
// SPI1 state exists from startup or appears when the failing PSRAM probe runs.
// Reads app1+0x1c: true content is the first segment header tail + app
// descriptor magic, and the wrapped misread substitutes the image magic.
static uint8_t rawSampleEarly[8], rawSampleSetup[8];
static esp_err_t rawSampleEarlyErr = ESP_FAIL, rawSampleSetupErr = ESP_FAIL;

static esp_err_t sampleCrossingRead(uint8_t out[8]) {
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!p) return ESP_ERR_NOT_FOUND;
  return esp_partition_read(p, 0x1c, out, 8);
}

__attribute__((constructor)) static void earlyFlashSample() {
  rawSampleEarlyErr = sampleCrossingRead(rawSampleEarly);
}

static const uint32_t CHECK_INTERVAL_MS = 60UL * 1000;

// Generous enough that a slow but working check never trips it: one check
// makes several blocking TLS calls, each with a 12s timeout. Anything past
// this is a genuine stall, not slowness.
static const uint32_t WDT_TIMEOUT_S = 120;

// The first poll runs a minute after boot, so power-cycling a board is
// itself a way to pull an update promptly -- useful when a relative can
// reach the plug but nothing can reach the board. Steady-state polling is
// half-hourly, which is frequent enough for firmware and keeps the
// request count negligible.
static const uint32_t FW_POLL_FIRST_MS = 60UL * 1000;
static const uint32_t FW_POLL_INTERVAL_MS = 30UL * 60 * 1000;

// A node counts as dead after missing this many of its own publish intervals.
// A publishes every 60s, so 5 gives the agreed 5-minute threshold while
// staying tolerant of one or two dropped uploads.
static const uint32_t STALE_MULTIPLE = 5;

// Used when a node's meta.interval is missing, so a malformed node still gets
// a sane threshold rather than being treated as permanently fine.
static const uint32_t DEFAULT_INTERVAL_S = 60;

// Full passes over the candidate networks before giving up and rebooting.
// Each pass is a 12s timeout per configured network plus the blink between
// them, so four passes is roughly three to five minutes -- long enough for
// someone to notice the purple LED, reach for a phone and switch the hotspot
// on, and short enough that a firmware that genuinely cannot connect still
// reaches the reboot that lets pull_ota roll it back.
static const uint32_t WIFI_BOOT_ATTEMPTS = 4;

// ---- state ------------------------------------------------------------
static WebServer server(80);
static String logBuf;
static const size_t LOG_MAX = 12000;

static uint32_t checksRun = 0, alertsRaised = 0, alertsCleared = 0;
static volatile bool otaInProgress = false;
static String lastError = "none";
static String lastSummary = "no check yet";
static bool anyAlertActive = false;

static void logLine(const char *fmt, ...) {
  char line[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  char stamp[24] = "";
  time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min,
             tm.tm_sec);
  }

  // Also to the console. The board keeps its log in RAM and serves it over
  // HTTP, which is fine until the failure being chased is a crash -- then the
  // buffer dies with the board and the only witness is the serial port, where
  // the panic handler prints its backtrace too.
  Serial.print(stamp);
  Serial.println(line);

  logBuf += stamp;
  logBuf += line;
  logBuf += '\n';
  if (logBuf.length() > LOG_MAX) {
    int cut = logBuf.indexOf('\n', logBuf.length() - LOG_MAX);
    logBuf.remove(0, cut < 0 ? logBuf.length() / 2 : cut + 1);
  }
}

// ---- led --------------------------------------------------------------
// Colour is the only diagnosis available with no console attached:
//   green = all nodes healthy      red = a node is stale (alert active)
//   blue  = connecting / OTA       yellow = RTDB unreachable
//   purple = no WiFi (will reboot)
enum StatusColor { OFF, GREEN, BLUE, RED, YELLOW, PURPLE };

static void setLed(StatusColor c) {
  uint8_t r = 0, g = 0, b = 0;
  switch (c) {
    case GREEN:  g = 40; break;
    case BLUE:   b = 40; break;
    case RED:    r = 40; break;
    case YELLOW: r = 40; g = 25; break;
    case PURPLE: r = 30; b = 30; break;
    case OFF:    break;
  }
  // neopixelWrite, not rgbLedWrite: the latter is Arduino-ESP32 3.x only.
  neopixelWrite(LED_PIN, r, g, b);
}

static void blinkColor(StatusColor c, int times, int onMs) {
  for (int i = 0; i < times; i++) {
    setLed(c);
    delay(onMs);
    setLed(OFF);
    delay(onMs);
  }
}

// ---- wifi -------------------------------------------------------------
//
// The board has to survive being moved to a pond whose network nobody here
// knows in advance, and the person on site cannot be asked to operate
// anything. So credentials live in NVS rather than in the build, and there are
// two ways to change them without a reflash and without being on site:
//
//   /wifi    sets the target network. Served as a form, so the person on site
//            fills in their own network's name and password -- they know it,
//            we do not.
//   /rescue  sets the rescue hotspot itself, for swapping to a different
//            phone later.
//
// Connection order at boot, first success wins:
//
//   1. NVS target        what /wifi last stored; the normal case
//   2. NVS target_prev   the target before that, so a mistyped SSID undoes
//                        itself instead of stranding the board
//   3. WIFI_NETWORKS     the known sites from secrets.h -- the Taipei bench, the
//                        family house, the pond
//   4. NVS rescue        the hand-opened phone hotspot
//
// Ordered by what each one costs to use, cheapest first. The known sites need
// nobody to do anything, so they come before the hotspot: the real recovery
// plan for a board that has stopped working is that someone unplugs it and
// carries it back to the family house, where step 3 picks it up and it can be
// fixed remotely at leisure. The hotspot only exists while a person is
// standing there holding it open, so it is the last thing tried -- and being
// last also means the 12s it costs is only ever spent once everything else has
// already failed, rather than on every boot for a network that is switched
// off.
struct WiFiNetwork {
  const char *ssid;
  const char *pass;
};
static const WiFiNetwork NETWORKS[] = WIFI_NETWORKS;
static const char *connectedSsid = "none";

// Set when the network we ended up on is the rescue hotspot, which is what
// makes the board announce itself on Discord and what the status page reports.
static bool onRescueNetwork = false;

// How long to wait for one network before moving on. Twelve seconds is what
// this board has always used and is enough for a slow DHCP.
static const uint32_t WIFI_ATTEMPT_MS = 12000;

// ---- credential storage ------------------------------------------------
//
// Namespace "wifi", separate from pull_ota's "fw", so erasing one cannot
// disturb the other.
//
//   ssid/pass            current target
//   pssid/ppass          previous target, kept for the automatic revert
//   rssid/rpass          rescue hotspot
//   pending              1 while a freshly-set target has not yet proven it
//                        can connect
static Preferences wifiPrefs;

struct Credentials {
  String ssid;
  String pass;
};

static Credentials nvsRead(const char *ssidKey, const char *passKey) {
  Credentials c;
  wifiPrefs.begin("wifi", true);
  c.ssid = wifiPrefs.getString(ssidKey, "");
  c.pass = wifiPrefs.getString(passKey, "");
  wifiPrefs.end();
  return c;
}

static Credentials storedTarget() { return nvsRead("ssid", "pass"); }
static Credentials storedPrevious() { return nvsRead("pssid", "ppass"); }

// The rescue hotspot falls back to the compiled-in default until /rescue has
// been used, so a board that has never been configured is still rescuable.
static Credentials storedRescue() {
  Credentials c = nvsRead("rssid", "rpass");
  if (!c.ssid.length()) {
    c.ssid = RESCUE_SSID;
    c.pass = RESCUE_PASS;
  }
  return c;
}

static bool targetPending() {
  wifiPrefs.begin("wifi", true);
  bool p = wifiPrefs.getBool("pending", false);
  wifiPrefs.end();
  return p;
}

static void clearPending() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.remove("pending");
  wifiPrefs.end();
}

// Stores a new target and marks it unproven. The outgoing target becomes the
// previous one -- that is the whole revert mechanism, so it must not be
// skipped even when the two are identical.
static void storeTarget(const String &ssid, const String &pass) {
  Credentials old = storedTarget();
  wifiPrefs.begin("wifi", false);
  if (old.ssid.length()) {
    wifiPrefs.putString("pssid", old.ssid);
    wifiPrefs.putString("ppass", old.pass);
  }
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.putBool("pending", true);
  wifiPrefs.end();
}

static void storeRescue(const String &ssid, const String &pass) {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("rssid", ssid);
  wifiPrefs.putString("rpass", pass);
  wifiPrefs.end();
}

// Promotes the previous target back to current. Called when a pending target
// failed to connect, so the board returns to the network it was last happy on
// instead of waiting for someone to notice.
static void revertTarget() {
  Credentials prev = storedPrevious();
  if (!prev.ssid.length()) return;
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", prev.ssid);
  wifiPrefs.putString("pass", prev.pass);
  wifiPrefs.remove("pssid");
  wifiPrefs.remove("ppass");
  wifiPrefs.remove("pending");
  wifiPrefs.end();
}

// ---- connecting ---------------------------------------------------------

// What this boot tried and how it went, so the status page and the Discord
// announcement can explain the situation without re-deriving it. RAM only.
static String wifiAttemptedSsid = "";
static bool wifiReverted = false;
// True only on the boot where a target that was still unproven connected for
// the first time -- the one boot worth announcing as a success.
static bool wifiJustConfirmed = false;
// True when the target was unreachable and one of the known sites answered
// instead: the board has been moved, or the pond network is down.
static bool onKnownSite = false;

static bool tryNetwork(const String &ssid, const String &pass) {
  if (!ssid.length()) return false;
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t deadline = millis() + WIFI_ATTEMPT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline)
    blinkColor(BLUE, 1, 250);
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  return false;
}

// connectedSsid points into this, so the winning name outlives the call.
static String connectedSsidStore;

static bool connectWiFi() {
  onRescueNetwork = false;
  onKnownSite = false;

  Credentials target = storedTarget();
  bool pending = targetPending();
  wifiAttemptedSsid = target.ssid;

  // 1. the stored target
  if (tryNetwork(target.ssid, target.pass)) {
    connectedSsidStore = target.ssid;
    connectedSsid = connectedSsidStore.c_str();
    if (pending) {
      clearPending();  // it works; stop calling it unproven
      wifiJustConfirmed = true;
    }
    return true;
  }

  // 2. the target before it. Only meaningful while the current one is still
  //    unproven -- once a target has connected at least once, a later failure
  //    is the network being down, not a bad credential, and silently moving
  //    back to an older network would hide that.
  if (pending) {
    Credentials prev = storedPrevious();
    if (tryNetwork(prev.ssid, prev.pass)) {
      revertTarget();
      wifiReverted = true;
      connectedSsidStore = prev.ssid;
      connectedSsid = connectedSsidStore.c_str();
      return true;
    }
  }

  // 3. the known sites. These need nobody to do anything, which is why they
  //    come before the hotspot: a board that has stopped working gets
  //    unplugged and carried back to the family house, and this is the step
  //    that catches it when it is plugged in there.
  for (const WiFiNetwork &n : NETWORKS) {
    if (tryNetwork(n.ssid, n.pass)) {
      if (pending) {
        revertTarget();
        wifiReverted = true;
      }
      // Only unexpected if a target was configured at all. A board that has
      // never been given one is supposed to be here.
      onKnownSite = target.ssid.length() > 0;
      connectedSsidStore = n.ssid;
      connectedSsid = connectedSsidStore.c_str();
      return true;
    }
  }

  // 4. the rescue hotspot, last because it is the only option that costs
  //    somebody something: it exists only while a person is holding it open.
  //    Worth trying for the case where the board cannot be moved, or moving it
  //    is not worth the trip.
  Credentials rescue = storedRescue();
  if (tryNetwork(rescue.ssid, rescue.pass)) {
    onRescueNetwork = true;
    // The pending target failed and nothing else worked either. Drop the
    // pending mark so the next boot does not re-run this same dance, but keep
    // what is stored: the form shows it, and it is the only record of what was
    // attempted.
    if (pending) {
      clearPending();
      wifiReverted = true;
    }
    connectedSsidStore = rescue.ssid;
    connectedSsid = connectedSsidStore.c_str();
    return true;
  }
  return false;
}

// ---- rtdb -------------------------------------------------------------

// Discord bodies here are hand-built JSON, and the Chinese in them is written
// as \uXXXX escapes -- so the message templates are already JSON source and
// must be passed through untouched. Only the values interpolated into them are
// data: an SSID is chosen by whoever owns the network and can perfectly well
// contain a quote or a backslash, either of which would end the string early
// and produce a body Discord rejects.
//
// Hence this is applied to the values, by the caller, and never to the
// template. Escaping the whole message would replace every backslash in every
// \u escape and the text would arrive as literal "u5931u6557".
static String jsonEscape(const String &in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\' || c == '\n' || c == '\r')
      o += ' ';
    else
      o += c;
  }
  return o;
}

static bool rtdbRequest(const char *method, const String &path,
                        const String &body, String *out) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);

  HTTPClient http;
  String url = String("https://") + RTDB_HOST + path;
  if (!http.begin(client, url)) {
    lastError = "http.begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(12000);

  int code = body.length() ? http.sendRequest(method, body)
                           : http.sendRequest(method);
  bool ok = (code == 200);
  if (ok && out) *out = http.getString();
  if (!ok) lastError = String(method) + " " + path + " -> " + String(code);
  http.end();
  return ok;
}

// Minimal field extraction. A full JSON parse of /devices would need more
// heap than the response is worth; these readings are flat numbers and short
// strings at a known depth, so a scan is enough and cannot fragment the heap.
// Same idea as extractNumber for the short string fields -- ssid and ip. No
// unescaping: these come from our own firmware, which already scrubs quotes
// and backslashes before publishing them.
static bool extractString(const String &json, int from, const char *key,
                          String *out) {
  String needle = String("\"") + key + "\":\"";
  int k = json.indexOf(needle, from);
  if (k < 0) return false;
  int v = k + needle.length();
  int e = json.indexOf('"', v);
  if (e < 0) return false;
  *out = json.substring(v, e);
  return true;
}

static bool extractNumber(const String &json, int from, const char *key,
                          double *out) {
  String needle = String("\"") + key + "\":";
  int k = json.indexOf(needle, from);
  if (k < 0) return false;
  int v = k + needle.length();
  int end = v;
  while (end < (int)json.length() &&
         (isdigit(json[end]) || json[end] == '-' || json[end] == '.'))
    end++;
  if (end == v) return false;
  *out = json.substring(v, end).toDouble();
  return true;
}

// ---- discord ----------------------------------------------------------
//
// Notification and acknowledgement both run from this device: it posts an
// alert message, then polls that message's reactions for a tick from the
// owner. Polling is what makes a 24/7 server unnecessary -- Discord
// interactions would have to be pushed to a listener, but reactions can be
// pulled on our own schedule.
//
// Cadence: re-notify every RENOTIFY_S while an alert is active and unacked.
// After acknowledgement, stay quiet for ACK_MUTE_S and then resume -- an
// acknowledged-but-unfixed pond is exactly the case that must not go silent.

static const uint32_t RENOTIFY_S = 5 * 60;
static const uint32_t ACK_MUTE_S = 60 * 60;
static const char *ACK_EMOJI = "%E2%9C%85";  // url-encoded white_check_mark

// Learned from /users/@me at boot rather than configured, so distinguishing
// the bot's own pre-seeded tick from a real acknowledgement needs no manual
// id lookup.
static String botUserId = "";

static bool discordRequest(const char *method, const String &path,
                           const String &body, String *out) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);

  HTTPClient http;
  if (!http.begin(client, "https://discord.com/api/v10" + path)) {
    lastError = "discord begin failed";
    return false;
  }
  http.addHeader("Authorization", String("Bot ") + DISCORD_BOT_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "pond-watchdog/1.0");
  http.setTimeout(12000);

  int code = body.length() ? http.sendRequest(method, body)
                           : http.sendRequest(method);
  bool ok = (code >= 200 && code < 300);
  if (out) *out = http.getString();
  if (!ok) lastError = "discord " + String(code) + " on " + path;
  http.end();
  return ok;
}

// Finds the message's own id in a Discord message object. Scans only at brace
// depth 1, so ids belonging to nested objects (the author, the mentions array)
// cannot be mistaken for the message id.
static String extractTopLevelId(const String &json) {
  int depth = 0;
  bool inStr = false, esc = false;
  for (int i = 0; i < (int)json.length(); i++) {
    char c = json[i];
    if (esc) { esc = false; continue; }
    if (c == '\\') { esc = true; continue; }
    if (c == '"') { inStr = !inStr; continue; }
    if (inStr) continue;

    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
    else if (depth == 1 && c == ':') {
      // Is the key immediately before this colon exactly "id"?
      int q2 = json.lastIndexOf('"', i);
      if (q2 <= 0) continue;
      int q1 = json.lastIndexOf('"', q2 - 1);
      if (q1 < 0) continue;
      if (json.substring(q1 + 1, q2) != "id") continue;

      int v = json.indexOf('"', i);
      if (v < 0) return "";
      int e = json.indexOf('"', v + 1);
      if (e < 0) return "";
      return json.substring(v + 1, e);
    }
  }
  return "";
}

// Formats a unix time as local HH:MM. Alerts are read on a phone, where an
// absolute clock time is easier to act on than an elapsed-seconds count.
static String localHhMm(time_t t) {
  if (t < 1600000000) return "\\u4e0d\\u660e";  // unknown
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

// Posts the alert and returns the message id, which is what later reaction
// polling needs. Also pre-seeds the tick so acknowledging is a single tap.
//
// Text is escaped as \uXXXX rather than literal UTF-8: the body is hand-built
// JSON, and raw multibyte characters there are easy to corrupt.
static String discordNotify(const String &nodeId, const String &reason,
                            uint32_t staleFor, time_t lastSeen,
                            const String &lastReading, int notifyCount) {
  // Renders as:
  //   @owner ⚠️ **pond-site 失聯**（第 2 次提醒）
  //   最後回報：01:45
  //   已中斷：6 分鐘
  //   最後讀值：26.3 °C
  //   原因：超過 6 分鐘未回報
  //   點 ✅ 回報已知悉（靜音 60 分鐘）
  String content = "<@" + String(DISCORD_USER_ID) + "> \\u26a0\\ufe0f **";
  content += nodeId + " \\u5931\\u806f**";
  if (notifyCount > 1)
    content += "\\uff08\\u7b2c " + String(notifyCount) + " \\u6b21\\u63d0\\u9192\\uff09";
  content += "\\n";
  content += "\\u6700\\u5f8c\\u56de\\u5831\\uff1a" + localHhMm(lastSeen) + "\\n";
  content += "\\u5df2\\u4e2d\\u65b7\\uff1a" + String(staleFor / 60) + " \\u5206\\u9418\\n";
  if (lastReading.length())
    content += "\\u6700\\u5f8c\\u8b80\\u503c\\uff1a" + lastReading + "\\n";
  content += "\\u539f\\u56e0\\uff1a" + reason + "\\n";
  content += "\\u9ede \\u2705 \\u56de\\u5831\\u5df2\\u77e5\\u6089\\uff08\\u975c\\u97f3 " +
             String(ACK_MUTE_S / 60) + " \\u5206\\u9418\\uff09";

  String body = "{\"content\":\"" + content + "\"}";
  String resp;
  if (!discordRequest("POST",
                      "/channels/" + String(DISCORD_CHANNEL_ID) + "/messages",
                      body, &resp)) {
    logLine("discord notify FAILED: %s", lastError.c_str());
    return "";
  }

  // The message id is the one at the top level of the response. A plain
  // search for the first "id" would instead find the id nested inside the
  // mentions array, so anchor on "channel_id" -- which only appears at the
  // top level -- and take the last top-level "id" before it.
  String msgId = extractTopLevelId(resp);

  discordRequest("PUT",
                 "/channels/" + String(DISCORD_CHANNEL_ID) + "/messages/" +
                     msgId + "/reactions/" + ACK_EMOJI + "/@me",
                 "", nullptr);
  return msgId;
}

// True once someone other than the bot has ticked the message.
static bool discordAcked(const String &msgId) {
  if (!msgId.length()) return false;

  String resp;
  if (!discordRequest("GET",
                      "/channels/" + String(DISCORD_CHANNEL_ID) +
                          "/messages/" + msgId + "/reactions/" + ACK_EMOJI,
                      "", &resp))
    return false;

  // The response lists users who reacted. Our own pre-seeded tick is always
  // there, so an ack means some *other* user id appears.
  int pos = 0;
  while (true) {
    int k = resp.indexOf("\"id\":\"", pos);
    if (k < 0) return false;
    int v = k + 6;
    int e = resp.indexOf('"', v);
    String uid = resp.substring(v, e);
    pos = e;
    if (uid != botUserId) return true;
  }
}

// ---- alerts -----------------------------------------------------------

// Only written on a transition. Rewriting an active alert every minute would
// reset nothing but would churn the bot's listener and hide when it started.
static bool writeAlert(const String &id, bool active, time_t lastSeen,
                       const String &reason) {
  time_t now = time(nullptr);
  String body = "{";
  body += "\"active\":" + String(active ? "true" : "false");
  body += ",\"lastSeen\":" + String((uint32_t)lastSeen);
  body += ",\"reason\":\"" + reason + "\"";
  body += ",\"by\":\"" + String(DEVICE_ID) + "\"";
  if (active) {
    body += ",\"firedAt\":" + String((uint32_t)now);
    // Cleared on every fresh alert: driveNotifications() sets it back once it
    // sees the reaction, so a new alert has to start from unacknowledged.
    body += ",\"acked\":false";
  } else {
    body += ",\"clearedAt\":" + String((uint32_t)now);
  }
  body += "}";

  return rtdbRequest("PATCH", "/alerts/" + id + ".json", body, nullptr);
}

// ---- notification state ----------------------------------------------
//
// Per-node so a second node (C) needs no extra code. Kept in RAM only: after
// a reboot the first check re-notifies, which is the safe direction to err.
struct NotifyState {
  String nodeId;
  String msgId;      // last alert message, polled for the ack reaction
  time_t lastSent;   // when we last posted
  time_t ackedAt;    // when the owner ticked it, 0 if never
  int sentCount;     // shown in the message so repeats are distinguishable
  bool alertActive;  // mirrors /alerts/<id>/active, so no refetch per check
  bool inUse;
  // The network the node last reported being on, so a move is noticed once
  // rather than announced every minute for as long as it stays there.
  String lastSsid;
  bool announcedMove;
  time_t lastTry;    // when we last attempted a post, success or not
  uint32_t failures; // consecutive failed posts, drives the backoff

  void reset() {
    msgId = "";
    lastSent = 0;
    ackedAt = 0;
    sentCount = 0;
    lastTry = 0;
    failures = 0;
    lastSsid = "";
    announcedMove = false;
  }
};

static const int MAX_NODES = 6;
static NotifyState notifyStates[MAX_NODES];

static NotifyState &notifyState(const String &id) {
  for (int i = 0; i < MAX_NODES; i++)
    if (notifyStates[i].inUse && notifyStates[i].nodeId == id)
      return notifyStates[i];
  for (int i = 0; i < MAX_NODES; i++)
    if (!notifyStates[i].inUse) {
      notifyStates[i].inUse = true;
      notifyStates[i].nodeId = id;
      notifyStates[i].reset();
      return notifyStates[i];
    }
  return notifyStates[0];  // full: reuse the first rather than overflow
}

static uint32_t notifySent = 0, notifyAcked = 0;

// Decides whether to post now. Called on every check while a node is stale.
// `lastSeen` and `lastReading` come from the node's own latest/ record, so the
// alert says when contact was lost and what the last value was.
static void driveNotifications(const String &id, uint32_t staleFor,
                               time_t lastSeen, const String &lastReading) {
  NotifyState &st = notifyState(id);
  time_t now = time(nullptr);

  // Poll for an acknowledgement on the message we last sent.
  if (st.msgId.length() && !st.ackedAt && discordAcked(st.msgId)) {
    st.ackedAt = now;
    notifyAcked++;
    logLine("%s acknowledged, muting %lu min", id.c_str(),
            (unsigned long)(ACK_MUTE_S / 60));
    // Mark the RTDB alert too, so a dashboard can show it was seen.
    rtdbRequest("PATCH", "/alerts/" + id + ".json",
                "{\"acked\":true,\"ackedAt\":" + String((uint32_t)now) + "}",
                nullptr);
  }

  // An acknowledged alert stays quiet only for the mute window: a node that
  // is still dead an hour later has to speak up again.
  if (st.ackedAt) {
    if ((uint32_t)(now - st.ackedAt) < ACK_MUTE_S) return;
    logLine("%s mute expired, resuming alerts", id.c_str());
    st.ackedAt = 0;
    st.lastSent = 0;
  }

  if (st.lastSent && (uint32_t)(now - st.lastSent) < RENOTIFY_S) return;

  // Back off after failures. RENOTIFY_S only paces *successful* posts, because
  // lastSent is set on success -- so while Discord is unreachable (an outage,
  // a bad token, a deleted channel) every cycle retried, once a minute, each
  // one a failing TLS round-trip that slows the whole loop down. That is worst
  // exactly when a node is down and an update might be needed. Double the wait
  // per consecutive failure, capped.
  if (st.failures) {
    uint32_t wait = RENOTIFY_S << (st.failures - 1 < 4 ? st.failures - 1 : 4);
    if ((uint32_t)(now - st.lastTry) < wait) return;
  }
  st.lastTry = now;

  // "超過 %d 分鐘未回報" -- exceeded N minutes without reporting
  String reason = "\\u8d85\\u904e " + String(staleFor / 60) +
                  " \\u5206\\u9418\\u672a\\u56de\\u5831";
  String msgId =
      discordNotify(id, reason, staleFor, lastSeen, lastReading,
                    st.sentCount + 1);
  if (msgId.length()) {
    st.msgId = msgId;
    st.lastSent = now;
    st.sentCount++;
    st.failures = 0;
    notifySent++;
    logLine("discord notified for %s (msg %s, #%d)", id.c_str(),
            msgId.c_str(), st.sentCount);
  } else {
    st.failures++;
    logLine("discord notify FAILED for %s (#%lu), next try in %lus",
            id.c_str(), (unsigned long)st.failures,
            (unsigned long)(RENOTIFY_S
                            << (st.failures - 1 < 4 ? st.failures - 1 : 4)));
  }
}

// Defined further down with the rest of the Discord helpers; needed here
// because the check loop runs before them in the file.
static bool discordSay(const String &content);

// Speaks for the other nodes, which have no Discord credentials of their own.
// Reports a node that is alive but on a network other than the one it usually
// publishes from, and carries the URL of its config form so whoever moved it
// can point it at a new network by tapping the link.
//
// Announced once per move, not once per check: the interesting event is the
// change, and a node can sit on the family house WiFi for days while it is
// being worked on.
static void announceNodeMove(const String &id, const String &obj,
                             bool isStale) {
  // A stale node's last report is history, not where it is now.
  if (isStale) return;

  String ssid, ip;
  if (!extractString(obj, 0, "ssid", &ssid) || !ssid.length()) return;

  NotifyState &ns = notifyState(id);

  // First sighting: adopt whatever it says without announcing. Otherwise every
  // reboot of this device would re-announce every node.
  if (!ns.lastSsid.length()) {
    ns.lastSsid = ssid;
    return;
  }
  if (ssid == ns.lastSsid) return;

  String from = ns.lastSsid;
  ns.lastSsid = ssid;

  extractString(obj, 0, "ip", &ip);

  // 節點換了網路
  String m = "\\u2139\\ufe0f **" + jsonEscape(id) +
             " \\u63db\\u4e86\\u7db2\\u8def**\n";
  m += "\\u539f\\u672c\\uff1a" + jsonEscape(from) + "\n";
  m += "\\u73fe\\u5728\\uff1a" + jsonEscape(ssid) + "\n";
  if (ip.length()) {
    m += "\n\\u8981\\u6539\\u5b83\\u7684 WiFi \\u8acb\\u9ede\\uff1a\n";
    m += "http://" + jsonEscape(ip) + "/wifi";
  }
  if (discordSay(m))
    logLine("%s moved %s -> %s, announced", id.c_str(), from.c_str(),
            ssid.c_str());
}

// ---- the check --------------------------------------------------------
static void runCheck() {
  checksRun++;

  String json;
  if (!rtdbRequest("GET", "/devices.json", "", &json)) {
    logLine("RTDB unreachable: %s", lastError.c_str());
    setLed(YELLOW);
    return;
  }

  time_t now = time(nullptr);
  int stale = 0, healthy = 0;
  String summary;

  // Walk the top-level device ids. Each looks like  "pond-site":{...}
  int pos = 1;  // skip the opening brace
  while (true) {
    int q1 = json.indexOf('"', pos);
    if (q1 < 0) break;
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 < 0) break;
    String id = json.substring(q1 + 1, q2);

    // The device's own object ends where the next top-level id begins; using
    // the whole remainder as the search window is fine because the fields we
    // read appear before it.
    int objStart = json.indexOf('{', q2);
    if (objStart < 0) break;

    // Find the matching close brace to bound this device's fields.
    int depth = 0, objEnd = objStart;
    for (int i = objStart; i < (int)json.length(); i++) {
      if (json[i] == '{') depth++;
      else if (json[i] == '}') { depth--; if (!depth) { objEnd = i; break; } }
    }
    String obj = json.substring(objStart, objEnd + 1);
    pos = objEnd + 1;

    if (id == DEVICE_ID) continue;  // don't watch ourselves

    double ts = 0, interval = DEFAULT_INTERVAL_S;
    if (!extractNumber(obj, 0, "ts", &ts)) {
      logLine("%s: no latest.ts -- treating as stale", id.c_str());
      ts = 0;
    }
    extractNumber(obj, 0, "interval", &interval);
    if (interval < 10) interval = DEFAULT_INTERVAL_S;

    uint32_t age = (now > (time_t)ts) ? (uint32_t)(now - (time_t)ts) : 0;
    uint32_t limit = (uint32_t)interval * STALE_MULTIPLE;
    bool isStale = age > limit;

    summary += id + "=" + String(age) + "s" + (isStale ? "(STALE) " : "(ok) ");

    // A node that is publishing happily but from an unexpected network has not
    // failed -- it has been carried somewhere, which is the recovery plan for
    // a board that stopped working. Nobody would otherwise be told: it is not
    // stale, so no alert fires, and only this device has both the ssid the
    // node reports and a way to say anything on Discord. The other nodes have
    // no Discord credentials at all, which is why this is announced on their
    // behalf rather than by them.
    announceNodeMove(id, obj, isStale);

    // Transition detection uses state this device already holds, rather than
    // re-fetching /alerts/<id>/active every minute: each such fetch was a full
    // TLS handshake (tens of KB of heap) to re-read a boolean we wrote
    // ourselves. Cutting them removes most of the per-check allocation churn,
    // which is the suspected cause of the hang this firmware recovers from.
    // Cost of keeping it local: after a reboot the first check re-raises an
    // alert that was already active, which errs toward notifying.
    NotifyState &ns = notifyState(id);
    bool wasActive = ns.alertActive;

    if (isStale && !wasActive) {
      String reason = "no publish for " + String(age) + "s (limit " +
                      String(limit) + "s)";
      if (writeAlert(id, true, (time_t)ts, reason)) {
        alertsRaised++;
        ns.alertActive = true;
        logLine("ALERT RAISED %s: %s", id.c_str(), reason.c_str());
      } else {
        logLine("failed to raise alert for %s: %s", id.c_str(),
                lastError.c_str());
      }
      ns.reset();
    } else if (!isStale && wasActive) {
      if (writeAlert(id, false, (time_t)ts, "publishing again")) {
        alertsCleared++;
        ns.alertActive = false;
        logLine("alert cleared %s (age %lus)", id.c_str(),
                (unsigned long)age);
      }
      ns.reset();
    }

    if (isStale) {
      // Surface whatever the node last measured. Read from meta.sensors so a
      // node with different sensors needs no change here.
      String reading;
      double temp;
      if (extractNumber(obj, 0, "temp", &temp))
        reading = String(temp, 1) + " \\u00b0C";
      driveNotifications(id, age, (time_t)ts, reading);
    }

    if (isStale) stale++; else healthy++;
  }

  anyAlertActive = (stale > 0);
  lastSummary = summary.length() ? summary : "no other devices found";
  logLine("check: %d ok, %d stale | heap=%u blk=%u | %s", healthy, stale,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
          lastSummary.c_str());
}

// Its own heartbeat, so a future node C can watch this watchdog in turn.
static bool publishSelf() {
  time_t now = time(nullptr);
  String body = "{";
  body += "\"devices/" + String(DEVICE_ID) + "/latest\":{";
  body += "\"ts\":" + String((uint32_t)now);
  body += ",\"checks\":" + String(checksRun);
  body += ",\"alerting\":" + String(anyAlertActive ? "true" : "false");
  // Where the board actually is. Both are needed from 300km away: the SSID
  // says whether it is still at the pond or has been carried somewhere, and
  // the IP is the only way to reach its config page, which is on a private
  // network that cannot be scanned from here.
  body += ",\"ssid\":\"" + jsonEscape(String(connectedSsid)) + "\"";
  body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  body += "}";
  body += "}";
  return rtdbRequest("PATCH", "/.json", body, nullptr);
}

static bool publishMeta() {
  String body = "{";
  body += "\"name\":\"" + String(DEVICE_NAME) + "\"";
  body += ",\"scope\":\"watchdog\"";
  body += ",\"role\":\"watchdog\"";
  body += ",\"fw\":\"" + String(FW_VERSION) + "\"";
  body += ",\"interval\":" + String(CHECK_INTERVAL_MS / 1000);
  body += ",\"staleMultiple\":" + String(STALE_MULTIPLE);
  body += ",\"bootAt\":" + String((uint32_t)time(nullptr));
  body += "}";
  return rtdbRequest("PUT", "/devices/" + String(DEVICE_ID) + "/meta.json",
                     body, nullptr);
}

// Names the cause of the most recent boot -- the first thing to look at when a
// board has been rebooting on its own. The distinction that matters here:
// RTC_SW_* is our own ESP.restart(), *WDT_* is a hang, BROWNOUT is the supply
// sagging (a thin USB cable does this), and PANIC is a crash.
// Enum names are the ESP32-S3 set; the S3 has no plain SW_CPU_RESET.
static const char *resetReasonName() {
  switch (rtc_get_reset_reason(0)) {
    case POWERON_RESET:          return "POWERON (power applied)";
    case RTC_SW_CPU_RESET:       return "SW_CPU (our ESP.restart)";
    case RTC_SW_SYS_RESET:       return "SW_SYS (our ESP.restart)";
    case DEEPSLEEP_RESET:        return "DEEPSLEEP";
    case TG0WDT_SYS_RESET:       return "TASK_WDT0 (hang)";
    case TG1WDT_SYS_RESET:       return "TASK_WDT1 (hang)";
    case TG0WDT_CPU_RESET:       return "TASK_WDT0_CPU (hang)";
    case TG1WDT_CPU_RESET:       return "TASK_WDT1_CPU (hang)";
    case RTCWDT_SYS_RESET:       return "RTC_WDT (hang)";
    case RTCWDT_CPU_RESET:       return "RTC_WDT_CPU (hang)";
    case RTCWDT_RTC_RESET:       return "RTC_WDT_RTC (hang)";
    case SUPER_WDT_RESET:        return "SUPER_WDT (hang)";
    case RTCWDT_BROWN_OUT_RESET: return "BROWNOUT (power dip)";
    case POWER_GLITCH_RESET:     return "POWER_GLITCH (power dip)";
    case USB_UART_CHIP_RESET:    return "USB_UART (host reset)";
    case USB_JTAG_CHIP_RESET:    return "USB_JTAG (host reset)";
    case EFUSE_RESET:            return "EFUSE";
    case GLITCH_RTC_RESET:       return "GLITCH";
    case INTRUSION_RESET:        return "INTRUSION";
    default:                     return "OTHER/PANIC (crash)";
  }
}

// ---- wifi configuration over http --------------------------------------
//
// Two ways in, one handler each. Both accept the credentials as query
// parameters and both serve a form when called without them, because the two
// callers are completely different people:
//
//   the operator  types a URL, or scripts it, and knows the OTA password
//   the person
//   on site       taps a link out of Discord and fills in two boxes
//
// The form is what makes the second case possible at all. The new network's
// name and password are known on site and nowhere else, so asking for them
// there is not a fallback -- it is the only place the answer exists.

// Percent-decoding is done by WebServer::arg(), so the values arrive usable.
// What still has to be escaped is the way back out: into HTML for the form,
// and into JSON for Discord.
static String htmlEscape(const String &in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

// Posts a one-shot message. Deliberately mentions nobody: a mention is what
// makes a phone buzz, and these messages report where the board ended up, not
// that something needs doing. The board only lands on an unexpected network
// because a person carried it somewhere and plugged it in -- they are already
// looking for it, so buzzing them adds nothing, and buzzing everyone for every
// reboot is how a channel gets muted.
//
// The alert path is the opposite case and keeps its mention: nobody is
// watching when a pond node goes quiet at 3am. See discordNotify().
//
// `content` is JSON string source, not plain text: callers build it with
// \uXXXX escapes and run jsonEscape() over anything interpolated.
static bool discordSay(const String &content) {
  String body = "{\"content\":\"" + content + "\"}";
  return discordRequest("POST",
                        "/channels/" + String(DISCORD_CHANNEL_ID) + "/messages",
                        body, nullptr);
}

// Announces the outcome of the last boot's WiFi decision. Called once, after
// the network and the clock are up.
//
// The rescue case is the one that matters: the board is reachable only for as
// long as someone holds a hotspot open, so the message carries the URL of the
// form rather than describing where to find it. Tapping it in Discord is the
// entire procedure.
static void announceWiFiState() {
  String ip = WiFi.localIP().toString();

  // Landing anywhere other than the configured target is worth saying out
  // loud, whichever network it turned out to be. Either somebody moved the
  // board -- which is the recovery plan working, and they are waiting for this
  // message to tell them where it went -- or the pond network is down, which
  // needs to be known. Both carry the setup URL, because whoever reads this is
  // the person who can act on it, and tapping the link is the whole procedure.
  if (onKnownSite) {
    String m = "\\u2139\\ufe0f **" + String(DEVICE_ID) +
               " \\u4e0d\\u5728\\u76ee\\u6a19\\u7db2\\u8def\\u4e0a**\n";
    if (wifiAttemptedSsid.length())
      m += "\\u76ee\\u6a19\\uff1a" + jsonEscape(wifiAttemptedSsid) +
           "\\uff08\\u9023\\u4e0d\\u4e0a\\uff09\n";
    m += "\\u76ee\\u524d\\u9023\\u4e0a\\uff1a" +
         jsonEscape(String(connectedSsid)) + "\n\n";
    m += "\\u8981\\u6539\\u76ee\\u6a19 WiFi \\u8acb\\u9ede\\uff1a\n";
    m += "http://" + ip + "/wifi";
    discordSay(m);
    return;
  }

  if (onRescueNetwork) {
    // 連不上目標網路，已連上救援熱點
    String m = "\\u26a0\\ufe0f **";
    m += String(DEVICE_ID) + " \\u9023\\u4e0d\\u4e0a\\u76ee\\u6a19\\u7db2\\u8def**\\n";
    if (wifiAttemptedSsid.length())
      m += "\\u5617\\u8a66\\u9023\\u7dda\\uff1a" + jsonEscape(wifiAttemptedSsid) +
           "\\uff08\\u5931\\u6557\\uff09\\n";
    m += "\\u76ee\\u524d\\u5728\\u6551\\u63f4\\u71b1\\u9ede\\uff1a" +
         jsonEscape(String(connectedSsid)) + "\\n\\n";
    m += "\\u8acb\\u9ede\\u4e0b\\u9762\\u9023\\u7d50\\u8a2d\\u5b9a\\u65b0\\u7684 WiFi\\uff1a\\n";
    m += "http://" + ip + "/wifi";
    discordSay(m);
    return;
  }

  if (wifiReverted) {
    // 新設定連不上，已自動退回
    String m = "\\u26a0\\ufe0f **" + String(DEVICE_ID) + " WiFi \\u8a2d\\u5b9a\\u5931\\u6557**\\n";
    if (wifiAttemptedSsid.length())
      m += "\\u9023\\u4e0d\\u4e0a\\uff1a" + jsonEscape(wifiAttemptedSsid) + "\\n";
    m += "\\u5df2\\u81ea\\u52d5\\u9000\\u56de\\uff1a" +
         jsonEscape(String(connectedSsid)) + "\\n";
    m += "IP\\uff1a" + ip;
    discordSay(m);
    return;
  }

  // A normal boot says nothing. The board reports for duty every minute
  // already; an extra message per reboot would only train the owner to ignore
  // this channel. Only a *newly confirmed* target is worth one.
  if (wifiJustConfirmed) {
    // WiFi 已更新
    String m = "\\u2705 **" + String(DEVICE_ID) + " WiFi \\u5df2\\u66f4\\u65b0**\\n";
    m += "\\u5df2\\u9023\\u4e0a\\uff1a" + jsonEscape(String(connectedSsid)) + "\\n";
    m += "IP\\uff1a" + ip;
    discordSay(m);
  }
}

// The form. Deliberately one self-contained string with no external CSS, no
// JavaScript and no favicon request: it is served to a phone over a hotspot
// with no internet route, so anything it cannot fetch locally would simply
// hang. utf-8 and the viewport line are both load-bearing -- without them the
// Chinese renders as boxes and the inputs come out too small to tap.
static String configFormPage(const char *action, const char *heading,
                             const String &currentSsid,
                             const String &statusLine) {
  String h = F("<!doctype html><html><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>ESP32 WiFi</title><style>"
               "body{font-family:system-ui,sans-serif;margin:0;padding:24px;"
               "background:#f4f4f6;color:#111}"
               ".c{max-width:420px;margin:0 auto;background:#fff;padding:24px;"
               "border-radius:12px;box-shadow:0 1px 4px rgba(0,0,0,.12)}"
               "h2{margin:0 0 16px;font-size:22px}"
               ".s{background:#f0f0f4;padding:12px;border-radius:8px;"
               "font-size:15px;line-height:1.7;margin-bottom:20px;color:#444}"
               "label{display:block;margin:14px 0 6px;font-size:16px}"
               "input{width:100%;box-sizing:border-box;padding:13px;"
               "font-size:17px;border:1px solid #ccc;border-radius:8px}"
               "button{width:100%;margin-top:22px;padding:16px;font-size:18px;"
               "border:0;border-radius:8px;background:#2563eb;color:#fff}"
               "</style></head><body><div class=\"c\">");
  h += "<h2>";
  h += heading;
  h += "</h2>";
  h += "<div class=\"s\">" + statusLine + "</div>";
  h += "<form method=\"get\" action=\"";
  h += action;
  h += "\">";
  // The password rides along hidden so the person on site never sees it and
  // never has to be told it.
  h += "<input type=\"hidden\" name=\"pass\" value=\"" +
       htmlEscape(String(OTA_PASSWORD)) + "\">";
  // 網路名稱 (SSID)
  h += F("<label>網路名稱 (SSID)</label>");
  h += "<input name=\"ssid\" value=\"" + htmlEscape(currentSsid) +
       "\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">";
  // 密碼
  h += F("<label>密碼</label>");
  h += F("<input name=\"wpass\" autocapitalize=\"off\" autocorrect=\"off\" "
         "spellcheck=\"false\">");
  // 儲存並重新啟動
  h += F("<button>儲存並重新啟動</button>");
  h += F("</form></div></body></html>");
  return h;
}

// Shown after a successful save. The board reboots a moment later, so this
// page has to say what to expect without being able to report it: the LED is
// the only signal available to someone standing next to it.
static String savedPage(const String &ssid) {
  String h = F("<!doctype html><html><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>ESP32 WiFi</title><style>"
               "body{font-family:system-ui,sans-serif;margin:0;padding:24px;"
               "background:#f4f4f6;color:#111}"
               ".c{max-width:420px;margin:0 auto;background:#fff;padding:24px;"
               "border-radius:12px;box-shadow:0 1px 4px rgba(0,0,0,.12)}"
               "h2{margin:0 0 14px;font-size:22px}"
               "p{font-size:16px;line-height:1.8;margin:10px 0}"
               "</style></head><body><div class=\"c\">");
  // 已儲存
  h += F("<h2>✅ 已儲存</h2>");
  h += "<p>";
  // 設定為：
  h += F("設定為：");
  h += "<b>" + htmlEscape(ssid) + "</b></p>";
  // 板子正在重新啟動，約 30 秒。
  h += F("<p>板子正在重新啟動，約 30 秒。</p>");
  // 如果紫燈停止閃爍，代表連線成功。
  h += F("<p>如果紫燈停止閃爍，"
         "代表連線成功。</p>");
  // 如果紫燈繼續閃，請保持熱點開啟並回報。
  h += F("<p>如果紫燈繼續閃，"
         "請保持熱點開啟並回報。</p>");
  h += F("</div></body></html>");
  return h;
}

// Shared by both handlers: the OTA password gates the write, so a stray
// request on the pond's own LAN cannot reconfigure the board out from under
// itself. Not a secrecy measure -- it is there to stop an accident.
static bool configAuthorised() {
  return server.arg("pass") == OTA_PASSWORD;
}

static void handleWifiConfig() {
  if (!configAuthorised()) {
    server.send(403, "text/plain; charset=utf-8", "bad pass");
    return;
  }

  String ssid = server.arg("ssid");
  if (!ssid.length()) {
    Credentials cur = storedTarget();
    String status;
    if (onRescueNetwork) {
      // 目前在救援熱點，請填入現場的 WiFi。
      status = F("目前在救援熱點，"
                 "請填入現場的 WiFi。");
      if (wifiAttemptedSsid.length()) {
        // 上次嘗試：
        status += F("<br>上次嘗試：");
        status += htmlEscape(wifiAttemptedSsid);
        // （失敗）
        status += F("（失敗）");
      }
    } else {
      // 目前連線：
      status = F("目前連線：");
      status += htmlEscape(String(connectedSsid));
    }
    // 設定目標 WiFi
    server.send(200, "text/html; charset=utf-8",
                configFormPage("/wifi",
                               "設定目標 WiFi", cur.ssid,
                               status));
    return;
  }

  storeTarget(ssid, server.arg("wpass"));
  logLine("wifi target set to %s, rebooting", ssid.c_str());
  server.send(200, "text/html; charset=utf-8", savedPage(ssid));
  // Let the response actually leave before the reset tears the socket down.
  delay(1200);
  ESP.restart();
}

static void handleRescueConfig() {
  if (!configAuthorised()) {
    server.send(403, "text/plain; charset=utf-8", "bad pass");
    return;
  }

  String ssid = server.arg("ssid");
  if (!ssid.length()) {
    Credentials cur = storedRescue();
    // 只有在目標網路連不上時才會用到這組。
    String status = F("只有在目標網路"
                      "連不上時才會用到"
                      "這組。");
    // 設定救援熱點
    server.send(200, "text/html; charset=utf-8",
                configFormPage("/rescue",
                               "設定救援熱點", cur.ssid,
                               status));
    return;
  }

  storeRescue(ssid, server.arg("wpass"));
  logLine("rescue hotspot set to %s", ssid.c_str());
  // No reboot: the rescue credentials are not in use right now, and rebooting
  // to apply them would drop the very connection being used to set them.
  server.send(200, "text/html; charset=utf-8", savedPage(ssid));
}

// ---- http status page -------------------------------------------------
static void handleRoot() {
  String b = "=== ESP32-B watchdog ===\n";
  b += "ssid: " + String(connectedSsid);
  if (onRescueNetwork) b += "  [RESCUE HOTSPOT]";
  b += "\n";
  {
    Credentials t = storedTarget();
    b += "target: " + (t.ssid.length() ? t.ssid : String("(none, using defaults)"));
    if (targetPending()) b += "  [unproven]";
    b += "\n";
    Credentials pv = storedPrevious();
    if (pv.ssid.length()) b += "target prev: " + pv.ssid + "\n";
    b += "rescue: " + storedRescue().ssid + "\n";
    if (wifiReverted)
      b += "note: " + wifiAttemptedSsid + " failed, reverted this boot\n";
  }
  b += "ip: " + WiFi.localIP().toString() + "\n";
  b += "rssi: " + String(WiFi.RSSI()) + " dBm\n";
  b += "uptime: " + String(millis() / 1000) + "s\n";
  b += "free heap: " + String(ESP.getFreeHeap()) + "\n\n";
  b += "checks run: " + String(checksRun) + "\n";
  b += "alerts raised: " + String(alertsRaised) + "\n";
  b += "alerts cleared: " + String(alertsCleared) + "\n";
  b += "alert active now: " + String(anyAlertActive ? "YES" : "no") + "\n";
  b += "discord sent: " + String(notifySent) + "\n";
  b += "discord acked: " + String(notifyAcked) + "\n";
  b += "bot id: " + (botUserId.length() ? botUserId : String("UNKNOWN")) + "\n";
  // Which OTA slot is live, and how much room the next update has. A failed
  // "Could Not Activate" points here first.
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (run) b += "running part: " + String(run->label) + " size=" + String(run->size) + "\n";
  if (next) b += "next part: " + String(next->label) + " size=" + String(next->size) + "\n";
  b += "sketch size: " + String(ESP.getSketchSize()) + "\n";
  b += "free sketch space: " + String(ESP.getFreeSketchSpace()) + "\n";
  // PSRAM presence: a psram_type mismatch is invisible to USB flashing but
  // can break the buffer allocation an OTA write needs.
  b += "psram: " + String(ESP.getPsramSize()) + " (free " +
       String(ESP.getFreePsram()) + ")\n";
  b += "flash: " + String(ESP.getFlashChipSize()) + "\n";
  // Both OTA slots, always visible -- not only in a failure message. This is
  // the state set_boot_partition() consults, and it has never been captured
  // while the board was healthy, so there is no baseline to compare against.
  for (int i = 0; i < 2; i++) {
    const esp_partition_t *pp = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        i ? ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0,
        nullptr);
    b += String(i ? "app1" : "app0") + ": ";
    if (!pp) { b += "missing\n"; continue; }
    esp_ota_img_states_t st;
    esp_err_t se = esp_ota_get_state_partition(pp, &st);
    b += "state=";
    b += (se == ESP_OK) ? String((int)st)
                        : String("ERR:") + esp_err_to_name(se);
    esp_image_header_t ih = {};
    if (esp_partition_read(pp, 0, &ih, sizeof(ih)) == ESP_OK) {
      char buf[64];
      snprintf(buf, sizeof(buf), " magic=%02X chip=%u segs=%u", ih.magic,
               (unsigned)ih.chip_id, (unsigned)ih.segment_count);
      b += buf;
    }
    b += "\n";
  }
  // A reboot loop is diagnosed from these: why the last boot ended, how close
  // the heap has ever come to empty, and the largest block still allocatable
  // (each TLS session needs a big contiguous one).
  b += "last reset: " + String(resetReasonName()) + "\n";
  b += "min free heap ever: " + String(ESP.getMinFreeHeap()) + "\n";
  b += "largest free block: " + String(ESP.getMaxAllocHeap()) + "\n";
  b += "task wdt: " + String(WDT_TIMEOUT_S) + "s\n";
  b += "fw version: " + String(otaPullVersion()) + "\n";
  b += "fw pull: " + otaPullStatus() + " (tries " +
       String(otaPullAttempts()) + ", fails " +
       String(otaPullFailures()) + ")\n";
  b += "last check: " + lastSummary + "\n";
  b += "last error: " + lastError + "\n";
  b += "raw sample ctor : " + String(esp_err_to_name(rawSampleEarlyErr)) +
       " " + probeHex(rawSampleEarly, 8) + "\n";
  b += "raw sample setup: " + String(esp_err_to_name(rawSampleSetupErr)) +
       " " + probeHex(rawSampleSetup, 8) + "\n";
  if (otaLastProbe.length()) {
    b += "---- last flash probe ----\n";
    b += otaLastProbe;
  }
  b += "========================\n\n";
  b += logBuf;
  server.send(200, "text/plain; charset=utf-8", b);
}

static void handleCheckNow() {
  runCheck();
  server.send(200, "text/plain; charset=utf-8",
              "check done: " + lastSummary + "\n");
}

// Posts a real Discord alert on demand, so the notification path can be
// verified without waiting for an actual outage.
// Forces a firmware check now instead of waiting for the poll interval.
static void handleFwCheck() {
  otaPullCheck(RTDB_HOST);
  // If an update was applied the board reboots inside the call above, so
  // reaching this line means no update happened.
  server.send(200, "text/plain; charset=utf-8",
              "running v" + String(otaPullVersion()) + "\nresult: " +
                  otaPullStatus() + "\n");
}

static void handleTestAlert() {
  // "手動測試" -- manual test
  String msgId = discordNotify("TEST", "\\u624b\\u52d5\\u6e2c\\u8a66", 0,
                               time(nullptr), "28.0 \\u00b0C", 1);
  if (!msgId.length()) {
    server.send(500, "text/plain", "discord post failed: " + lastError + "\n");
    return;
  }
  server.send(200, "text/plain",
              "posted msg " + msgId + "\nreact to it, then GET /testack?msg=" +
                  msgId + "\n");
}

// Runs the flash read-path probe against the slot the next update would use,
// on demand. This is how a VALIDATE_FAILED gets diagnosed remotely: the slot
// still holds the rejected image, and the probe says whether the raw SPI
// path and the mmap/cache path even agree on what is there.
static void handleVerify() {
  const esp_partition_t *t = esp_ota_get_next_update_partition(nullptr);
  if (server.hasArg("part"))
    t = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 server.arg("part") == "app0"
                                     ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                     : ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                 nullptr);
  if (!t) {
    server.send(500, "text/plain", "no target partition\n");
    return;
  }
  size_t len = server.hasArg("len") ? (size_t)server.arg("len").toInt() : 0;
  FlashProbeResult r = flashProbe(t, len);
  otaLastProbe = flashProbeReport(r, "");
  logLine("probe %s: raw=%.8s mmap=%.8s diff=%d imgv=%s", t->label,
          r.shaRaw.c_str(), r.shaMmap.c_str(), r.diffChunks,
          esp_err_to_name(r.imgVerify));
  server.send(200, "text/plain; charset=utf-8",
              String("target: ") + t->label + " @0x" +
                  String(t->address, HEX) + "\n" + otaLastProbe);
}

// Reads a window of a partition over both paths and returns the bytes
// themselves, so the exact geometry of a misread can be mapped from outside:
//   /rawprobe?part=app1&off=0x20&n=64&step=16
// reads n bytes starting at off, the raw path in `step`-sized calls (the
// mmap reference in one go), and prints both. Varying `step` measures whether
// the corruption depends on transaction size; varying `off`, on address.
static void handleRawProbe() {
  String pname = server.hasArg("part") ? server.arg("part") : "app1";
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      pname == "app0" ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                      : ESP_PARTITION_SUBTYPE_APP_OTA_1,
      nullptr);
  if (!p) {
    server.send(500, "text/plain", "partition not found\n");
    return;
  }
  size_t off = strtoul(server.arg("off").c_str(), nullptr, 0);
  size_t n = server.hasArg("n") ? strtoul(server.arg("n").c_str(), nullptr, 0)
                                : 64;
  if (n < 1) n = 1;
  if (n > 1024) n = 1024;
  size_t step = server.hasArg("step")
                    ? strtoul(server.arg("step").c_str(), nullptr, 0)
                    : n;
  if (step < 1 || step > n) step = n;
  if (off + n > p->size) {
    server.send(400, "text/plain", "window past partition end\n");
    return;
  }

  uint8_t raw[1024];
  memset(raw, 0xAA, sizeof(raw));
  String errs;
  for (size_t i = 0; i < n; i += step) {
    size_t c = min(step, n - i);
    esp_err_t e = esp_partition_read(p, off + i, raw + i, c);
    if (e != ESP_OK) errs += String(esp_err_to_name(e)) + "@" + String(i) + " ";
  }

  String out = pname + " off=0x" + String(off, HEX) + " n=" + String(n) +
               " step=" + String(step) + "\n";
  if (errs.length()) out += "read errors: " + errs + "\n";
  out += "raw:  " + probeHex(raw, n) + "\n";

  const void *mem = nullptr;
  spi_flash_mmap_handle_t mh = 0;
  if (esp_partition_mmap(p, off, n, SPI_FLASH_MMAP_DATA, &mem, &mh) ==
      ESP_OK) {
    out += "mmap: " + probeHex((const uint8_t *)mem, n) + "\n";
    spi_flash_munmap(mh);
  } else {
    out += "mmap: FAILED\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

// Reports whether the test message has been acknowledged yet.
static void handleTestAck() {
  String msgId = server.arg("msg");
  if (!msgId.length()) {
    server.send(400, "text/plain", "usage: /testack?msg=<message_id>\n");
    return;
  }
  bool acked = discordAcked(msgId);
  server.send(200, "text/plain",
              String("acked: ") + (acked ? "YES" : "no") + "\n");
}

void setup() {
  rawSampleSetupErr = sampleCrossingRead(rawSampleSetup);

  // First thing, before anything that could fault: an OTA-installed image
  // that dies during boot leaves no other trace. The delay gives the USB CDC
  // link time to enumerate, or the opening lines are lost.
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.printf("=== boot: %s (v%lu) reset=%s ===\n", FW_VERSION,
                (unsigned long)FW_VERSION_CODE, resetReasonName());
  {
    const esp_partition_t *r = esp_ota_get_running_partition();
    if (r)
      Serial.printf("running=%s @0x%lx\n", r->label,
                    (unsigned long)r->address);
    for (int i = 0; i < 2; i++) {
      const esp_partition_t *pp = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP,
          i ? ESP_PARTITION_SUBTYPE_APP_OTA_1
            : ESP_PARTITION_SUBTYPE_APP_OTA_0,
          nullptr);
      if (!pp) continue;
      esp_ota_img_states_t st;
      esp_err_t se = esp_ota_get_state_partition(pp, &st);
      Serial.printf("%s: state=%s\n", pp->label,
                    se == ESP_OK ? String((int)st).c_str()
                                 : esp_err_to_name(se));
    }
  }

  blinkColor(BLUE, 2, 120);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Keep retrying rather than rebooting straight away. The rescue hotspot is
  // switched on by hand after someone notices the LED, which cannot be
  // synchronised with a boot -- so the board has to still be looking when the
  // hotspot finally appears. Purple keeps blinking throughout: that is the
  // signal the person on site is told to watch for.
  //
  // Bounded, though, and this is the part that must not be removed. An image
  // that cannot reach WiFi is exactly what pull_ota's rollback exists to undo,
  // and rollback only happens on a reboot. Looping here forever would trap a
  // broken update in the one state it was designed to escape. So: give the
  // hotspot a realistic window, then reboot and let the safety net work.
  uint32_t wifiTries = 0;
  while (!connectWiFi()) {
    if (++wifiTries >= WIFI_BOOT_ATTEMPTS) {
      logLine("no wifi after %lu attempts, rebooting",
              (unsigned long)wifiTries);
      setLed(PURPLE);
      delay(2000);
      ESP.restart();
    }
    // Slow purple blink between passes, distinct from the fast blue of an
    // attempt in progress.
    for (int i = 0; i < 5; i++) blinkColor(PURPLE, 1, 500);
  }
  blinkColor(GREEN, 5, 60);

  // Staleness is judged against wall-clock time, so NTP must land first.
  // Taiwan is UTC+8. Stored timestamps are unix seconds either way, but
  // every human-readable time -- the log lines and the Discord alert --
  // goes through localtime_r(), so without the offset those all read 8
  // hours early.
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  uint32_t ntpDeadline = millis() + 15000;
  while (time(nullptr) < 1600000000 && millis() < ntpDeadline) delay(200);

  logLine("boot ok ip=%s ssid=%s time=%s%s",
          WiFi.localIP().toString().c_str(), connectedSsid,
          time(nullptr) > 1600000000 ? "synced" : "NOT SYNCED",
          onRescueNetwork ? " [RESCUE]" : "");
  otaPullBegin(RTDB_HOST, DEVICE_ID, FW_VERSION_CODE);
  logLine("fw v%lu, pull-ota: %s", (unsigned long)FW_VERSION_CODE,
          otaPullStatus().c_str());

  logLine("meta publish: %s", publishMeta() ? "ok" : "FAILED");

  // Identify ourselves so the ack check can ignore the bot's own reaction.
  String me;
  if (discordRequest("GET", "/users/@me", "", &me)) {
    int k = me.indexOf("\"id\":\"");
    if (k >= 0) botUserId = me.substring(k + 6, me.indexOf('"', k + 6));
    logLine("discord ok, bot id=%s", botUserId.c_str());
  } else {
    logLine("discord /users/@me FAILED: %s", lastError.c_str());
  }

  // Report what happened to WiFi at boot. After the bot id is known, because
  // that call is the proof Discord is usable at all.
  announceWiFiState();

  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    // A check does several blocking TLS round-trips; if one starts mid-upload
    // the OTA socket stalls long enough for the host to give up. Suspend
    // checking until the reboot that ends the update.
    otaInProgress = true;
    setLed(BLUE);
  });
  ArduinoOTA.onEnd([]() { setLed(GREEN); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    // An upload holds the loop for ~30s, which would otherwise look like a
    // stall to the watchdog.
    esp_task_wdt_reset();
    setLed((done / 16384) % 2 ? BLUE : OFF);
  });
  ArduinoOTA.onError([](ota_error_t) {
    otaInProgress = false;  // resume checking; the old firmware is still live
    setLed(RED);
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/wifi", handleWifiConfig);
  server.on("/rescue", handleRescueConfig);
  server.on("/check", handleCheckNow);
  server.on("/testalert", handleTestAlert);
  server.on("/testack", handleTestAck);
  server.on("/fwcheck", handleFwCheck);
  server.on("/verify", handleVerify);
  server.on("/rawprobe", handleRawProbe);
  server.begin();

  // Armed last, so a slow boot (WiFi retries, NTP wait) cannot trip it.
  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // true = panic/reboot on timeout
  esp_task_wdt_add(NULL);                  // watch the Arduino loop task
  logLine("task watchdog armed at %lus", (unsigned long)WDT_TIMEOUT_S);
}

void loop() {
  static uint32_t nextCheck = 0;

  // Every path below returns through here, so one feed at the top covers them
  // all; a stall inside runCheck() is exactly what we want to be caught.
  esp_task_wdt_reset();

  ArduinoOTA.handle();

  // While an upload is running, do nothing else: no HTTP serving, no checks.
  // An interrupted OTA leaves the old firmware intact, so pausing monitoring
  // for the ~30s of an update is the cheaper risk.
  if (otaInProgress) {
    delay(1);
    return;
  }

  server.handleClient();

  if ((int32_t)(millis() - nextCheck) < 0) {
    // Pulse while idle so health is readable at a glance: red means a node is
    // currently stale, which is the whole point of this device.
    static uint32_t nextPulse = 0;
    if ((int32_t)(millis() - nextPulse) >= 0) {
      nextPulse = millis() + 2000;
      setLed(anyAlertActive ? RED : GREEN);
      delay(30);
      setLed(OFF);
    }
    delay(10);
    return;
  }
  nextCheck = millis() + CHECK_INTERVAL_MS;

  // Give OTA a window before the blocking TLS work starts.
  for (int i = 0; i < 20; i++) {
    ArduinoOTA.handle();
    delay(5);
  }

  // Confirm the image as soon as it has proven the things an update could
  // plausibly break: WiFi is up and RTDB accepts a write. Everything after
  // this point is ordinary work, and some of it is slow -- runCheck() posts to
  // Discord on every cycle while a node is stale, which is enough to push the
  // loop past the 120s task watchdog. v16 was rolled back and blacklisted that
  // way: it never reached this call, because the pond node was unplugged and
  // each cycle was doing a Discord round-trip. Confirming before that work
  // means a genuinely broken image still reverts, but a merely slow one does
  // not take the whole update path down with it.
  publishSelf();
  otaMarkRunningFirmwareGood();

  // Poll for firmware BEFORE running the checks, not after. Remote update is
  // the only way to fix this board once it is deployed, so it must not sit
  // behind the work most likely to be slow or to hang: runCheck() reaches out
  // to Discord on every cycle while a node is stale, and a Discord outage
  // turns that into a failing TLS round-trip each minute. The cycles where
  // updating matters most are exactly the cycles where something is already
  // wrong, so the update path goes first.
  static uint32_t nextFwPoll = FW_POLL_FIRST_MS;
  if ((int32_t)(millis() - nextFwPoll) >= 0) {
    nextFwPoll = millis() + FW_POLL_INTERVAL_MS;
    otaPullCheck(RTDB_HOST);
    logLine("fw check: %s", otaPullStatus().c_str());
  }

  runCheck();
}
