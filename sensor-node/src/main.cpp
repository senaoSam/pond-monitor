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
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

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
static const char *FW_VERSION = "a2-2026.08.22";

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

// ---- state ------------------------------------------------------------
static WebServer server(80);

static float lastTemp = NAN, lastHumid = NAN;
static time_t lastReadTime = 0;
static uint32_t uploadOk = 0, uploadFail = 0, readFail = 0;
static String lastError = "none";
static bool lastCycleOk = false;  // drives the idle heartbeat colour
static volatile bool otaInProgress = false;

static String logBuf;
static const size_t LOG_MAX = 8000;

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

// ---- wifi -------------------------------------------------------------
struct WiFiNetwork {
  const char *ssid;
  const char *pass;
};
static const WiFiNetwork NETWORKS[] = WIFI_NETWORKS;
static const char *connectedSsid = "none";

// Try each known network in turn. Moving the board between the bench and the
// pond then needs no reflash -- whichever network is in range wins.
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
static void handleRoot() {
  String b = "=== ESP32-A ===\n";
  b += "ssid: " + String(connectedSsid) + "\n";
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

void setup() {
  blinkColor(BLUE, 2, 120);

  Serial1.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (!connectWiFi()) {
    setLed(PURPLE);  // solid purple: no known network, nothing can be reported
    delay(5000);
    ESP.restart();
  }
  blinkColor(GREEN, 5, 60);

  // History keys and heartbeat freshness are both wall-clock based, so NTP
  // must land before the first publish.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t ntpDeadline = millis() + 15000;
  while (time(nullptr) < 1600000000 && millis() < ntpDeadline) delay(200);

  logLine("boot ok ip=%s rssi=%d time=%s",
          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
          time(nullptr) > 1600000000 ? "synced" : "NOT SYNCED");
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
    // Alternate so a stalled update is visually distinct from a running one.
    setLed((done / 16384) % 2 ? BLUE : OFF);
  });
  ArduinoOTA.onError([](ota_error_t) {
    otaInProgress = false;  // resume publishing; old firmware is still live
    setLed(RED);
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/now", handleNow);
  server.begin();
}

void loop() {
  static uint32_t nextUpload = 0;
  static uint32_t lastHistoryBucket = 0;

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
  } else {
    uploadFail++;
    logLine("publish FAILED: %s", lastError.c_str());
    lastCycleOk = false;
    blinkColor(YELLOW, 2, 300);
  }
}
