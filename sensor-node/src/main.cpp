// ESP32-A : read the QX-DT01P probe over RS485 and publish to Firebase RTDB.
//
// Modbus parameters were found by brute-force sweep (see git history / A1):
//   9600 8N1, slave id 1, function 0x03
//   reg 0x0000 = temperature x10   reg 0x0001 = humidity x10
// Note the UART pin roles are the reverse of the module's silkscreen naming:
// GPIO17 is the ESP32's RX, GPIO18 its TX. The A+/B- polarity is correct as
// wired (the inverted-polarity sweeps found nothing).
//
// Publishing (see RTDB layout in the project notes):
//   /devices/esp32-a/latest      overwritten every UPLOAD_INTERVAL -- this is
//                                the heartbeat ESP32-B watches
//   /history/esp32-a/<YYYY-MM>/<unix_ts>   appended every HISTORY_INTERVAL
// Both go out in a single multi-path PATCH so a history sample costs no extra
// request. Sharding history by month keeps any one node small and lets old
// data be dropped by deleting a whole child.
//
// A status page is served on port 80 for diagnostics -- this board's serial
// console does not reach the host, so HTTP is the only console we have.
//
// Wiring (isolated TTL<->RS485 module):
//   module VCC1 -> ESP32 3V3      module A+ -> probe white (A)
//   module GND1 -> ESP32 GND      module B- -> probe yellow (B)
//   module Tx   -> ESP32 GPIO17   module VCC2 -> 12V+
//   module Rx   -> ESP32 GPIO18   module GND2 -> 12V-
// The module is opto-isolated: do NOT bridge GND1 and GND2.

#include <ArduinoOTA.h>
#include <Arduino.h>
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
static const int PIN_RS485_RX = 17;  // ESP32 RX <- module Tx
static const int PIN_RS485_TX = 18;  // ESP32 TX -> module Rx
static const int LED_PIN = 48;

static const uint32_t RS485_BAUD = 9600;
static const uint8_t  SLAVE_ID   = 1;
static const uint16_t REG_TEMP   = 0x0000;  // value is degrees C x10
static const uint16_t REG_HUMID  = 0x0001;  // value is %RH x10

// Identity is tied to what is measured, not to the hardware: swapping in a
// replacement board keeps the history continuous. Water temperature is a
// site-wide reading (surface temp barely differs between ponds), so it lives
// under a site-scoped id rather than under any one pond. Per-pond sensors
// (dissolved oxygen, pH) will get their own ids later.
static const char *DEVICE_ID = "pond-site";
static const char *DEVICE_NAME = "fish-pond-site";
static const char *DEVICE_SCOPE = "site";
static const char *FW_VERSION = "a12-2026.09.04-TEST";
// Monotonic; RTDB /firmware/pond-site/version is compared against this to
// decide whether a pull-based update is due. Bump on every release.
static const uint32_t FW_VERSION_CODE = 12;

// The probe's second register tracks temperature inversely and in lockstep
// (~3% per degree), so it is derived rather than an independent humidity
// measurement -- its 98-101% readings in a 28C air-conditioned room are not
// credible. Water temperature is what this project needs, and that register
// verified correct against a hand-warming test. Dropped: meta.sensors follows,
// so consumers need no change.
static const bool PUBLISH_HUMIDITY = false;

// Heartbeat cadence. `latest` is tiny, so a fast refresh costs almost no
// bandwidth but lets B detect a dead device in ~3 min instead of ~15.
static const uint32_t UPLOAD_INTERVAL_MS  = 60UL * 1000;
static const uint32_t HISTORY_INTERVAL_MS = 5UL * 60 * 1000;

static const uint32_t REPLY_TIMEOUT_MS = 200;

// The first poll runs a minute after boot, so power-cycling a board is
// itself a way to pull an update promptly -- useful when a relative can
// reach the plug but nothing can reach the board. Steady-state polling is
// half-hourly, which is frequent enough for firmware and keeps the
// request count negligible.
static const uint32_t FW_POLL_FIRST_MS = 60UL * 1000;
static const uint32_t FW_POLL_INTERVAL_MS = 30UL * 60 * 1000;

// The default task WDT (5s, watching IDLE0) aborts a pull update on this
// board: PullOta erases the whole 6.4MB target slot in one synchronous call,
// which starves IDLE0 well past 5s (measured on v6: three identical
// task_wdt/IDLE0 aborts ~9s into the download, ipc0 running -- flash ops).
// Re-arming the same WDT at 120s both survives that erase and adds what v6
// never had: a reboot if the loop itself ever hangs. Sized like the
// watchdog's: one pull makes several blocking TLS calls plus the erase, so
// anything past two minutes is a genuine stall, not slowness.
static const uint32_t WDT_TIMEOUT_S = 120;

