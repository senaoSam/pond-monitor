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

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Arduino.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "pull_ota.h"
#include "reset_reason.h"
#include "secrets.h"

// ---- configuration ----------------------------------------------------
static const int LED_PIN = 48;  // onboard WS2812

static const char *DEVICE_ID = "watchdog";

// The development bench's network. A node seen here is being flashed or
// tested, so its first sighting is not announced -- see runCheck().
static const char *BENCH_SSID = "Wang3697";
static const char *DEVICE_NAME = "home-watchdog";
// "w" since w55; "b" (b54 and earlier) dated from when this was board B, and
// B is now a pond. FW_VERSION_CODE carried on from 54 regardless.
static const char *FW_VERSION = "w60-2026.10.20";
// Monotonic; RTDB /firmware/watchdog/version is compared against this to
// decide whether a pull-based update is due. Bump on every release.
static const uint32_t FW_VERSION_CODE = 60;

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

// Forgets both stored targets, so the next boot starts from the known sites as
// if the board had never been configured.
//
// Needed because every /wifi call stores the network that currently works as
// the previous target -- which is exactly right in the field, and exactly what
// makes the later steps unreachable on the bench: step 2 catches the board
// every time, so the rescue hotspot can never be exercised no matter how many
// bad SSIDs are tried.
//
// Also the honest way to hand a board on to a new site: without it, the old
// pond's credentials stay one layer down for the rest of the board's life.
static void clearTargets() {
  wifiPrefs.begin("wifi", false);
  wifiPrefs.remove("ssid");
  wifiPrefs.remove("pass");
  wifiPrefs.remove("pssid");
  wifiPrefs.remove("ppass");
  wifiPrefs.remove("pending");
  wifiPrefs.end();
}

// Promotes the previous target back to current. Called when a pending target
// failed to connect, so the board returns to the network it was last happy on
// instead of waiting for someone to notice.
static void revertTarget() {
  Credentials prev = storedPrevious();
  wifiPrefs.begin("wifi", false);
  // Clear the marker whether or not there is somewhere to revert to. It only
  // means "this target has not proved itself yet", and it has now had its
  // chance -- leaving it set made the board re-run the same failing sequence
  // on every boot, forever, with the status page permanently reading
  // [unproven].
  wifiPrefs.remove("pending");
  if (!prev.ssid.length()) {
    wifiPrefs.end();
    return;
  }
  wifiPrefs.putString("ssid", prev.ssid);
  wifiPrefs.putString("pass", prev.pass);
  wifiPrefs.remove("pssid");
  wifiPrefs.remove("ppass");
  wifiPrefs.remove("pending");
  wifiPrefs.end();
}

// ---- connecting ---------------------------------------------------------

// Filled in by tryNetwork as the boot proceeds and read by the incident
// record; defined here because the connection code runs before that module.
static String bootAttempts;
static uint32_t bootWifiMs;
static uint32_t bootPasses;

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
  uint32_t started = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t deadline = started + WIFI_ATTEMPT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline)
    blinkColor(BLUE, 1, 250);

  // Each outcome goes into the boot record. Which networks were tried, in
  // what order, and how long each took is what separates "wrong password"
  // (fails fast) from "router not answering" (runs the full timeout) from
  // "weak signal" (connects, but slowly and at a poor RSSI).
  uint32_t took = millis() - started;
  bool ok = WiFi.status() == WL_CONNECTED;
  if (bootAttempts.length()) bootAttempts += " ";
  bootAttempts += ssid + (ok ? ":ok(" : ":fail(") + String(took / 1000.0, 1) +
                  "s";
  if (ok) bootAttempts += "," + String(WiFi.RSSI()) + "dBm";
  bootAttempts += ")";
  bootWifiMs += took;

  if (ok) return true;
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

// ---- incident record ---------------------------------------------------
//
// The one class of failure this device cannot report is the one where it has
// no network: a power dip at the pond, a router that went away overnight, a
// hang. By the time anyone looks, the board has usually been power-cycled by
// hand and is working again, and whatever happened left no trace.
//
// So each boot writes a short record of how the *previous* run ended and how
// this one came up, and uploads it once there is a network. It answers "what
// actually happened at 3am" well enough to say which kind of problem it was --
// power, network, firmware or a person -- which is what decides who has to do
// something about it.
//
// Deliberately one record, overwritten each boot, not a history. Anything
// worth keeping is in RTDB within a minute of the board coming back; NVS only
// has to survive the gap.
//
// Flash wear is not a concern at this rate. NVS wear-levels across its whole
// partition and the endurance spec is 100k erases per sector, so one write per
// boot is decades even if the board rebooted hourly. What would kill it is
// writing from loop() -- so nothing here is ever called from there.

static Preferences incPrefs;

// Written continuously while healthy, read back on the next boot. Together
// with the reset reason it separates a power cut from a person unplugging the
// board: a POWERON whose last heartbeat was seconds ago is a supply that
// vanished without warning, while one whose last heartbeat was hours ago is
// somebody who pulled the plug and put it back later.
static const uint32_t BEAT_INTERVAL_S = 60;
static uint32_t lastBeatWrite = 0;

// What the previous run left behind, read once at boot.
struct PrevRun {
  bool present;
  uint32_t lastBeat;    // unix seconds of the last healthy heartbeat
  uint32_t uptime;      // how long that run had been going, seconds
  uint32_t bootCount;   // total boots, so a reboot loop is visible as a number
};
static PrevRun prevRun = {false, 0, 0, 0};

// This boot, filled in as it happens.
static void incidentBegin() {
  incPrefs.begin("inc", false);
  prevRun.lastBeat = incPrefs.getUInt("beat", 0);
  prevRun.uptime = incPrefs.getUInt("up", 0);
  prevRun.bootCount = incPrefs.getUInt("boots", 0);
  prevRun.present = prevRun.lastBeat > 0 || prevRun.bootCount > 0;
  incPrefs.putUInt("boots", prevRun.bootCount + 1);
  // Clear the uptime immediately: if this boot dies before writing its own,
  // the next one should see 0 rather than inherit a stale figure.
  incPrefs.putUInt("up", 0);
  incPrefs.end();
}

// Called from loop() while things are healthy -- but only actually writes once
// a minute, which is the whole reason this is safe to call from there.
static void incidentHeartbeat() {
  time_t now = time(nullptr);
  if (now < 1600000000) return;  // no clock, a timestamp would be meaningless
  if (lastBeatWrite && (millis() - lastBeatWrite) < BEAT_INTERVAL_S * 1000)
    return;
  lastBeatWrite = millis();

  incPrefs.begin("inc", false);
  incPrefs.putUInt("beat", (uint32_t)now);
  incPrefs.putUInt("up", millis() / 1000);
  incPrefs.end();
}

// True when this boot is worth reporting. A clean restart that came straight
// back up is not an incident -- reporting every reboot would bury the ones
// that matter, and most reboots here are our own OTA.
static bool incidentWorthReporting(const char *reset) {
  if (bootPasses > 0) return true;              // wifi did not work first try
  if (strstr(reset, "hang")) return true;       // any watchdog
  if (strstr(reset, "BROWNOUT")) return true;   // supply dipped
  if (strstr(reset, "GLITCH")) return true;
  if (strstr(reset, "PANIC")) return true;
  if (strstr(reset, "crash")) return true;
  // A power-on whose previous run was still beating moments ago is a supply
  // that disappeared with no warning -- the signature of a dip too fast for
  // the brownout detector, which otherwise looks exactly like a normal start.
  if (strstr(reset, "POWERON") && prevRun.present && prevRun.lastBeat) {
    time_t now = time(nullptr);
    if (now > (time_t)prevRun.lastBeat &&
        (uint32_t)(now - prevRun.lastBeat) < 180)
      return true;
  }
  return false;
}

