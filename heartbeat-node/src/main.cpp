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
//   /outages/<id>/<start>  every stretch the heartbeat stopped, and why
//
// Built with flash_mode dio and a 4MB partition table on purpose: one
// watchdog board misread flash under qio (see the handoff), and checking
// every board of a bulk order for that is not practical; dio is slower but a
// heartbeat does not care. 4MB keeps the image installable, and updatable,
// on any of the 4/8/16MB S3 boards a bulk order might bring.

#include <Arduino.h>
#include <esp_task_wdt.h>
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
#include "reset_reason.h"

// ---- configuration ----------------------------------------------------
static const char *FW_GROUP = "heartbeat";
static const char *FW_VERSION = "h6-2026.10.18";
// Monotonic; /firmware/<group>/version is compared against this. Bump on
// every release.
static const uint32_t FW_VERSION_CODE = 6;

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
// ts is handed back so the outage record can key on exactly the value the
// watchdog will read as latest.ts.
static bool publishBeat(uint32_t &ts) {
  time_t now = time(nullptr);
  ts = (uint32_t)now;
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
// Written only from boot and once per beat (a minute), so flash wear is a
// non-issue.
static Preferences incPrefs;

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

// The ts of the beat that just got through, not the time now: an outage that
// follows starts at this value, and the watchdog keys the same outage on it.
static void incidentHeartbeat(uint32_t beatTs) {
  if (beatTs < 1600000000) return;
  incPrefs.begin("inc", false);
  incPrefs.putUInt("beat", beatTs);
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

// ---- outage record ------------------------------------------------------
// Every stretch the heartbeat stopped, from the board's own side: the last
// beat that got through, the next one that did, and why. It goes to
// /outages/<id>/<start>, the record the watchdog opens when it judges the
// board stale -- start is the ts of the last beat, the very latest.ts the
// watchdog keys on -- so the two halves merge into one row. The watchdog's
// half says the board is down right now; this half says why, and covers
// anything that happened while the watchdog itself was down.
//
// Only gaps the watchdog would alert on count: the ponds restart their
// supplies on a schedule, and those short, expected gaps would bury the real
// outages in the statistics.
//
// Held in NVS from the moment it opens, because the outage is often a power
// cut that ends this run. One write when it opens, one when it closes.
//
//   cause   power     POWERON: the supply went away
//           brownout  the supply sagged
//           crash     a hang or a panic
//           reboot    our own restart (an update, a re-label)
//           wifi      the board stayed up, WiFi dropped
//           internet  WiFi up, but no name lookup: the pond's uplink
//           cloud     internet up, RTDB refused or timed out
// The watchdog's STALE_MULTIPLE: it alerts once a node has been silent for
// more than this many of its intervals.
static const uint32_t OUTAGE_MIN_S = 5 * (PUBLISH_INTERVAL_MS / 1000);

static bool outagePending = false;
static String outageCause;          // mirrors NVS "oc" while pending
static uint32_t lastBeatTs = 0;     // ts of the last beat that got through
static int lastBeatRssi = 0;

// Set from the WiFi event task. Only the first drop after a connection counts:
// the reconnect attempts that follow report their own, less telling reasons.
static volatile bool wifiWasUp = false;
static volatile uint8_t wifiDropReason = 0;

static void wifiEventsBegin() {
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { wifiWasUp = true; },
               ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        if (!wifiWasUp) return;
        wifiWasUp = false;
        wifiDropReason = info.wifi_sta_disconnected.reason;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

// Read on a website, so in words; the code stays in for anything unlisted.
static String wifiDropName(uint8_t r) {
  switch (r) {
    case 200: return "收不到基地台訊號 (200)";             // BEACON_TIMEOUT
    case 201: return "找不到基地台 (201)";                 // NO_AP_FOUND
    case 2: case 15: case 202: case 204:
      return "認證失敗 (" + String(r) + ")";               // password / handshake
    case 8: return "主動斷線 (8)";                         // ASSOC_LEAVE
    case 0: return "原因不明";
    default: return "WiFi 斷線 (" + String(r) + ")";
  }
}

static const char *causeFromReset(const char *reset) {
  if (strstr(reset, "POWERON")) return "power";
  if (strstr(reset, "BROWNOUT") || strstr(reset, "GLITCH")) return "brownout";
  if (strstr(reset, "hang") || strstr(reset, "PANIC") || strstr(reset, "crash"))
    return "crash";
  return "reboot";
}

// Opens a record, or -- if one is already open -- refines a cause that only
// said "reboot": a restart followed by a failed network is a network outage.
static void outageOpen(uint32_t start, const String &cause,
                       const String &detail, const char *reset, int rssi) {
  if (outagePending && outageCause != "reboot") return;
  if (!outagePending && start < 1600000000) return;  // no beat to start from
  incPrefs.begin("inc", false);
  if (!outagePending) {
    incPrefs.putUInt("os", start);
    if (reset) incPrefs.putString("or", reset);
    if (rssi) incPrefs.putInt("orr", rssi);
  }
  incPrefs.putString("oc", cause);
  incPrefs.putString("od", detail);
  incPrefs.end();
  outagePending = true;
  outageCause = cause;
  logLine("outage open: %s %s", cause.c_str(), detail.c_str());
}

// At boot: a run that ended mid-outage carries on with it; otherwise this boot
// is itself the start of one, and the reset reason says why.
static void outageAtBoot() {
  const char *reset = resetReasonName();
  incPrefs.begin("inc", false);
  outagePending = incPrefs.getUInt("os", 0) != 0;
  if (outagePending) {
    outageCause = incPrefs.getString("oc", "");
    // The first reboot inside the outage is the telling one; later ones are
    // usually ours, giving up on WiFi.
    if (!incPrefs.getString("or", "").length()) incPrefs.putString("or", reset);
  }
  incPrefs.end();
  if (!outagePending && prevRun.lastBeat)
    outageOpen(prevRun.lastBeat, causeFromReset(reset), "", reset, 0);
}

// A beat just failed while the board is running. Works out which link broke.
static void outageOnFailure() {
  if (outagePending && outageCause != "reboot") return;
  String cause, detail;
  if (WiFi.status() != WL_CONNECTED) {
    cause = "wifi";
    detail = wifiDropName(wifiDropReason);
  } else {
    IPAddress ip;
    if (!WiFi.hostByName(RTDB_HOST, ip)) {
      cause = "internet";
      detail = "DNS 查詢失敗";
    } else {
      cause = "cloud";
      detail = lastError;
    }
  }
  outageOpen(lastBeatTs, cause, detail, nullptr, lastBeatRssi);
}

// A beat got through. Uploads the open record, if any, and clears it; on a
// failed upload it stays in NVS -- end included -- and the next beat retries.
static void outageOnSuccess(uint32_t beatTs) {
  if (!outagePending || beatTs < 1600000000) return;
  incPrefs.begin("inc", false);
  uint32_t start = incPrefs.getUInt("os", 0);
  uint32_t end = incPrefs.getUInt("oe", 0);
  if (!end) {
    end = beatTs;
    incPrefs.putUInt("oe", end);
  }
  String cause = incPrefs.getString("oc", "");
  String detail = incPrefs.getString("od", "");
  String reset = incPrefs.getString("or", "");
  int rssi = incPrefs.getInt("orr", 0);
  incPrefs.end();

  uint32_t dur = end > start ? end - start : 0;
  // The same test the watchdog applies (age > limit), so a gap is in the
  // statistics exactly when it would have raised an alert.
  if (dur > OUTAGE_MIN_S && start) {
    String body = "{\"start\":" + String(start);
    body += ",\"end\":" + String(end);
    body += ",\"dur\":" + String(dur);
    body += ",\"active\":false";
    body += ",\"cause\":\"" + cause + "\"";
    if (detail.length()) body += ",\"detail\":\"" + jsonEscape(detail) + "\"";
    if (reset.length()) body += ",\"reset\":\"" + jsonEscape(reset) + "\"";
    if (rssi) body += ",\"rssi\":" + String(rssi);
    body += ",\"fw\":" + String(FW_VERSION_CODE);
    body += "}";
    if (!rtdb("PATCH", "/outages/" + me.id + "/" + String(start) + ".json",
              body)) {
      logLine("outage upload FAILED: %s", lastError.c_str());
      return;
    }
    logLine("outage recorded: %s %lus", cause.c_str(), (unsigned long)dur);
  }

  incPrefs.begin("inc", false);
  for (const char *k : {"os", "oe", "oc", "od", "or", "orr"}) incPrefs.remove(k);
  incPrefs.end();
  outagePending = false;
  outageCause = "";
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
  if (outagePending) b += "outage open: " + outageCause + " (not yet uploaded)\n";
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
  outageAtBoot();
  wifiEventsBegin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Keep looking for a few minutes rather than rebooting at once -- the rescue
  // hotspot is switched on by hand after someone notices the purple LED -- but
  // bounded, so a broken update still reaches the reboot that rolls it back.
  uint32_t tries = 0;
  while (!connectWiFi()) {
    bootPasses = ++tries;
    // A restart of our own that then cannot find a network is a WiFi outage;
    // a power cut that cannot is still a power cut (outageOpen keeps it).
    if (tries == 1)
      outageOpen(prevRun.lastBeat, "wifi", "開機時連不上 WiFi", nullptr, 0);
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

  uint32_t beatTs;
  if (publishBeat(beatTs)) {
    publishOk++;
    lastCycleOk = true;
    // A publish got through, so this image works: commit it and cancel the
    // rollback that would otherwise revert it on the next boot.
    otaMarkRunningFirmwareGood();
    outageOnSuccess(beatTs);
    incidentHeartbeat(beatTs);
    lastBeatTs = beatTs;
    lastBeatRssi = WiFi.RSSI();
  } else {
    publishFail++;
    lastCycleOk = false;
    logLine("publish FAILED: %s", lastError.c_str());
    outageOnFailure();
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
