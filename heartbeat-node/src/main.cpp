// Heartbeat node: proves a power supply is alive by publishing every minute.
//
// One of many. Each board is plugged into a different supply at a pond --
// A01..A10 at pond A, B01.. at pond B -- and the watchdog reports any that go
// quiet, so "A05 stopped" means "A05's supply failed". There is no sensor:
// the heartbeat is the measurement.
//
// Every board runs this same image. Who a board is comes from the registry
// (shared/Registry: RTDB /registry/<MAC>, written on the bench by
// tools/register.sh), and which firmware it pulls from its group: all
// heartbeat boards share /firmware/heartbeat, and the ones registered as
// canary pull /firmware/heartbeat-canary, where a release goes first.
//
//   /devices/<id>/latest   {ts, ssid, ip, rssi, up}   every minute
//   /devices/<id>/meta     {name, role, group, interval, mac, ...}   per boot
//   /incidents/<id>/<ts>   how the previous run ended, when it is worth saying
//
// Built with flash_mode dio and a 4MB partition table on purpose: one
// watchdog board misread flash under qio (see the handoff), and checking
// every board of a bulk order for that is not practical; dio is slower but a
// heartbeat does not care. 4MB keeps the image installable, and updatable,
// on any of the 4/8/16MB S3 boards a bulk order might bring.

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <rom/rtc.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "secrets.h"

static void logLine(const char *fmt, ...);
static void siteWifiWaitTick();

#include "site_wifi.h"
#include "registry.h"
#include "pull_ota.h"

// ---- configuration ----------------------------------------------------
static const char *FW_GROUP = "heartbeat";
static const char *FW_VERSION = "h1-2026.10.10";
// Monotonic; /firmware/<group>/version is compared against this. Bump on
// every release.
static const uint32_t FW_VERSION_CODE = 1;

// The onboard RGB. GPIO48 on the DevKitC-1 v1.0 layout these boards follow;
// v1.1 boards moved it to GPIO38, so a dark LED on a new batch means look here
// first, not that the board is dead.
static const int LED_PIN = 48;

// Kept at the cadence the watchdog was built around: it treats a node as
// stale after five missed intervals, read from meta.interval.
static const uint32_t PUBLISH_INTERVAL_MS = 60UL * 1000;

// The first firmware poll a minute after boot, so power-cycling a board is
// itself a way to pull an update promptly; then half-hourly. The registry and
// the site WiFi entry are refreshed on the same schedule.
static const uint32_t POLL_FIRST_MS = 60UL * 1000;
static const uint32_t POLL_INTERVAL_MS = 30UL * 60 * 1000;

// 120s, not the default 5s: PullOta's slot erase starves IDLE0 for several
// seconds (it killed the pond sensor's v6 that way), and two minutes also
// catches a genuinely hung loop.
static const uint32_t WDT_TIMEOUT_S = 120;

// Full passes over every candidate network before rebooting, roughly three to
// five minutes. Bounded so an image that cannot connect still reaches the
// reboot that lets pull_ota roll it back.
static const uint32_t WIFI_BOOT_ATTEMPTS = 4;

// ---- state ------------------------------------------------------------
static WebServer server(80);
static Identity me;
static uint32_t publishOk = 0, publishFail = 0;
static String lastError = "none";
static bool lastCycleOk = false;

static String logBuf;
static const size_t LOG_MAX = 6000;

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

// ---- led --------------------------------------------------------------
// WS2812: needs neopixelWrite, digitalWrite leaves it dark. Colour is the
// whole diagnosis on site, where there is no console:
//   green pulse  publishing          yellow pulse  publish failing
//   cyan pulse   not registered yet  blue          connecting
//   purple       no WiFi, will reboot
enum StatusColor { OFF, GREEN, BLUE, YELLOW, PURPLE, CYAN };