// Uploads the record and clears the marker. Best-effort: if RTDB is
// unreachable the record stays in NVS and the next boot tries again.
static void incidentReport(const char *rtdbHost, const char *deviceId) {
  const char *reset = resetReasonName();
  if (!incidentWorthReporting(reset)) return;

  time_t now = time(nullptr);
  if (now < 1600000000) return;  // key would be meaningless

  String body = "{";
  body += "\"reset\":\"" + String(reset) + "\"";
  body += ",\"boots\":" + String(prevRun.bootCount + 1);
  if (prevRun.lastBeat) {
    body += ",\"lastBeat\":" + String(prevRun.lastBeat);
    body += ",\"gap\":" + String((uint32_t)(now - (time_t)prevRun.lastBeat));
  }
  body += ",\"prevUptime\":" + String(prevRun.uptime);
  body += ",\"wifiMs\":" + String(bootWifiMs);
  body += ",\"wifiPasses\":" + String(bootPasses);
  body += ",\"attempts\":\"" + jsonEscape(bootAttempts) + "\"";
  body += ",\"ssid\":\"" + jsonEscape(String(connectedSsid)) + "\"";
  body += ",\"rssi\":" + String(WiFi.RSSI());
  body += ",\"fw\":" + String(FW_VERSION_CODE);
  body += ",\"heap\":" + String(ESP.getFreeHeap());
  body += "}";

  String path = "/incidents/" + String(deviceId) + "/" +
                String((uint32_t)now) + ".json";
  if (rtdbRequest("PUT", path, body, nullptr))
    logLine("incident reported: %s", reset);
  else
    logLine("incident report FAILED: %s", lastError.c_str());
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
// Time of day alone is only unambiguous for today. An alert that has been
// firing since yesterday said "last reported 11:18" with no way to tell which
// day was meant -- and by the time anyone reads it, that is the normal case:
// a node stays down until somebody drives out to it. So the date is included
// as soon as the timestamp is not from today, and left off when it is, which
// keeps the common alert short.
// "2305 分鐘" is arithmetic homework, not information. Past an hour the useful
// unit is hours, and past a day it is days -- the reader wants to know roughly
// how long this has been going on, not the exact minute count.
static String humanDuration(uint32_t seconds) {
  char buf[32];
  if (seconds < 3600)
    snprintf(buf, sizeof(buf), "%lu \\u5206\\u9418",
             (unsigned long)(seconds / 60));  // 分鐘
  else if (seconds < 86400)
    snprintf(buf, sizeof(buf), "%lu \\u5c0f\\u6642 %lu \\u5206",
             (unsigned long)(seconds / 3600),
             (unsigned long)((seconds % 3600) / 60));  // 小時 分
  else
    snprintf(buf, sizeof(buf), "%lu \\u5929 %lu \\u5c0f\\u6642",
             (unsigned long)(seconds / 86400),
             (unsigned long)((seconds % 86400) / 3600));  // 天 小時
  return String(buf);
}

static String localHhMm(time_t t) {
  if (t < 1600000000) return "\\u4e0d\\u660e";  // unknown

  struct tm tm;
  localtime_r(&t, &tm);

  time_t now = time(nullptr);
  struct tm nowTm;
  localtime_r(&now, &nowTm);

  char buf[24];
  if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday)
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  else
    snprintf(buf, sizeof(buf), "%d/%d %02d:%02d", tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
  return String(buf);
}

// Posts an alert and returns the message id, which later reaction polling
// needs. Also pre-seeds the tick so acknowledging is a single tap. `content`
// is JSON string source (see alertText).
static String postAlert(const String &content) {
  String body = "{\"content\":\"" + content + "\"}";
  String resp;
  if (!discordRequest("POST",
                      "/channels/" + String(DISCORD_CHANNEL_ID) + "/messages",
                      body, &resp)) {
    logLine("discord notify FAILED: %s", lastError.c_str());
    return "";
  }
  // The message's own id is the one at the top level of the response; the
  // first "id" in the text belongs to the mentions array instead.
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
    // And the previous episode's progress, which batchSave() rewrites for
    // this one (see alertsRestore).
    body += ",\"state\":null";
  } else {
    body += ",\"clearedAt\":" + String((uint32_t)now);
  }
  body += "}";

  return rtdbRequest("PATCH", "/alerts/" + id + ".json", body, nullptr);
}

// /alerts/<id> holds only the latest episode; this keeps every one, for
// statistics read by other sites. Keyed by the last heartbeat before going
// quiet, so the re-raise after this board reboots lands on the same record
// instead of counting the outage twice. Best-effort: a failure costs a row of
// history, never the alert itself.
static void writeOutage(const String &id, time_t start, bool active,
                        time_t end, const String &reason) {
  if (!start) return;  // never published: there is no start to key on
  time_t now = time(nullptr);
  String path = "/outages/" + id + "/" + String((uint32_t)start);
  // A heartbeat board writes its own end -- its first beat back, exactly --
  // while ours is whatever latest.ts read at the next check, up to a beat
  // later. One extra read, on recoveries only, to not overwrite the better one.
  String boardEnd;
  if (!active && rtdbRequest("GET", path + "/end.json", "", &boardEnd) &&
      boardEnd != "null")
    end = 0;
  String body = "{";
  body += "\"start\":" + String((uint32_t)start);
  body += ",\"active\":" + String(active ? "true" : "false");
  if (active) {
    body += ",\"detectedAt\":" + String((uint32_t)now);
    body += ",\"reason\":\"" + reason + "\"";
  } else if (end) {
    body += ",\"end\":" + String((uint32_t)end);
    body += ",\"dur\":" + String((uint32_t)(end > start ? end - start : 0));
  }
  body += "}";
  if (!rtdbRequest("PATCH", path + ".json", body, nullptr))
    logLine("outage record FAILED for %s: %s", id.c_str(), lastError.c_str());
}

// ---- phone: the Air724UG dialer ---------------------------------------
//
// A Discord mention at 3am is easy to sleep through, so an alert nobody has
// ticked eventually rings a phone. The ringing is done by an Air724UG 4G
// module (air-dialer/) on UART1, over VoLTE. It never touches the internet:
// it is on a prepaid card that bills data by the MB, while an unanswered call
// -- which is the alarm -- costs nothing. Everything that needs the network
// stays here, on the house WiFi.
//
// Protocol, one line each way (see air-dialer/dialer.lua):
//   PING                     -> PONG csq=<n> reg=<0|1> ims=<0|1>
//   LINK                     (sent on each PONG: "I heard you")
//   DIAL <num> <ring_s>      -> ACK, later DONE <outcome>
//   HANGUP                   (the DIAL in progress ends with DONE cancelled)
//   (module boot)            -> READY <version>
//
// Driven from loop() without blocking: a call takes ~30s, and waiting for it
// in place would stall HTTP, OTA and the task watchdog for that long.
//
// Wiring: GPIO17 -> module RXD1, GPIO18 <- module TXD1, GND common, and
// GPIO16 -> PWR_EN. PWR_EN low cuts the module's power and floating powers
// it, so the pin is left as an input except to power-cycle a module that has
// stopped answering.
static const int AIR_TX_PIN = 17;
static const int AIR_RX_PIN = 18;
static const int AIR_EN_PIN = 16;
// Every 30s, because the module's DTU LED shows the link from these: steady
// once PINGs arrive and our LINK replies do too, so whoever wires the boards
// up on site can see within half a minute whether it is right. UART costs
// nothing. The power-cycle threshold is counted in PINGs, so it is set to
// keep about five minutes of silence before acting.
static const uint32_t AIR_PING_EVERY_MS = 30UL * 1000;
static const uint32_t AIR_PONG_WAIT_MS = 5000;
static const uint32_t AIR_DEAD_PINGS = 10;      // missed in a row -> power-cycle
static const uint32_t AIR_DIAL_TIMEOUT_MS = 90UL * 1000;
// Between the end of one call and the next DIAL. The module is still tearing
// the last call down when it reports DONE: a DIAL sent in the same second
// failed at once as nocarrier and that number never rang (w58, the second
// number of a round whose first was answered in seconds).
static const uint32_t AIR_DIAL_GAP_MS = 5000;
static const int RING_S = 25;

// The phone rings together with this Discord notification of an episode --
// the third -- if none has been ticked: notifications go out at 0, 5 and 10
// minutes after a node went stale, so the call lands ~15 minutes after the
// supply actually failed (b48 waited one more period, ~20; the user asked for
// it with the third). Keyed to time rather than to how many posts succeeded,
// so a Discord outage cannot hold the phone back.
//
// The episode starts when the node went stale -- its last beat plus the stale
// limit -- not when this board noticed. Since b53: before, a watchdog that was
// itself down (or rebooting, which a sagging supply made it do every few
// minutes) restarted the ten minutes from whenever it got back, and a supply
// that failed while it was down rang 30 minutes late instead of at once.
static const int NOTIFIES_BEFORE_CALL = 3;

// When an episode's first call is due: the moment its NOTIFIES_BEFORE_CALL-th
// notification is.
static time_t callDueAt(time_t episodeAt) {
  return episodeAt + (NOTIFIES_BEFORE_CALL - 1) * RENOTIFY_S;
}
// After the first, a round every CALL_REPEAT_S -- counted from the start of
// the last one, a round still dialing when the next is due finishing first --
// for as long as the alert stands. Only a tick on Discord (or the node coming
// back) stops it: answering does not. The phone is an alarm clock and nothing
// more, so a call picked up half-asleep, or by a voicemail box, is not taken
// as anyone knowing. Until w56 it was every 10 minutes, three rounds at most,
// after which the phone went quiet whether anyone had woken or not.
static const uint32_t CALL_REPEAT_S = 3 * 60;
// Checks come about once a minute, give or take a second, so a round due at
// exactly 180s could be seen at 179s and slip a whole check (w56 rang 4
// minutes apart that way). Due this much early counts as due.
static const uint32_t CALL_SLACK_S = 30;