// ---- state ------------------------------------------------------------
// Full passes over the candidate networks before giving up and rebooting.
// Each pass is a 12s timeout per configured network, so four passes is roughly
// three to five minutes -- long enough for someone to notice the purple LED
// and switch a hotspot on, and short enough that a firmware which genuinely
// cannot connect still reaches the reboot that lets pull_ota roll it back.
static const uint32_t WIFI_BOOT_ATTEMPTS = 4;

static WebServer server(80);

static float lastTemp = NAN, lastHumid = NAN;
static time_t lastReadTime = 0;
static uint32_t uploadOk = 0, uploadFail = 0, readFail = 0;
static String lastError = "none";
static bool lastCycleOk = false;  // drives the idle heartbeat colour
static volatile bool otaInProgress = false;

static String logBuf;
static const size_t LOG_MAX = 8000;

// Two timed samples of the same 8-byte raw flash read, one from a global
// constructor (before initArduino()) and one from the top of setup(). The
// read crosses a 32-byte boundary, which is the exact shape the watchdog
// board's qio misread corrupted -- this board's raw read path has never been
// measured (its v5->v6 update succeeding is only indirect evidence), so these
// two lines on the status page are its qualification. Reads app1+0x1c, same
// window as the watchdog so the boards are directly comparable.
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

static void logLine(const char *fmt, ...) {
  char line[200];
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

  logBuf += stamp;
  logBuf += line;
  logBuf += '\n';
  if (logBuf.length() > LOG_MAX) {
    int cut = logBuf.indexOf('\n', logBuf.length() - LOG_MAX);
    logBuf.remove(0, cut < 0 ? logBuf.length() / 2 : cut + 1);
  }
}

// The onboard RGB LED is a WS2812, which needs the addressable-LED protocol
// rather than a level on the pin -- digitalWrite() leaves it dark, which is
// why earlier status blinks were invisible. Colour carries the meaning so the
// board is diagnosable on site, where there is no console and no network
// access to the status page:
//   green  = healthy publish        blue = connecting
//   red    = sensor read failed     yellow = publish failed
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
  // neopixelWrite, not rgbLedWrite: the latter only exists in Arduino-ESP32
  // 3.x and this project builds against 2.0.x. Driving the same pin with
  // digitalWrite would corrupt the WS2812 bit timing, so this is the only
  // write to it.
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

static void blink(int times, int onMs) { blinkColor(GREEN, times, onMs); }

// RTDB bodies here are hand-built JSON, so a value that could close the
// string early has to be scrubbed. An SSID is chosen by whoever owns the
// network and can perfectly well contain a quote or a backslash.
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

// Set when the board ended up on the rescue hotspot rather than a real
// network. Reported on the status page and in the heartbeat, which is
// how the watchdog notices and announces it -- this board has no Discord
// credentials of its own.
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

// What this boot tried and how it went. Read by the status page and by the
// config form, which shows the failed SSID so whoever is fixing it can see
// what was actually attempted. RAM only: it describes this boot.
static String wifiAttemptedSsid = "";
static bool wifiReverted = false;
// True only on the boot where a target that was still unproven connected for
// the first time -- the one boot worth announcing as a success.
static bool wifiJustConfirmed = false;
// True when the target was unreachable and one of the known sites answered
// instead: the board has been moved, or the pond network is down. This board
// says so on its status page and through the ssid it publishes; the watchdog
// is what turns that into a message.
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

// ---- modbus rtu -------------------------------------------------------
static uint16_t crc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// Read `count` consecutive registers into `out`. Hand-rolled rather than via
// ModbusMaster so the reply timeout is ours to choose.
static bool readRegs(uint16_t reg, uint16_t count, uint16_t *out) {
  uint8_t req[8] = {SLAVE_ID,          0x03,
                    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
                    (uint8_t)(count >> 8), (uint8_t)(count & 0xFF)};
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  while (Serial1.available()) Serial1.read();
  Serial1.write(req, 8);
  Serial1.flush();

  uint8_t resp[64];
  size_t n = 0;
  uint32_t deadline = millis() + REPLY_TIMEOUT_MS;
  while (millis() < deadline && n < sizeof(resp)) {
    if (Serial1.available()) {
      resp[n++] = Serial1.read();
      deadline = millis() + 10;  // an inter-byte gap ends the frame
    }
  }

  size_t want = 3 + count * 2 + 2;
  if (n < want) return false;
  if (resp[0] != SLAVE_ID || resp[1] != 0x03) return false;
  if (crc16(resp, n - 2) != (uint16_t)(resp[n - 2] | (resp[n - 1] << 8)))
    return false;

  for (uint16_t i = 0; i < count; i++)
    out[i] = (uint16_t)(resp[3 + i * 2] << 8) | resp[4 + i * 2];
  return true;
}