static void setLed(StatusColor c) {
  uint8_t r = 0, g = 0, b = 0;
  switch (c) {
    case GREEN:  g = 40; break;
    case BLUE:   b = 40; break;
    case YELLOW: r = 40; g = 25; break;
    case PURPLE: r = 30; b = 30; break;
    case CYAN:   g = 30; b = 30; break;
    case OFF:    break;
  }
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

static void siteWifiWaitTick() { blinkColor(BLUE, 1, 125); }

static String jsonEscape(const String &in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    o += (c == '"' || c == '\\' || c == '\n' || c == '\r') ? ' ' : c;
  }
  return o;
}

// RTC_SW_* is our own ESP.restart(), *WDT_* a hang, BROWNOUT a sagging supply.
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

// ---- rtdb -------------------------------------------------------------
static bool rtdb(const char *method, const String &path, const String &body) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  if (!http.begin(client, String("https://") + RTDB_HOST + path)) {
    lastError = "http.begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.sendRequest(method, body);
  bool ok = code == 200;
  if (!ok) lastError = String(method) + " " + String(code);
  http.end();
  return ok;
}

// The heartbeat. ssid and ip are what make a board reachable and say where it
// actually is; rssi is the early warning for a supply placed too far from the
// AP. No "ts" anywhere else under /devices/<id>: the watchdog takes the first
// "ts" it finds there as the heartbeat.
static bool publishBeat() {
  time_t now = time(nullptr);
  String body = "{\"ts\":" + String((uint32_t)now);
  body += ",\"ssid\":\"" + jsonEscape(connectedSsid) + "\"";
  body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  body += ",\"rssi\":" + String(WiFi.RSSI());
  body += ",\"up\":" + String(millis() / 1000);
  body += "}";
  return rtdb("PUT", "/devices/" + me.id + "/latest.json", body);
}

// Per boot. interval is what the watchdog derives its stale threshold from;
// mac and registered let an unlabelled board be told apart and found.
static bool publishMeta() {
  String body = "{";
  body += "\"name\":\"" + jsonEscape(me.id) + "\"";
  body += ",\"role\":\"heartbeat\"";
  body += ",\"scope\":\"power\"";
  body += ",\"group\":\"" + otaPullGroup() + "\"";
  body += ",\"fw\":\"" + String(FW_VERSION) + "\"";
  body += ",\"fwCode\":" + String(FW_VERSION_CODE);
  body += ",\"interval\":" + String(PUBLISH_INTERVAL_MS / 1000);
  body += ",\"mac\":\"" + me.mac + "\"";
  body += ",\"registered\":" + String(me.registered ? "true" : "false");
  if (me.note.length()) body += ",\"note\":\"" + jsonEscape(me.note) + "\"";
  body += ",\"bootAt\":" + String((uint32_t)time(nullptr));
  body += "}";
  return rtdb("PUT", "/devices/" + me.id + "/meta.json", body);
}

// ---- incident record ---------------------------------------------------
// Same scheme as the pond sensor: each boot records how the previous run
// ended, and uploads it once there is a network. For a heartbeat board this
// is directly useful -- a POWERON whose last beat was seconds ago is exactly
// the supply cut it exists to detect, recorded from the board's own side.
// Written only from boot and at most once a minute, so flash wear is a
// non-issue.
static Preferences incPrefs;
static uint32_t lastBeatWrite = 0;

struct PrevRun {
  bool present;
  uint32_t lastBeat, uptime, bootCount;
};
static PrevRun prevRun = {false, 0, 0, 0};

static void incidentBegin() {
  incPrefs.begin("inc", false);
  prevRun.lastBeat = incPrefs.getUInt("beat", 0);
  prevRun.uptime = incPrefs.getUInt("up", 0);
  prevRun.bootCount = incPrefs.getUInt("boots", 0);
  prevRun.present = prevRun.lastBeat > 0 || prevRun.bootCount > 0;
  incPrefs.putUInt("boots", prevRun.bootCount + 1);
  incPrefs.putUInt("up", 0);
  incPrefs.end();
}