// The numbers, rung one after another in this order each round. They live in
// /alertControl/callNumbers (a list), so they are changed from RTDB without a
// release. The last list read is kept in NVS, so a reboot with RTDB out of
// reach still rings the right phones; until a list has ever been read, the one
// compiled in from secrets.h.
static const char *CALL_NUMBERS_BUILTIN[] = CALL_NUMBERS;
static const int MAX_CALL_NUMBERS = 6;
static String callNumbers[MAX_CALL_NUMBERS];
static int callNumberCount = 0;
static String callNumbersFrom = "built-in";  // built-in | nvs | db
static String callNumbersBad = "";           // last unusable list warned about
static Preferences callPrefs;

static bool discordSay(const String &content);

// What the module accepts (dialer.lua checks the same): digits, an optional
// leading +, 6 to 15 long.
static bool callNumberValid(const String &n) {
  int start = n.startsWith("+") ? 1 : 0;
  if ((int)n.length() - start < 6 || n.length() > 15) return false;
  for (int i = start; i < (int)n.length(); i++)
    if (!isDigit(n[i])) return false;
  return true;
}

static String callNumbersJoined() {
  String s;
  for (int i = 0; i < callNumberCount; i++)
    s += (i ? "," : "") + callNumbers[i];
  return s;
}

// Takes a comma-separated list; returns how many usable numbers it held. With
// none, the current list is left alone.
static int callNumbersSet(const String &list, const char *from) {
  String found[MAX_CALL_NUMBERS];
  int n = 0, pos = 0;
  while (pos <= (int)list.length() && n < MAX_CALL_NUMBERS) {
    int comma = list.indexOf(',', pos);
    if (comma < 0) comma = list.length();
    String num = list.substring(pos, comma);
    num.trim();
    if (callNumberValid(num)) found[n++] = num;
    else if (num.length()) logLine("call numbers: ignoring \"%s\"", num.c_str());
    pos = comma + 1;
  }
  if (!n) return 0;
  for (int i = 0; i < n; i++) callNumbers[i] = found[i];
  callNumberCount = n;
  callNumbersFrom = from;
  return n;
}

// Once, at boot.
static void callNumbersLoad() {
  callPrefs.begin("calls", true);
  String saved = callPrefs.getString("list", "");
  callPrefs.end();
  if (callNumbersSet(saved, "nvs")) return;
  String builtin;
  for (const char *n : CALL_NUMBERS_BUILTIN)
    builtin += String(builtin.length() ? "," : "") + n;
  callNumbersSet(builtin, "built-in");
}

// Called with /alertControl/callNumbers on every good read of it. A missing
// entry leaves the list as it is. One with nothing dialable in it is taken for
// a mistake rather than a wish for silence -- switching alerts off is what the
// rest of /alertControl is for -- so the old list stays, and Discord hears
// about it once.
static void callNumbersFromDb(JsonVariantConst v) {
  if (v.isNull()) return;
  String list;
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>())
      list += (list.length() ? "," : "") + e.as<String>();
  } else if (v.is<JsonObjectConst>()) {
    // A list edited by hand in the Firebase console comes back as an object
    // once an entry in the middle is deleted: {"0":..,"2":..}, keys in order.
    for (JsonPairConst e : v.as<JsonObjectConst>())
      list += (list.length() ? "," : "") + e.value().as<String>();
  } else {
    list = v.as<String>();
  }

  String before = callNumbersJoined();
  String beforeFrom = callNumbersFrom;
  if (!callNumbersSet(list, "db")) {
    if (list != callNumbersBad) {
      callNumbersBad = list;
      logLine("call numbers: nothing dialable in db, keeping %s",
              before.c_str());
      discordSay("📞 撥號名單裡沒有可撥的號碼，沿用：" + before);
    }
    return;
  }
  callNumbersBad = "";
  String now = callNumbersJoined();
  if (now == before && beforeFrom != "built-in") return;
  callPrefs.begin("calls", false);
  callPrefs.putString("list", now);
  callPrefs.end();
  if (now == before) return;  // the built-in list confirmed: nothing to say
  logLine("call numbers: %s (was %s)", now.c_str(), before.c_str());
  discordSay("📞 撥號名單：" + now);
}

struct AirStatus {
  bool everAnswered = false;
  int csq = -1, reg = -1, ims = -1;
  uint32_t lastPongMs = 0;
  uint32_t missed = 0;      // PINGs in a row without a PONG
  uint32_t restarts = 0;    // power-cycles this boot
  uint32_t cyclesSincePong = 0;
  uint32_t calls = 0;       // DIALs completed this boot
  String lastOutcome = "none";
  String version = "";
} air;

// alarm: rung for an alert, so dropped the moment that is ticked. A /testcall
// is not, and always goes through.
struct PendingDial {
  String number;
  bool alarm;
};
static PendingDial dialQueue[MAX_CALL_NUMBERS];
static int dialQueueLen = 0;
static bool dialInFlight = false, dialIsAlarm = false;
static uint32_t dialStartMs = 0, dialEndMs = 0;
static String dialNumber;
static bool pongAwaited = false;
static uint32_t pingSentMs = 0;
static uint32_t nextPingMs = 15000;  // first PING shortly after boot
static String airLine;

// Queues one call per number, in order.
static void airDial(bool alarm) {
  for (int i = 0; i < callNumberCount; i++) {
    if (dialQueueLen >= MAX_CALL_NUMBERS) {
      logLine("air: dial queue full, dropping %s", callNumbers[i].c_str());
      continue;
    }
    dialQueue[dialQueueLen++] = {callNumbers[i], alarm};
  }
}

static void airSend(const String &line) {
  Serial1.print(line + "\n");
}

// Whether a round rung for an alert is still under way.
static bool airAlarmPending() {
  if (dialInFlight && dialIsAlarm) return true;
  for (int i = 0; i < dialQueueLen; i++)
    if (dialQueue[i].alarm) return true;
  return false;
}

// The alert was ticked (or recovered, or switched off): the rest of the round
// is dropped, and a phone ringing right now stops.
static void airCancelAlarm() {
  int kept = 0, dropped = 0;
  for (int i = 0; i < dialQueueLen; i++) {
    if (dialQueue[i].alarm) dropped++;
    else dialQueue[kept++] = dialQueue[i];
  }
  dialQueueLen = kept;
  bool ringing = dialInFlight && dialIsAlarm;
  if (ringing) airSend("HANGUP");  // DONE cancelled follows
  if (dropped || ringing)
    logLine("air: alarm stopped, %d queued call(s) dropped%s", dropped,
            ringing ? ", hanging up" : "");
}

// Defined with the batches: polls Discord for a tick, and says whether any
// alert still wants the phone. Asked before each call of a round, so a tick
// stops the numbers not yet rung rather than waiting for the next check.
static bool phoneStillWanted();

static void airHandleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line.startsWith("PONG")) {
    pongAwaited = false;
    air.missed = 0;
    air.everAnswered = true;
    air.lastPongMs = millis();
    air.cyclesSincePong = 0;
    int k;
    if ((k = line.indexOf("csq=")) >= 0) air.csq = line.substring(k + 4).toInt();
    if ((k = line.indexOf("reg=")) >= 0) air.reg = line.substring(k + 4).toInt();
    if ((k = line.indexOf("ims=")) >= 0) air.ims = line.substring(k + 4).toInt();
    airSend("LINK");  // completes the round trip the module's DTU LED shows
  } else if (line.startsWith("READY")) {
    air.version = line.substring(5);
    air.version.trim();
    logLine("air: module ready %s", air.version.c_str());
    nextPingMs = millis();  // light its DTU LED now, not up to 30s later
  } else if (line.startsWith("DONE")) {
    if (!dialInFlight) return;
    dialInFlight = false;
    dialEndMs = millis();
    air.calls++;
    air.lastOutcome = line.substring(5);
    air.lastOutcome.trim();
    logLine("air: call to %s ended: %s", dialNumber.c_str(),
            air.lastOutcome.c_str());
  } else if (line.startsWith("ERR")) {
    logLine("air: %s", line.c_str());
    if (dialInFlight) {
      dialInFlight = false;
      dialEndMs = millis();
      air.lastOutcome = line;
    }
  }
  // ACK needs nothing: DONE follows.
}