// Temperature and humidity share one request: they are adjacent registers.
static bool readProbe(float *tempC, float *humidPct) {
  uint16_t regs[2];
  if (!readRegs(REG_TEMP, 2, regs)) return false;
  *tempC = (int16_t)regs[0] / 10.0f;  // negatives are two's complement
  *humidPct = (int16_t)regs[1] / 10.0f;
  return true;
}

// ---- rtdb -------------------------------------------------------------

// One multi-path PATCH at the database root updates `latest` and (when due)
// appends a history sample, at the cost of a single request.
static bool publish(float tempC, float humidPct, time_t ts, bool withHistory) {
  struct tm tm;
  gmtime_r(&ts, &tm);
  char month[8];
  snprintf(month, sizeof(month), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);

  // Field names are spelled out rather than abbreviated: the extra bytes cost
  // ~350KB/month against a 1GB allowance, which buys a schema that is still
  // readable months from now.
  String sample = "\"temp\":" + String(tempC, 1);
  if (PUBLISH_HUMIDITY) sample += ",\"humid\":" + String(humidPct, 1);

  String body = "{";
  body += "\"devices/" + String(DEVICE_ID) + "/latest\":{";
  body += "\"ts\":" + String((uint32_t)ts) + "," + sample;
  // Where the board actually is. This board has no Discord credentials, so
  // the watchdog is what turns a change here into a message -- and the IP is
  // the only way to reach the config page, on a private network that cannot
  // be scanned from anywhere else.
  body += ",\"ssid\":\"" + jsonEscape(String(connectedSsid)) + "\"";
  body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  body += "}";
  if (withHistory) {
    body += ",\"history/" + String(DEVICE_ID) + "/" + month + "/" +
            String((uint32_t)ts) + "\":{" + sample + "}";
  }
  body += "}";

  WiFiClientSecure client;
  client.setInsecure();  // RTDB over TLS without pinning a CA on the device
  client.setTimeout(10000);

  HTTPClient http;
  String url = String("https://") + RTDB_HOST + "/.json";
  if (!http.begin(client, url)) {
    lastError = "http.begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int code = http.sendRequest("PATCH", body);
  bool ok = (code == 200);
  if (!ok) {
    lastError = "PATCH " + String(code) + ": " + http.getString().substring(0, 120);
  }
  http.end();
  return ok;
}

// Written once per boot. `interval` and `sensors` exist so consumers need no
// hardcoded knowledge of this device: ESP32-B derives its staleness threshold
// from `interval`, and a dashboard learns which series exist from `sensors`.
static bool publishMeta() {
  String body = "{";
  body += "\"name\":\"" + String(DEVICE_NAME) + "\"";
  body += ",\"scope\":\"" + String(DEVICE_SCOPE) + "\"";
  body += ",\"model\":\"QX-DT01P\"";
  body += ",\"fw\":\"" + String(FW_VERSION) + "\"";
  body += ",\"interval\":" + String(UPLOAD_INTERVAL_MS / 1000);
  body += ",\"sensors\":[\"temp\"";
  if (PUBLISH_HUMIDITY) body += ",\"humid\"";
  body += "]";
  body += ",\"bootAt\":" + String((uint32_t)time(nullptr));
  body += "}";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  HTTPClient http;
  String url = String("https://") + RTDB_HOST + "/devices/" + DEVICE_ID +
               "/meta.json";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.sendRequest("PUT", body);
  http.end();
  return code == 200;
}

// ---- http status page -------------------------------------------------
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

// No Discord from this board. It has no token, and adding one would put a
// second copy of the credentials on a device that sits at a pond -- so where
// this board ended up is reported in the heartbeat instead (see publish), and
// the watchdog is what notices a change and announces it. That keeps the
// Discord credentials on exactly one board.

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
static void handleRoot() {
  String b = "=== ESP32-A ===\n";
  b += "ssid: " + String(connectedSsid);
  if (onRescueNetwork) b += "  [RESCUE HOTSPOT]";
  else if (onKnownSite) b += "  [NOT THE TARGET NETWORK]";
  b += "\n";
  {
    Credentials t = storedTarget();
    b += "target: " +
         (t.ssid.length() ? t.ssid : String("(none, using defaults)"));
    if (targetPending()) b += "  [unproven]";
    b += "\n";
    Credentials pv = storedPrevious();
    if (pv.ssid.length()) b += "target prev: " + pv.ssid + "\n";
    b += "rescue: " + storedRescue().ssid + "\n";
    if (wifiReverted)
      b += "note: " + wifiAttemptedSsid + " failed, reverted this boot\n";
    if (wifiJustConfirmed)
      b += "note: target confirmed on this boot\n";
  }
  b += "ip: " + WiFi.localIP().toString() + "\n";
  b += "rssi: " + String(WiFi.RSSI()) + " dBm\n";
  b += "uptime: " + String(millis() / 1000) + "s\n";
  b += "free heap: " + String(ESP.getFreeHeap()) + "\n\n";
  b += "last temp: " + (isnan(lastTemp) ? "n/a" : String(lastTemp, 1) + " C") + "\n";
  b += "last humid: " + (isnan(lastHumid) ? "n/a" : String(lastHumid, 1) + " %") + "\n";
  b += "last read ts: " + String((uint32_t)lastReadTime) + "\n\n";
  b += "uploads ok: " + String(uploadOk) + "\n";
  b += "uploads failed: " + String(uploadFail) + "\n";
  b += "probe read fails: " + String(readFail) + "\n";
  b += "last error: " + lastError + "\n";
  b += "task wdt: " + String(WDT_TIMEOUT_S) + "s\n";
  b += "fw version: " + String(otaPullVersion()) + "\n";
  b += "fw pull: " + otaPullStatus() + " (tries " +
       String(otaPullAttempts()) + ", fails " +
       String(otaPullFailures()) + ")\n";
  b += "raw sample ctor : " + String(esp_err_to_name(rawSampleEarlyErr)) +
       " " + probeHex(rawSampleEarly, 8) + "\n";
  b += "raw sample setup: " + String(esp_err_to_name(rawSampleSetupErr)) +
       " " + probeHex(rawSampleSetup, 8) + "\n";
  if (otaLastProbe.length()) {
    b += "---- last flash probe ----\n";
    b += otaLastProbe;
  }
  b += "===============\n\n";
  b += logBuf;
  server.send(200, "text/plain; charset=utf-8", b);
}

// Force an immediate read+publish, for testing without waiting a minute.
static void handleNow() {
  float t, h;
  if (!readProbe(&t, &h)) {
    server.send(500, "text/plain", "probe read failed\n");
    return;
  }
  time_t ts = time(nullptr);
  bool ok = publish(t, h, ts, false);
  server.send(ok ? 200 : 500, "text/plain",
              "t=" + String(t, 1) + " h=" + String(h, 1) +
                  (ok ? " published\n" : " publish FAILED: " + lastError + "\n"));
}

// Forces a firmware check now rather than waiting out the poll interval.
static void handleFwCheck() {
  otaPullCheck(RTDB_HOST);
  // An applied update reboots inside the call above, so reaching
  // here means nothing was installed.
  server.send(200, "text/plain; charset=utf-8",
              "running v" + String(otaPullVersion()) + "\nresult: " +
                  otaPullStatus() + "\n");
}

// Runs the flash read-path probe against the slot the next update would use,
// on demand. Same tool that diagnosed the watchdog's VALIDATE_FAILED: it says
// whether the raw SPI path and the mmap/cache path agree on what is there.
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

void setup() {
  rawSampleSetupErr = sampleCrossingRead(rawSampleSetup);

  blinkColor(BLUE, 2, 120);

  Serial1.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);

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
  // broken update in the one state it was designed to escape.
  uint32_t wifiTries = 0;
  while (!connectWiFi()) {
    if (++wifiTries >= WIFI_BOOT_ATTEMPTS) {
      logLine("no wifi after %lu attempts, rebooting",
              (unsigned long)wifiTries);
      setLed(PURPLE);
      delay(2000);
      ESP.restart();
    }
    for (int i = 0; i < 5; i++) blinkColor(PURPLE, 1, 500);
  }
  blinkColor(GREEN, 5, 60);

  // History keys and heartbeat freshness are both wall-clock based, so NTP
  // must land before the first publish.
  // Taiwan is UTC+8. Stored timestamps are unix seconds either way, but
  // every human-readable time -- the log lines and the Discord alert --
  // goes through localtime_r(), so without the offset those all read 8
  // hours early.
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  uint32_t ntpDeadline = millis() + 15000;
  while (time(nullptr) < 1600000000 && millis() < ntpDeadline) delay(200);

  logLine("boot ok ip=%s rssi=%d time=%s",
          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
          time(nullptr) > 1600000000 ? "synced" : "NOT SYNCED");
  otaPullBegin(RTDB_HOST, DEVICE_ID, FW_VERSION_CODE);
  logLine("fw v%lu, pull-ota: %s", (unsigned long)FW_VERSION_CODE,
          otaPullStatus().c_str());

  logLine("meta publish: %s", publishMeta() ? "ok" : "FAILED");

  // OTA is a bench convenience only: this board needs a manual BOOT+RST dance
  // for every USB flash. Once deployed the board comes back to the bench for
  // changes, so there is no rollback machinery here -- a bad build is
  // recovered over USB.
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    // A publish blocks on TLS for seconds; if one starts mid-upload the OTA
    // socket stalls long enough for the host to give up.
    otaInProgress = true;
    setLed(BLUE);
  });
  ArduinoOTA.onEnd([]() { setLed(GREEN); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    // An upload holds the loop for ~30s, which would otherwise look like a
    // stall to the watchdog.
    esp_task_wdt_reset();
    // Alternate so a stalled update is visually distinct from a running one.
    setLed((done / 16384) % 2 ? BLUE : OFF);
  });
  ArduinoOTA.onError([](ota_error_t) {
    otaInProgress = false;  // resume publishing; old firmware is still live
    setLed(RED);
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/wifi", handleWifiConfig);
  server.on("/rescue", handleRescueConfig);
  server.on("/now", handleNow);
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
  static uint32_t nextUpload = 0;
  static uint32_t lastHistoryBucket = 0;

  // Every path below returns through here, so one feed at the top covers them
  // all; a stall anywhere in the cycle is exactly what should be caught.
  esp_task_wdt_reset();

  ArduinoOTA.handle();

  // While an upload is running, do nothing else. An interrupted OTA leaves the
  // old firmware intact, so pausing publishing for the ~30s of an update is
  // the cheaper risk.
  if (otaInProgress) {
    delay(1);
    return;
  }

  server.handleClient();

  if ((int32_t)(millis() - nextUpload) < 0) {
    // Pulse once every 2s while idle. Without this the LED is only lit for
    // 60ms per minute, so walking up to the board tells you nothing -- on
    // site this pulse is the whole diagnosis: green = last cycle succeeded,
    // yellow/red = it did not.
    static uint32_t nextPulse = 0;
    if ((int32_t)(millis() - nextPulse) >= 0) {
      nextPulse = millis() + 2000;
      setLed(lastCycleOk ? GREEN : RED);
      delay(30);
      setLed(OFF);
    }
    delay(10);
    return;
  }

  // An OTA request that lands mid-cycle would otherwise wait out a blocking
  // TLS publish, long enough for the host side to give up. Give it a short
  // dedicated window right before the slow work starts.
  for (int i = 0; i < 20; i++) {
    ArduinoOTA.handle();
    delay(5);
  }
  nextUpload = millis() + UPLOAD_INTERVAL_MS;

  float t, h;
  if (!readProbe(&t, &h)) {
    readFail++;
    lastError = "probe read failed";
    logLine("probe read FAILED (total %lu)", (unsigned long)readFail);
    lastCycleOk = false;
    blinkColor(RED, 3, 300);
    return;
  }

  lastTemp = t;
  lastHumid = h;
  lastReadTime = time(nullptr);

  // Bucket on wall-clock time rather than elapsed millis: measuring from the
  // previous sample let the 5-minute test fall just short at the 5th minute
  // and land on the 6th, drifting the series. Bucketing pins samples to :00,
  // :05, :10 ... and keeps them aligned even across a reboot.
  uint32_t bucket = (uint32_t)lastReadTime / (HISTORY_INTERVAL_MS / 1000);
  bool withHistory = (bucket != lastHistoryBucket);

  if (publish(t, h, lastReadTime, withHistory)) {
    uploadOk++;
    if (withHistory) lastHistoryBucket = bucket;
    logLine("t=%.1fC h=%.1f%% published%s", t, h,
            withHistory ? " (+history)" : "");
    lastCycleOk = true;
    blinkColor(GREEN, 1, 60);

    // A publish succeeded, so this image works -- commit it and
    // cancel the rollback that would otherwise revert us.
    otaMarkRunningFirmwareGood();
  } else {
    uploadFail++;
    logLine("publish FAILED: %s", lastError.c_str());
    lastCycleOk = false;
    blinkColor(YELLOW, 2, 300);
  }

  static uint32_t nextFwPoll = FW_POLL_FIRST_MS;
  if ((int32_t)(millis() - nextFwPoll) >= 0) {
    nextFwPoll = millis() + FW_POLL_INTERVAL_MS;
    otaPullCheck(RTDB_HOST);
    logLine("fw check: %s", otaPullStatus().c_str());
  }
}