static void incidentHeartbeat() {
  time_t now = time(nullptr);
  if (now < 1600000000) return;
  if (lastBeatWrite && millis() - lastBeatWrite < 60000) return;
  lastBeatWrite = millis();
  incPrefs.begin("inc", false);
  incPrefs.putUInt("beat", (uint32_t)now);
  incPrefs.putUInt("up", millis() / 1000);
  incPrefs.end();
}

// A clean restart that came straight back is not an incident; reporting every
// reboot would bury the ones that matter, and most reboots are our own OTA.
static bool incidentWorthReporting(const char *reset) {
  if (bootPasses > 0) return true;
  if (strstr(reset, "hang") || strstr(reset, "BROWNOUT") ||
      strstr(reset, "GLITCH") || strstr(reset, "PANIC") || strstr(reset, "crash"))
    return true;
  // Power came back: always worth a line on a board whose job is power.
  if (strstr(reset, "POWERON") && prevRun.present && prevRun.lastBeat) return true;
  return false;
}

static void incidentReport() {
  const char *reset = resetReasonName();
  if (!incidentWorthReporting(reset)) return;
  time_t now = time(nullptr);
  if (now < 1600000000) return;

  String body = "{\"reset\":\"" + String(reset) + "\"";
  body += ",\"boots\":" + String(prevRun.bootCount + 1);
  if (prevRun.lastBeat) {
    body += ",\"lastBeat\":" + String(prevRun.lastBeat);
    body += ",\"gap\":" + String((uint32_t)(now - (time_t)prevRun.lastBeat));
  }
  body += ",\"prevUptime\":" + String(prevRun.uptime);
  body += ",\"wifiMs\":" + String(bootWifiMs);
  body += ",\"wifiPasses\":" + String(bootPasses);
  body += ",\"attempts\":\"" + jsonEscape(bootAttempts) + "\"";
  body += ",\"rssi\":" + String(WiFi.RSSI());
  body += ",\"fw\":" + String(FW_VERSION_CODE);
  body += "}";
  bool ok = rtdb("PUT", "/incidents/" + me.id + "/" + String((uint32_t)now) +
                            ".json", body);
  logLine("incident %s: %s", ok ? "reported" : "report FAILED", reset);
}

// ---- http -------------------------------------------------------------
static void handleRoot() {
  String b = "=== heartbeat " + me.id + " ===\n";
  b += "mac: " + me.mac + "\n";
  b += "identity: " + String(me.registered ? "registered" : "NOT REGISTERED") +
       " (" + me.source + ")" + (me.canary ? "  [canary]" : "") + "\n";
  if (me.note.length()) b += "note: " + me.note + "\n";
  b += siteWifiStatus();
  b += "uptime: " + String(millis() / 1000) + "s\n";
  b += "free heap: " + String(ESP.getFreeHeap()) + "\n\n";
  b += "publishes ok: " + String(publishOk) + ", failed: " + String(publishFail) + "\n";
  b += "last error: " + lastError + "\n";
  b += "last reset: " + String(resetReasonName()) + "\n";
  b += "boot count: " + String(prevRun.bootCount + 1) + "\n";
  b += "fw: " + String(FW_VERSION) + " (v" + String(otaPullVersion()) +
       ", group " + otaPullGroup() + ")\n";
  b += "fw pull: " + otaPullStatus() + " (tries " + String(otaPullAttempts()) +
       ", fails " + String(otaPullFailures()) + ")\n";
  b += "========================\n\n";
  b += logBuf;
  server.send(200, "text/plain; charset=utf-8", b);
}

static void handleFwCheck() {
  otaPullCheck(RTDB_HOST);
  server.send(200, "text/plain; charset=utf-8",
              "running v" + String(otaPullVersion()) + " (" + otaPullGroup() +
                  ")\nresult: " + otaPullStatus() + "\n");
}