// Holds PWR_EN low long enough for the module's supply to collapse, then lets
// it float, which powers it back up.
static void airPowerCycle() {
  logLine("air: no answer to %lu PINGs, power-cycling",
          (unsigned long)air.missed);
  pinMode(AIR_EN_PIN, OUTPUT);
  digitalWrite(AIR_EN_PIN, LOW);
  for (int i = 0; i < 30; i++) {
    esp_task_wdt_reset();
    delay(100);
  }
  pinMode(AIR_EN_PIN, INPUT);
  air.restarts++;
  air.missed = 0;
  // A dial in flight died with the module.
  if (dialInFlight) {
    dialInFlight = false;
    air.lastOutcome = "module restarted";
  }
  nextPingMs = millis() + 60000;  // it needs a while to boot and register
}

static void airService() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      airHandleLine(airLine);
      airLine = "";
    } else if (c != '\r' && airLine.length() < 200) {
      airLine += c;
    }
  }

  if (dialInFlight && millis() - dialStartMs > AIR_DIAL_TIMEOUT_MS) {
    dialInFlight = false;
    dialEndMs = millis();
    air.lastOutcome = "timeout";
    logLine("air: call to %s got no DONE in time", dialNumber.c_str());
  }

  if (!dialInFlight && dialQueueLen > 0 &&
      millis() - dialEndMs >= AIR_DIAL_GAP_MS) {
    if (dialQueue[0].alarm && !phoneStillWanted()) {
      airCancelAlarm();
      return;
    }
    PendingDial d = dialQueue[0];
    for (int i = 1; i < dialQueueLen; i++) dialQueue[i - 1] = dialQueue[i];
    dialQueueLen--;
    dialInFlight = true;
    dialIsAlarm = d.alarm;
    dialStartMs = millis();
    dialNumber = d.number;
    airSend("DIAL " + d.number + " " + String(RING_S));
    logLine("air: dialing %s%s", d.number.c_str(), d.alarm ? "" : " (test)");
    return;
  }

  if (pongAwaited && millis() - pingSentMs > AIR_PONG_WAIT_MS) {
    pongAwaited = false;
    air.missed++;
    if (air.missed >= AIR_DEAD_PINGS) {
      // Said on Discord twice at most per stretch of silence -- when the first
      // power-cycle is tried, and when that did not bring it back -- and
      // without a mention: it is a degraded alarm path, not a pond outage.
      if (air.cyclesSincePong == 0)
        // "📞 撥號模組沒有回應，已斷電重啟"
        discordSay("\\ud83d\\udcde \\u64a5\\u865f\\u6a21\\u7d44\\u6c92\\u6709"
                   "\\u56de\\u61c9\\uff0c\\u5df2\\u65b7\\u96fb\\u91cd\\u555f");
      else if (air.cyclesSincePong == 1)
        // "📞 撥號模組重啟後仍無回應"
        discordSay("\\ud83d\\udcde \\u64a5\\u865f\\u6a21\\u7d44\\u91cd\\u555f"
                   "\\u5f8c\\u4ecd\\u7121\\u56de\\u61c9");
      air.cyclesSincePong++;
      airPowerCycle();
    }
  }

  if (!dialInFlight && !pongAwaited && (int32_t)(millis() - nextPingMs) >= 0) {
    nextPingMs = millis() + AIR_PING_EVERY_MS;
    pongAwaited = true;
    pingSentMs = millis();
    airSend("PING");
  }
}

// The module counts as healthy while its last PONG is recent and it reports
// being registered for VoLTE -- the one thing a call actually needs.
static bool airHealthy() {
  return air.everAnswered &&
         millis() - air.lastPongMs < 2 * AIR_PING_EVERY_MS + AIR_PONG_WAIT_MS &&
         air.ims == 1;
}

// ---- per-node state ---------------------------------------------------
//
// One entry per node seen in /devices. Held in RAM, but rebuilt from /alerts
// after a reboot (alertsRestore), so an alert carries on where it was instead
// of starting over.
//
// The text added in b48 is written as plain UTF-8. That is valid JSON as it
// stands; the \uXXXX escapes elsewhere in this file predate it and are left
// alone.
struct NodeState {
  String id;
  bool inUse = false;
  bool alertActive = false;  // mirrors /alerts/<id>/active
  time_t lastSeen = 0;       // its last heartbeat, as of when it went stale
  int batch = -1;            // the alert batch it is listed in
  bool muted = false;        // alerts switched off in /alertControl
  bool seenThisCheck = false;
  // Refreshed every check, for the alert text.
  uint32_t age = 0;
  String note, reading;
  // The network it last reported, so a move is announced once (see
  // announceNodeMove) rather than every minute it stays there.
  String lastSsid;
};

// Twenty-odd heartbeat boards plus the one-off nodes, with room to grow. The
// first version tracked six, which a single pond would have overflowed.
static const int MAX_NODES = 40;
static NodeState nodeStates[MAX_NODES];

static NodeState &nodeState(const String &id) {
  for (int i = 0; i < MAX_NODES; i++)
    if (nodeStates[i].inUse && nodeStates[i].id == id) return nodeStates[i];
  for (int i = 0; i < MAX_NODES; i++)
    if (!nodeStates[i].inUse) {
      nodeStates[i] = NodeState();
      nodeStates[i].inUse = true;
      nodeStates[i].id = id;
      return nodeStates[i];
    }
  // Full. The overflow node goes untracked rather than evicting a real one,
  // and the log says so.
  static NodeState overflow;
  static bool warned = false;
  if (!warned) {
    warned = true;
    logLine("node table full (%d): %s not tracked", MAX_NODES, id.c_str());
  }
  overflow = NodeState();
  overflow.id = id;
  return overflow;
}

static uint32_t notifySent = 0, notifyAcked = 0;

// Per site (the id's leading letter: A05 is at pond A), how many nodes there
// are and how many are stale, from the current check. When every node at a
// pond goes quiet at once it is far more likely the pond's router or uplink
// than every supply failing together, and the alert says so.
static int siteTotal[26], siteStale[26];

static int siteIndex(const String &id) {
  if (id.length() < 2 || id[0] < 'A' || id[0] > 'Z') return -1;
  for (size_t i = 1; i < id.length(); i++)
    if (!isdigit((unsigned char)id[i])) return -1;
  return id[0] - 'A';
}

// ---- alert switches ---------------------------------------------------
//
// Which nodes may notify, set from the owner's website. Switched off, a node
// is still watched and its outages still recorded in /alerts and /outages --
// only Discord and the phone go quiet, so the statistics have no holes.
//
//   /alertControl/groups/<gid>  {members: {A01:true, ...}, enabled, until?, note?}
//   /alertControl/nodes/<id>    {enabled, until?, note?}
//
// An entry counts until its "until" (unix seconds) passes; without one it
// holds for good. A node's own entry beats its groups. The website keeps a
// node in one group at most; should two disagree anyway, "on" wins -- a
// missed outage costs more than a needless call. Anything unlisted is on.
static JsonDocument alertCtl;

// Re-read every check. On a failed read the last good copy stays in force;
// before the first one, everything alerts.
static void alertControlLoad() {
  String body;
  if (!rtdbRequest("GET", "/alertControl.json", "", &body)) {
    logLine("alertControl read FAILED: %s", lastError.c_str());
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logLine("alertControl parse FAILED: %s", err.c_str());
    return;
  }
  alertCtl = std::move(doc);
  callNumbersFromDb(alertCtl["callNumbers"]);
}

static bool alertEntryLive(JsonVariantConst e, time_t now) {
  if (!e["enabled"].is<bool>()) return false;
  JsonVariantConst until = e["until"];
  return until.isNull() || (uint32_t)now < until.as<uint32_t>();
}

// members is written as {A01:true}; a plain list is accepted too.
static bool alertGroupHas(JsonVariantConst members, const String &id) {
  if (members.is<JsonArrayConst>()) {
    for (JsonVariantConst m : members.as<JsonArrayConst>())
      if (id == m.as<const char *>()) return true;
    return false;
  }
  return members[id].as<bool>();
}

static bool alertsEnabledFor(const String &id, time_t now) {
  JsonVariantConst own = alertCtl["nodes"][id];
  if (alertEntryLive(own, now)) return own["enabled"].as<bool>();
  bool anyOn = false, anyOff = false;
  for (JsonPairConst g : alertCtl["groups"].as<JsonObjectConst>()) {
    JsonVariantConst e = g.value();
    if (!alertEntryLive(e, now) || !alertGroupHas(e["members"], id)) continue;
    if (e["enabled"].as<bool>()) anyOn = true;
    else anyOff = true;
  }
  return anyOn || !anyOff;
}

// How a node is named in Discord: its pond first, as set on the website
// ("文安1池(文安) A01"), since at 3am which pond matters before which board.
// The groups are the ponds; a board in none is named by its id alone. Ready
// for a JSON body, like the rest of the message.
static String nodeLabel(const String &id) {
  for (JsonPairConst g : alertCtl["groups"].as<JsonObjectConst>()) {
    JsonVariantConst e = g.value();
    if (!alertGroupHas(e["members"], id)) continue;
    String name = e["name"] | "";
    if (name.length()) return jsonEscape(name) + " " + jsonEscape(id);
  }
  return jsonEscape(id);
}

