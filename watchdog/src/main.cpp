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
// Sending the Discord message is deliberately NOT done here: the requirement
// is to keep re-notifying until acknowledged from a phone, and an ESP32 cannot
// receive Discord interactions. A Node.js bot owns that loop and clears
// /alerts/<id>/acked; this device only reports facts.

#include <ArduinoOTA.h>
#include <Arduino.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <rom/rtc.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
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
static const char *FW_VERSION = "b36-2026.09.01";
// Monotonic; RTDB /firmware/watchdog/version is compared against this to
// decide whether a pull-based update is due. Bump on every release.
static const uint32_t FW_VERSION_CODE = 36;

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
struct WiFiNetwork {
  const char *ssid;
  const char *pass;
};
static const WiFiNetwork NETWORKS[] = WIFI_NETWORKS;
static const char *connectedSsid = "none";

static bool connectWiFi() {
  for (const WiFiNetwork &n : NETWORKS) {
    WiFi.begin(n.ssid, n.pass);
    uint32_t deadline = millis() + 12000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline)
      blinkColor(BLUE, 1, 250);

    if (WiFi.status() == WL_CONNECTED) {
      connectedSsid = n.ssid;
      return true;
    }
    WiFi.disconnect();
  }
  return false;
}

// ---- rtdb -------------------------------------------------------------
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
    // The bot owns `acked`; clearing it here is what starts a fresh
    // notification loop for a newly-fired alert.
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
  time_t lastTry;    // when we last attempted a post, success or not
  uint32_t failures; // consecutive failed posts, drives the backoff

  void reset() {
    msgId = "";
    lastSent = 0;
    ackedAt = 0;
    sentCount = 0;
    lastTry = 0;
    failures = 0;
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

// ---- http status page -------------------------------------------------
static void handleRoot() {
  String b = "=== ESP32-B watchdog ===\n";
  b += "ssid: " + String(connectedSsid) + "\n";
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
  if (!connectWiFi()) {
    setLed(PURPLE);
    delay(5000);
    ESP.restart();
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

  logLine("boot ok ip=%s ssid=%s time=%s",
          WiFi.localIP().toString().c_str(), connectedSsid,
          time(nullptr) > 1600000000 ? "synced" : "NOT SYNCED");
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