// ---- setup / loop -----------------------------------------------------
void setup() {
  blinkColor(BLUE, 2, 120);
  incidentBegin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Keep looking for a few minutes rather than rebooting at once -- the rescue
  // hotspot is switched on by hand after someone notices the purple LED -- but
  // bounded, so a broken update still reaches the reboot that rolls it back.
  uint32_t tries = 0;
  while (!connectWiFi()) {
    bootPasses = ++tries;
    if (tries >= WIFI_BOOT_ATTEMPTS) {
      logLine("no wifi after %lu passes, rebooting", (unsigned long)tries);
      setLed(PURPLE);
      delay(2000);
      ESP.restart();
    }
    for (int i = 0; i < 5; i++) blinkColor(PURPLE, 1, 500);
  }
  blinkColor(GREEN, 5, 60);

  // Heartbeat freshness is wall-clock based, so NTP before the first publish.
  // UTC+8 only affects the human-readable log; stored stamps are unix seconds.
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  uint32_t ntpDeadline = millis() + 15000;
  while (time(nullptr) < 1600000000 && millis() < ntpDeadline) delay(200);

  me = registryResolve(RTDB_HOST);
  logLine("boot ok: %s (%s, mac %s) ip=%s rssi=%d", me.id.c_str(),
          me.source.c_str(), me.mac.c_str(), WiFi.localIP().toString().c_str(),
          WiFi.RSSI());

  // Canary boards take a release first; everyone else waits for it to be
  // promoted. tools/release.sh keeps the canary entry at least as new as the
  // main one, so a canary board never falls behind.
  String group = String(FW_GROUP) + (me.canary ? "-canary" : "");
  otaPullBegin(RTDB_HOST, me.id, group, FW_VERSION_CODE);
  logLine("fw v%lu group %s: %s", (unsigned long)FW_VERSION_CODE, group.c_str(),
          otaPullStatus().c_str());

  siteWifiSync(RTDB_HOST, registrySite(me));
  incidentReport();
  logLine("meta publish: %s", publishMeta() ? "ok" : "FAILED");

  server.on("/", handleRoot);
  server.on("/fwcheck", handleFwCheck);
  siteWifiAttach(server);
  server.begin();

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  static uint32_t nextPublish = 0;
  static uint32_t nextPoll = POLL_FIRST_MS;

  esp_task_wdt_reset();
  server.handleClient();

  if ((int32_t)(millis() - nextPublish) < 0) {
    // A pulse every 2s so walking up to the board tells you something.
    static uint32_t nextPulse = 0;
    if ((int32_t)(millis() - nextPulse) >= 0) {
      nextPulse = millis() + 2000;
      setLed(!lastCycleOk ? YELLOW : (me.registered ? GREEN : CYAN));
      delay(30);
      setLed(OFF);
    }
    delay(10);
    return;
  }
  nextPublish = millis() + PUBLISH_INTERVAL_MS;

  if (publishBeat()) {
    publishOk++;
    lastCycleOk = true;
    // A publish got through, so this image works: commit it and cancel the
    // rollback that would otherwise revert it on the next boot.
    otaMarkRunningFirmwareGood();
    incidentHeartbeat();
  } else {
    publishFail++;
    lastCycleOk = false;
    logLine("publish FAILED: %s", lastError.c_str());
  }

  if ((int32_t)(millis() - nextPoll) >= 0) {
    nextPoll = millis() + POLL_INTERVAL_MS;
    otaPullCheck(RTDB_HOST);  // reboots inside if an update installs
    logLine("fw check: %s", otaPullStatus().c_str());

    // A board re-labelled (or registered for the first time) on the bench
    // picks up its new identity here; rebooting is the simplest way to make
    // every path -- meta, OTA group, site WiFi -- agree on it.
    Identity now = registryResolve(RTDB_HOST);
    if (now.source == "registry" &&
        (now.id != me.id || now.canary != me.canary)) {
      logLine("registry changed: %s -> %s, rebooting", me.id.c_str(),
              now.id.c_str());
      delay(500);
      ESP.restart();
    }
    siteWifiSync(RTDB_HOST, registrySite(me));
  }
}