// ---- alert batches ----------------------------------------------------
//
// Each heartbeat board watches a different supply, so several going down in
// the same minute is common (one breaker, one storm) and each one matters.
// The nodes that go stale in the same check share one Discord message -- one
// notification listing them all -- and that message is re-sent, acknowledged
// and muted as a unit. A node that fails later starts a batch of its own, so
// a fresh failure is never hidden inside an alert already ticked.
struct Batch {
  bool inUse = false;
  String msgId;          // last message posted, polled for the tick
  String older[2];       // the two before it, still polled (see pollAcks)
  time_t episodeAt = 0;  // when this round of alerting began
  time_t lastSent = 0, ackedAt = 0, lastTry = 0;
  int sent = 0;          // posts in all, shown as "第 N 次提醒"
  int epPosts = 0;       // posts this episode, which set the schedule below
  int calls = 0;         // phone rounds this episode
  bool callPost = false; // a round has rung that Discord has not yet posted
  uint32_t failures = 0; // consecutive failed posts, drives the backoff
  bool dirty = false;    // changed since batchSave() last wrote it
};
static const int MAX_BATCHES = 12;
static Batch batches[MAX_BATCHES];
static time_t lastPhoneAt = 0;

// episodeAt is when the episode began: for a node found stale, the moment it
// went stale, which may be well before now if this board was down then.
static int batchOpen(time_t episodeAt) {
  for (int i = 0; i < MAX_BATCHES; i++)
    if (!batches[i].inUse) {
      batches[i] = Batch();
      batches[i].inUse = true;
      batches[i].episodeAt = episodeAt;
      batches[i].dirty = true;
      return i;
    }
  // All in use: fold into the newest rather than leave the node unreported.
  int newest = 0;
  for (int i = 1; i < MAX_BATCHES; i++)
    if (batches[i].episodeAt > batches[newest].episodeAt) newest = i;
  return newest;
}

static int batchMembers(int b) {
  int n = 0;
  for (int i = 0; i < MAX_NODES; i++)
    if (nodeStates[i].inUse && nodeStates[i].alertActive &&
        nodeStates[i].batch == b)
      n++;
  return n;
}

// ---- surviving a reboot ---------------------------------------------------
//
// This board reboots -- updates, and for days on end a sagging supply that
// restarted it every few minutes. With the batches in RAM alone, each reboot
// re-raised every open alert from scratch: the first notification again, the
// ten minutes to the phone again, an acknowledgement forgotten. A supply that
// failed during such a stretch could ring 20 minutes late, or not at all.
//
// So each batch's progress is kept next to its alerts, as
//   /alerts/<id>/state  {episodeAt, sent, epPosts, calls, lastCall, ackedAt,
//                        msgId, older}
// on every member, rewritten whenever it changes (a handful of times per
// episode), and read back once at boot. writeAlert() clears it when a fresh
// episode starts, so it never describes an older outage.

// One multi-path PATCH for all changed batches, rather than a TLS round trip
// per node.
static void batchSave() {
  String body;
  for (int b = 0; b < MAX_BATCHES; b++) {
    Batch &B = batches[b];
    if (!B.inUse || !B.dirty) continue;
    String st = "{\"episodeAt\":" + String((uint32_t)B.episodeAt);
    st += ",\"sent\":" + String(B.sent);
    st += ",\"epPosts\":" + String(B.epPosts);
    st += ",\"calls\":" + String(B.calls);
    st += ",\"lastCall\":" + String((uint32_t)lastPhoneAt);
    st += ",\"ackedAt\":" + String((uint32_t)B.ackedAt);
    st += ",\"msgId\":\"" + B.msgId + "\"";
    st += ",\"older\":\"" + B.older[0] + "," + B.older[1] + "\"}";
    for (int i = 0; i < MAX_NODES; i++) {
      const NodeState &ns = nodeStates[i];
      if (!ns.inUse || !ns.alertActive || ns.batch != b) continue;
      body += body.length() ? "," : "{";
      body += "\"" + ns.id + "/state\":" + st;
    }
  }
  if (!body.length()) return;
  body += "}";
  if (rtdbRequest("PATCH", "/alerts.json", body, nullptr)) {
    for (int b = 0; b < MAX_BATCHES; b++) batches[b].dirty = false;
  } else {
    // Left dirty: the next check tries again.
    logLine("alert state save FAILED: %s", lastError.c_str());
  }
}

// Once, before the first check after boot. Every alert still active comes back
// as it was; the check then carries on from there -- clearing the ones whose
// node recovered meanwhile, and leaving the stale ones in their episode.
// Members of one batch share a message, so they are regrouped by it.
static bool alertsRestored = false;
static int alertsRestoreTries = 0;

static bool alertsRestore() {
  if (alertsRestored) return true;
  String body;
  if (!rtdbRequest("GET", "/alerts.json", "", &body)) {
    // Not forever: after a few tries the check goes ahead without, which at
    // worst re-raises from scratch -- the old behaviour, still an alert.
    if (++alertsRestoreTries < 3) {
      logLine("alert restore: read FAILED, retrying next check");
      return false;
    }
    logLine("alert restore: giving up, starting fresh");
    alertsRestored = true;
    return true;
  }
  alertsRestored = true;
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    logLine("alert restore: parse FAILED, starting fresh");
    return true;
  }

  String keys[MAX_BATCHES];
  int restored = 0;
  for (JsonPairConst p : doc.as<JsonObjectConst>()) {
    JsonVariantConst a = p.value();
    if (!a["active"].as<bool>()) continue;
    NodeState &ns = nodeState(p.key().c_str());
    ns.alertActive = true;
    ns.lastSeen = a["lastSeen"].as<uint32_t>();

    // Raised but never saved (this board went down within the check that
    // raised it): its own batch, from when it was raised.
    JsonVariantConst st = a["state"];
    String key = st["msgId"].as<String>();
    if (st.isNull()) key = "";
    else if (!key.length() || key == "null")
      key = "ep" + String(st["episodeAt"].as<uint32_t>());

    int b = -1;
    for (int i = 0; key.length() && i < MAX_BATCHES; i++)
      if (batches[i].inUse && keys[i] == key) b = i;
    if (b < 0) {
      time_t ep = st.isNull() ? a["firedAt"].as<uint32_t>()
                              : st["episodeAt"].as<uint32_t>();
      b = batchOpen(ep);
      keys[b] = key;
      Batch &B = batches[b];
      B.dirty = false;  // nothing new to write
      if (!st.isNull()) {
        B.sent = st["sent"] | 0;
        B.epPosts = st["epPosts"] | 0;
        B.calls = st["calls"] | 0;
        B.ackedAt = st["ackedAt"].as<uint32_t>();
        B.msgId = (key.startsWith("ep")) ? "" : key;
        String older = st["older"] | "";
        int comma = older.indexOf(',');
        if (comma >= 0) {
          B.older[0] = older.substring(0, comma);
          B.older[1] = older.substring(comma + 1);
        }
        time_t lastCall = st["lastCall"].as<uint32_t>();
        if (lastCall > lastPhoneAt) lastPhoneAt = lastCall;
      } else {
        B.dirty = true;
      }
    }
    ns.batch = b;
    restored++;
    logLine("alert restored: %s (batch %d)", ns.id.c_str(), b);
  }
  if (restored) logLine("alert restore: %d active", restored);
  return true;
}

// The message for one batch. One node reads like the old per-node alert; more
// get a line each. Renders e.g.:
//   @owner ⚠️ **3 處失聯**：文安1池(文安) A01、A05、A06（第 2 次提醒）
//   • 文安1池(文安) A01（打氣機）最後回報 14:32，已中斷 9 分鐘
//   ...
//   ⚠️ 魚塭 A 全部 10 片都失聯 → 比較可能是網路問題
//   10 分鐘內沒人確認會撥電話
//   點 ✅ 回報已知悉（靜音 60 分鐘）
static String alertText(int b, time_t now) {
  const Batch &B = batches[b];
  String list;
  int n = 0;
  NodeState *only = nullptr;
  bool sitesHit[26] = {false};
  for (int i = 0; i < MAX_NODES; i++) {
    NodeState &ns = nodeStates[i];
    if (!ns.inUse || !ns.alertActive || ns.batch != b) continue;
    if (n++) list += "、";
    list += nodeLabel(ns.id);
    only = &ns;
    int s = siteIndex(ns.id);
    if (s >= 0) sitesHit[s] = true;
  }

  String c = "<@" + String(DISCORD_USER_ID) + "> ⚠️ **";
  c += (n == 1) ? nodeLabel(only->id) + " 失聯**"
                : String(n) + " 處失聯**：" + list;
  if (B.sent > 0) c += "（第 " + String(B.sent + 1) + " 次提醒）";
  c += "\\n";

  if (n == 1) {
    if (only->note.length()) c += "說明：" + jsonEscape(only->note) + "\\n";
    c += "最後回報：" + localHhMm(only->lastSeen) + "\\n";
    c += "已中斷：" + humanDuration(only->age) + "\\n";
    if (only->reading.length()) c += "最後讀值：" + only->reading + "\\n";
  } else {
    for (int i = 0; i < MAX_NODES; i++) {
      NodeState &ns = nodeStates[i];
      if (!ns.inUse || !ns.alertActive || ns.batch != b) continue;
      c += "• " + nodeLabel(ns.id);
      if (ns.note.length()) c += "（" + jsonEscape(ns.note) + "）";
      c += " 最後回報 " + localHhMm(ns.lastSeen) + "，已中斷 " +
           humanDuration(ns.age) + "\\n";
    }
  }

  for (int s = 0; s < 26; s++)
    if (sitesHit[s] && siteTotal[s] >= 2 && siteStale[s] == siteTotal[s])
      c += "⚠️ 魚塭 " + String((char)('A' + s)) + " 全部 " +
           String(siteTotal[s]) + " 片都失聯 → 比較可能是網路問題\\n";

  time_t callFrom = callDueAt(B.episodeAt);
  if (B.calls > 0)
    c += "已撥電話 " + String(B.calls) + " 輪，每 " +
         String(CALL_REPEAT_S / 60) + " 分鐘再撥，按 ✅ 才停\\n";
  else if (callFrom > now)
    c += String((int)((callFrom - now + 59) / 60)) + " 分鐘內沒人確認會撥電話\\n";

  c += "點 ✅ 回報已知悉（靜音 " + String(ACK_MUTE_S / 60) + " 分鐘）";
  return c;
}

// Looks for a tick on each batch's latest message. Run every check, and again
// before each call of a round -- skipped then if it ran moments ago, since a
// round of several numbers asks once per number.
static uint32_t lastAckPollMs = 0;
static const uint32_t ACK_POLL_MIN_MS = 15000;

//
// The two messages before the latest count too. Woken by the phone, it is easy
// to tick the message on screen just as a newer one goes out, and a tick that
// silently did nothing would leave the phone ringing.
static void pollAcks(time_t now) {
  lastAckPollMs = millis();
  for (int b = 0; b < MAX_BATCHES; b++) {
    Batch &B = batches[b];
    if (!B.inUse || B.ackedAt || !B.msgId.length()) continue;
    if (!discordAcked(B.msgId) &&
        !(B.older[0].length() && discordAcked(B.older[0])) &&
        !(B.older[1].length() && discordAcked(B.older[1])))
      continue;
    B.ackedAt = now;
    B.dirty = true;
    notifyAcked++;
    logLine("batch %d acknowledged, muting %lu min", b,
            (unsigned long)(ACK_MUTE_S / 60));
    for (int i = 0; i < MAX_NODES; i++) {
      NodeState &ns = nodeStates[i];
      if (ns.inUse && ns.alertActive && ns.batch == b)
        rtdbRequest("PATCH", "/alerts/" + ns.id + ".json",
                    "{\"acked\":true,\"ackedAt\":" + String((uint32_t)now) + "}",
                    nullptr);
    }
  }
}

// Some open batch, unticked, has reached its first call.
static bool phoneDue(time_t now) {
  for (int b = 0; b < MAX_BATCHES; b++) {
    const Batch &B = batches[b];
    if (B.inUse && !B.ackedAt && now >= callDueAt(B.episodeAt)) return true;
  }
  return false;
}

static bool phoneStillWanted() {
  time_t now = time(nullptr);
  if (millis() - lastAckPollMs >= ACK_POLL_MIN_MS) pollAcks(now);
  return phoneDue(now);
}

// Runs every check: acknowledgements, the phone, then Discord, per batch.
static void driveBatches(time_t now) {
  // Close batches whose nodes have all recovered.
  for (int b = 0; b < MAX_BATCHES; b++)
    if (batches[b].inUse && !batchMembers(b)) batches[b].inUse = false;

  // Acknowledgements and mute expiry.
  pollAcks(now);
  for (int b = 0; b < MAX_BATCHES; b++) {
    Batch &B = batches[b];
    if (!B.inUse) continue;
    // An acknowledged alert stays quiet only for the mute window: a node still
    // dead an hour later has to speak up again, and the phone waits its turn
    // again from there.
    if (B.ackedAt && (uint32_t)(now - B.ackedAt) >= ACK_MUTE_S) {
      logLine("batch %d mute expired, resuming", b);
      B.ackedAt = 0;
      B.lastSent = 0;
      B.episodeAt = now;
      B.epPosts = 0;
      B.calls = 0;
      B.callPost = false;
      // The ticked messages are done with: polled again, their tick would
      // acknowledge the new episode before it had said a word.
      B.msgId = B.older[0] = B.older[1] = "";
      B.dirty = true;
    }
  }

  // The phone: one round however many batches are waiting. It is only there
  // to wake someone up; what failed is in Discord. Keyed to time, not to how
  // many posts succeeded, so a Discord outage cannot hold it back.
  if (!phoneDue(now)) {
    airCancelAlarm();
  } else if (!airAlarmPending() &&
             (!lastPhoneAt ||
              (uint32_t)(now - lastPhoneAt) + CALL_SLACK_S >= CALL_REPEAT_S)) {
    lastPhoneAt = now;
    for (int b = 0; b < MAX_BATCHES; b++) {
      Batch &B = batches[b];
      if (B.inUse && !B.ackedAt && now >= callDueAt(B.episodeAt)) {
        B.calls++;
        B.callPost = true;
        B.dirty = true;
      }
    }
    logLine("unacknowledged alerts, phoning %d number(s)", callNumberCount);
    airDial(true);
  }

  // Discord, paced per batch and backed off after failures (doubling, capped
  // at 16x) so an unreachable Discord does not cost a failing TLS round trip
  // every minute.
  for (int b = 0; b < MAX_BATCHES; b++) {
    Batch &B = batches[b];
    if (!B.inUse || B.ackedAt) continue;
    // Before the phone: on a fixed timetable from the episode start -- 0, 5,
    // 10 minutes -- not RENOTIFY_S after the previous post. Checks run once a
    // minute, so pacing off the last post let each one slip by up to a check
    // and the third landed a minute or two after the phone, which is due at
    // 10 minutes.
    //
    // Once the phone has started: one post per round, in the same check, so
    // whoever the ringing wakes finds a fresh message at the top to tick. On
    // separate clocks (w56) the phone rang with no message to match and the
    // two read as out of step.
    if (B.calls) {
      if (!B.callPost) continue;
    } else if (B.epPosts &&
               now < B.episodeAt + (time_t)B.epPosts * RENOTIFY_S) {
      continue;
    }
    if (B.failures) {
      uint32_t wait = RENOTIFY_S << (B.failures - 1 < 4 ? B.failures - 1 : 4);
      if ((uint32_t)(now - B.lastTry) < wait) continue;
    }
    B.lastTry = now;
    String msgId = postAlert(alertText(b, now));
    if (msgId.length()) {
      B.older[1] = B.older[0];
      B.older[0] = B.msgId;
      B.msgId = msgId;
      B.callPost = false;
      B.lastSent = now;
      B.sent++;
      // Skip to the current slot rather than catch up on missed ones: after a
      // Discord outage the timetable is behind, and catching up would post
      // once a minute until it was not.
      int slot = (int)((now - B.episodeAt) / RENOTIFY_S) + 1;
      B.epPosts = B.epPosts + 1 > slot ? B.epPosts + 1 : slot;
      B.failures = 0;
      B.dirty = true;
      notifySent++;
      logLine("batch %d notified (%d nodes, #%d)", b, batchMembers(b), B.sent);
    } else {
      B.failures++;
      logLine("batch %d notify FAILED (#%lu)", b, (unsigned long)B.failures);
    }
  }

  batchSave();
}

static int batchesOpen() {
  int n = 0;
  for (int b = 0; b < MAX_BATCHES; b++) n += batches[b].inUse;
  return n;
}

// ---- first sighting -----------------------------------------------------
//
// Each node is announced once, the first time it is seen alive -- so wiring up
// twenty boards on the bench reports itself board by board, and a board that
// was never registered (it publishes as esp-<mac>) is pointed out. Who has
// been announced is kept in NVS, or every reboot of this board would announce
// every node again. The very first run after this was added adopts whatever
// already exists without a word.
static Preferences seenPrefs;
static String seenIds;  // ",A01,A05,pond-site,"
static bool seenLoaded = false, seenSeeding = false, seenDirty = false;

static void seenLoad() {
  if (seenLoaded) return;
  seenLoaded = true;
  seenPrefs.begin("seen", true);
  seenSeeding = !seenPrefs.isKey("ids");
  seenIds = seenPrefs.getString("ids", ",");
  seenPrefs.end();
}

// Returns true when `id` has not been seen before (and records it).
static bool seenFirst(const String &id) {
  seenLoad();
  String key = "," + id + ",";
  if (seenIds.indexOf(key) >= 0) return false;
  seenIds += id + ",";
  seenDirty = true;
  return !seenSeeding;
}

static void seenSave() {
  if (!seenDirty) return;
  seenDirty = false;
  seenSeeding = false;
  seenPrefs.begin("seen", false);
  seenPrefs.putString("ids", seenIds);
  seenPrefs.end();
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

  NodeState &ns = nodeState(id);

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
             " \\u63db\\u4e86\\u7db2\\u8def**\\n";
  m += "\\u539f\\u672c\\uff1a" + jsonEscape(from) + "\\n";
  m += "\\u73fe\\u5728\\uff1a" + jsonEscape(ssid) + "\\n";
  if (ip.length()) {
    m += "\\n\\u8981\\u6539\\u5b83\\u7684 WiFi \\u8acb\\u9ede\\uff1a\\n";
    m += "http://" + jsonEscape(ip) + "/wifi?pass=" + String(OTA_PASSWORD);
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

  alertControlLoad();
  if (!alertsRestore()) return;  // not yet: re-raising would start them over

  time_t now = time(nullptr);
  int stale = 0, healthy = 0, loud = 0;
  String summary, recovered, firsts;
  int recoveredCount = 0;
  int newBatch = -1;  // opened lazily, for the nodes that go stale this check
  memset(siteTotal, 0, sizeof(siteTotal));
  memset(siteStale, 0, sizeof(siteStale));
  for (int i = 0; i < MAX_NODES; i++) nodeStates[i].seenThisCheck = false;

  // Walk the top-level device ids. Each looks like  "pond-site":{...}
  int pos = 1;  // skip the opening brace
  while (true) {
    int q1 = json.indexOf('"', pos);
    if (q1 < 0) break;
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 < 0) break;
    String id = json.substring(q1 + 1, q2);

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

    NodeState &ns = nodeState(id);
    ns.seenThisCheck = true;
    ns.age = age;
    ns.note = "";
    extractString(obj, 0, "note", &ns.note);
    // Surface whatever the node last measured; a heartbeat board has nothing.
    ns.reading = "";
    double temp;
    if (extractNumber(obj, 0, "temp", &temp))
      ns.reading = String(temp, 1) + " \\u00b0C";

    // A node publishing happily from an unexpected network has been carried
    // somewhere -- see announceNodeMove.
    announceNodeMove(id, obj, isStale);

    // First time this node has ever been seen alive.
    // Not on the development bench, where boards are flashed and tested by
    // the dozen: arriving at a pond is a board's real first time online. Not
    // recorded as seen there either, so that arrival is still announced.
    String ssid;
    extractString(obj, 0, "ssid", &ssid);
    if (!isStale && ssid != BENCH_SSID && seenFirst(id)) {
      String mac;
      double rssi = 0;
      extractString(obj, 0, "mac", &mac);
      bool hasRssi = extractNumber(obj, 0, "rssi", &rssi);
      if (id.startsWith("esp-")) {
        firsts += "❓ **發現未登記的板子**：" + jsonEscape(id) + "\\n";
        if (mac.length()) firsts += "MAC：" + jsonEscape(mac) + "\\n";
        firsts += "請用 tools/register.sh <編號> <COM埠> --no-flash 登記\\n";
      } else {
        firsts += "🆕 **" + jsonEscape(id) + " 首次上線**";
        if (ns.note.length()) firsts += "（" + jsonEscape(ns.note) + "）";
        firsts += "\\n";
        if (ssid.length()) {
          firsts += "WiFi：" + jsonEscape(ssid);
          if (hasRssi) firsts += "，訊號 " + String((int)rssi) + " dBm";
          firsts += "\\n";
        }
      }
      logLine("%s seen for the first time", id.c_str());
    }

    bool wasMuted = ns.muted;
    ns.muted = !alertsEnabledFor(id, now);
    if (ns.muted != wasMuted)
      logLine("%s alerts switched %s", id.c_str(), ns.muted ? "off" : "on");

    // Only nodes whose alerts are on count toward "the whole pond is down": a
    // board switched off is usually unplugged on purpose (packed away, under
    // repair), and counting it made one real outage read as the pond's
    // network failing.
    int site = siteIndex(id);
    if (site >= 0 && !ns.muted) {
      siteTotal[site]++;
      if (isStale) siteStale[site]++;
    }

    // A restored alert whose node came back and went quiet again while this
    // board was down: a different outage. Close the old one quietly -- the
    // node's own /outages record has its true end -- and raise the new one.
    if (isStale && ns.alertActive && ts > 0 && (time_t)ts != ns.lastSeen) {
      writeAlert(id, false, ns.lastSeen, "replaced by a later outage");
      writeOutage(id, ns.lastSeen, false, 0, "");
      ns.alertActive = false;
      ns.batch = -1;
      logLine("%s: earlier alert closed, it recovered and failed again unwatched",
              id.c_str());
    }

    // Transition detection uses state this device already holds, rather than
    // re-fetching /alerts/<id>/active every minute: each such fetch was a full
    // TLS handshake to re-read a boolean we wrote ourselves.
    if (isStale && !ns.alertActive) {
      String reason = "no publish for " + String(age) + "s (limit " +
                      String(limit) + "s)";
      if (writeAlert(id, true, (time_t)ts, reason)) {
        alertsRaised++;
        ns.alertActive = true;
        ns.lastSeen = (time_t)ts;
        writeOutage(id, ns.lastSeen, true, 0, reason);
        // Switched off: recorded like any other, but in no batch, so nothing
        // posts and nothing rings.
        // The episode runs from when it went stale, so a board that was
        // itself down then catches up at once rather than starting the clock
        // now. Nodes raised together share the earliest start.
        time_t wentStale = ts > 0 ? (time_t)ts + limit : now;
        if (wentStale > now) wentStale = now;
        if (!ns.muted) {
          if (newBatch < 0) newBatch = batchOpen(wentStale);
          if (wentStale < batches[newBatch].episodeAt)
            batches[newBatch].episodeAt = wentStale;
          batches[newBatch].dirty = true;
          ns.batch = newBatch;
        }
        logLine("ALERT RAISED %s: %s (batch %d%s)", id.c_str(), reason.c_str(),
                ns.batch, ns.muted ? ", switched off" : "");
      } else {
        logLine("failed to raise alert for %s: %s", id.c_str(),
                lastError.c_str());
      }
    } else if (!isStale && ns.alertActive) {
      if (writeAlert(id, false, (time_t)ts, "publishing again")) {
        alertsCleared++;
        ns.alertActive = false;
        ns.batch = -1;
        // How long it was down: from its last beat before going quiet to the
        // first one back.
        uint32_t down = (ns.lastSeen && (time_t)ts > ns.lastSeen)
                            ? (uint32_t)((time_t)ts - ns.lastSeen) : 0;
        writeOutage(id, ns.lastSeen, false, (time_t)ts, "");
        if (!ns.muted) {
          recovered += "✅ **" + nodeLabel(id) + " 恢復**";
          if (ns.note.length()) recovered += "（" + jsonEscape(ns.note) + "）";
          if (down) recovered += "，中斷 " + humanDuration(down);
          recovered += "\\n";
          recoveredCount++;
        }
        logLine("alert cleared %s (down %lus)", id.c_str(), (unsigned long)down);
      }
    } else if (ns.alertActive && ns.muted && ns.batch >= 0) {
      // Switched off mid-alert: out of its batch, which closes once empty --
      // and with it any call still due.
      ns.batch = -1;
      logLine("%s switched off while alerting, dropped from its batch",
              id.c_str());
    } else if (ns.alertActive && !ns.muted && ns.batch < 0) {
      // Switched back on while still down: speaks up now, as a fresh alert.
      if (newBatch < 0) newBatch = batchOpen(now);
      batches[newBatch].dirty = true;
      ns.batch = newBatch;
      logLine("%s switched on while down, alerting (batch %d)", id.c_str(),
              newBatch);
    }

    if (isStale) stale++; else healthy++;
    if (isStale && !ns.muted) loud++;
  }

  // A node deleted from /devices while alerting can never recover; drop it so
  // its batch can close.
  for (int i = 0; i < MAX_NODES; i++) {
    NodeState &ns = nodeStates[i];
    if (ns.inUse && !ns.seenThisCheck && ns.alertActive) {
      ns.alertActive = false;
      ns.batch = -1;
      logLine("%s no longer in /devices, dropped", ns.id.c_str());
    }
  }

  // Said without a mention: good news and new boards do not need to buzz
  // anyone's phone.
  if (recoveredCount) discordSay(recovered);
  if (firsts.length()) discordSay(firsts);
  seenSave();

  driveBatches(now);

  // Red LED and "alerting" only for what is actually allowed to alert.
  anyAlertActive = (loud > 0);
  lastSummary = summary.length() ? summary : "no other devices found";
  logLine("check: %d ok, %d stale, %d batches | heap=%u blk=%u | %s", healthy,
          stale, batchesOpen(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap(), lastSummary.c_str());
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
  // The phone path. The dialer has no internet of its own, so this is the
  // only place its health is visible from outside: `ok` is the one-glance
  // answer (answering PINGs and registered for VoLTE), the rest says why not.
  // Deliberately no "ts" key in here: runCheck() on any watcher takes the
  // first "ts" under a device as its heartbeat.
  body += ",\"air\":{";
  body += "\"ok\":" + String(airHealthy() ? "true" : "false");
  body += ",\"csq\":" + String(air.csq);
  body += ",\"reg\":" + String(air.reg);
  body += ",\"ims\":" + String(air.ims);
  body += ",\"pongAge\":" +
          String(air.everAnswered ? (long)((millis() - air.lastPongMs) / 1000)
                                  : -1L);
  body += ",\"calls\":" + String(air.calls);
  body += ",\"last\":\"" + jsonEscape(air.lastOutcome) + "\"";
  body += ",\"restarts\":" + String(air.restarts);
  body += ",\"ver\":\"" + jsonEscape(air.version) + "\"";
  body += "}";
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
// watching when a pond node goes quiet at 3am. See alertText().
//
// `content` is JSON string source, not plain text: callers build it with
// \uXXXX escapes and run jsonEscape() over anything interpolated.
static bool discordSay(const String &content) {
  String body = "{\"content\":\"" + content + "\"}";
  bool ok = discordRequest("POST",
                           "/channels/" + String(DISCORD_CHANNEL_ID) +
                               "/messages",
                           body, nullptr);
  // Logged here rather than left to the callers. Every one of them treats this
  // as fire-and-forget, so a rejected body -- a stray newline or a single
  // backslash in the hand-built JSON -- looked identical to the message simply
  // not being due. Two of them shipped broken that way.
  if (!ok) logLine("discordSay FAILED: %s", lastError.c_str());
  return ok;
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

  // Says which branch below was taken, and why. Two broken releases in a row
  // came down to guessing at these flags from the outside; the board knows.
  logLine("wifi state: knownSite=%d rescue=%d reverted=%d confirmed=%d "
          "attempted=%s on=%s",
          onKnownSite, onRescueNetwork, wifiReverted, wifiJustConfirmed,
          wifiAttemptedSsid.length() ? wifiAttemptedSsid.c_str() : "(none)",
          connectedSsid);

  // Landing anywhere other than the configured target is worth saying out
  // loud, whichever network it turned out to be. Either somebody moved the
  // board -- which is the recovery plan working, and they are waiting for this
  // message to tell them where it went -- or the pond network is down, which
  // needs to be known. Both carry the setup URL, because whoever reads this is
  // the person who can act on it, and tapping the link is the whole procedure.
  if (onKnownSite) {
    String m = "\\u2139\\ufe0f **" + String(DEVICE_ID) +
               " \\u4e0d\\u5728\\u76ee\\u6a19\\u7db2\\u8def\\u4e0a**\\n";
    if (wifiAttemptedSsid.length())
      m += "\\u76ee\\u6a19\\uff1a" + jsonEscape(wifiAttemptedSsid) +
           "\\uff08\\u9023\\u4e0d\\u4e0a\\uff09\\n";
    m += "\\u76ee\\u524d\\u9023\\u4e0a\\uff1a" +
         jsonEscape(String(connectedSsid)) + "\\n\\n";
    m += "\\u8981\\u6539\\u76ee\\u6a19 WiFi \\u8acb\\u9ede\\uff1a\\n";
    m += "http://" + ip + "/wifi?pass=" + String(OTA_PASSWORD);
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
    m += "http://" + ip + "/wifi?pass=" + String(OTA_PASSWORD);
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

  // Checked before the ssid, so clear=1 needs no other parameter.
  if (server.arg("clear") == "1") {
    clearTargets();
    logLine("wifi targets cleared, rebooting");
    // savedPage renders into HTML, not into a JSON body, so the Chinese goes
    // in as itself rather than as \uXXXX escapes.
    server.send(200, "text/html; charset=utf-8",
                savedPage(String(F("(已清除，回到預設網路)"))));
    delay(1200);
    ESP.restart();
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
  String muted;
  for (int i = 0; i < MAX_NODES; i++)
    if (nodeStates[i].inUse && nodeStates[i].muted)
      muted += (muted.length() ? ", " : "") + nodeStates[i].id;
  b += "alerts switched off: " + (muted.length() ? muted : String("none")) + "\n";
  b += "discord sent: " + String(notifySent) + "\n";
  b += "discord acked: " + String(notifyAcked) + "\n";
  b += "alert batches open: " + String(batchesOpen()) + "\n";
  b += "bot id: " + (botUserId.length() ? botUserId : String("UNKNOWN")) + "\n";
  b += "dialer: " + String(airHealthy() ? "ok" : "NOT OK") + "  csq=" +
       String(air.csq) + " reg=" + String(air.reg) + " ims=" + String(air.ims);
  if (air.everAnswered)
    b += "  pong " + String((millis() - air.lastPongMs) / 1000) + "s ago";
  else
    b += "  never answered";
  b += "\n";
  b += "dialer calls: " + String(air.calls) + ", last: " + air.lastOutcome +
       ", restarts: " + String(air.restarts) +
       (dialInFlight ? "  [CALLING " + dialNumber + "]" : String("")) + "\n";
  b += "call numbers (" + callNumbersFrom + "): " + callNumbersJoined() + "\n";
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
  b += "boot count: " + String(prevRun.bootCount + 1) + "\n";
  b += "wifi at boot: " +
       (bootAttempts.length() ? bootAttempts : String("(none)"));
  if (bootPasses) b += "  [" + String(bootPasses) + " failed passes]";
  b += "\n";
  if (prevRun.lastBeat) {
    time_t nowT = time(nullptr);
    b += "prev run: up " + String(prevRun.uptime) + "s, last beat ";
    b += (nowT > (time_t)prevRun.lastBeat
              ? String((uint32_t)(nowT - (time_t)prevRun.lastBeat)) + "s ago"
              : String("(clock moved)"));
    b += "\n";
  }
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

// Rings every configured number now, for checking the phone path without
// taking a node down. Guarded like /wifi: each call can cost money if
// answered, so it is not something a passer-by on the LAN should trigger.
//   GET /testcall?pass=<OTA_PASSWORD>
static void handleTestCall() {
  if (!configAuthorised()) {
    server.send(403, "text/plain; charset=utf-8", "bad pass");
    return;
  }
  airDial(false);
  server.send(200, "text/plain; charset=utf-8",
              "queued " + String(callNumberCount) +
                  " call(s); the result shows on / as \"dialer calls\"\n");
}

static void handleTestAlert() {
  // Shaped like a real single-node alert, so the path a real one takes --
  // post, pre-seeded tick, then /testack -- can be checked end to end.
  String msgId = postAlert("<@" + String(DISCORD_USER_ID) +
                           "> ⚠️ **TEST 失聯**（手動測試）\\n"
                           "點 ✅ 回報已知悉（靜音 " + String(ACK_MUTE_S / 60) +
                           " 分鐘）");
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

  // The dialer link. PWR_EN floats, which keeps the module powered; it is only
  // driven to power-cycle it.
  pinMode(AIR_EN_PIN, INPUT);
  Serial1.begin(115200, SERIAL_8N1, AIR_RX_PIN, AIR_TX_PIN);
  callNumbersLoad();

  // Before WiFi: reads what the previous run left behind, and counts this
  // boot. Must happen even if everything below fails.
  incidentBegin();

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
    bootPasses = ++wifiTries;
    if (wifiTries >= WIFI_BOOT_ATTEMPTS) {
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

  // And the machine-readable version, for the failures nobody was awake for.
  incidentReport(RTDB_HOST, DEVICE_ID);

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
  server.on("/testcall", handleTestCall);
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
  airService();

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

  // Records that the board was alive and well at this moment. Rate-limited
  // internally to once a minute, so calling it from the cycle is cheap.
  incidentHeartbeat();

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
