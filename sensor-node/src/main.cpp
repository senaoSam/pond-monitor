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
static const char *FW_VERSION = "a7-2026.09.01";
// Monotonic; RTDB /firmware/pond-site/version is compared against this to
// decide whether a pull-based update is due. Bump on every release.
static const uint32_t FW_VERSION_CODE = 7;

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
  if (!connectWiFi()) {
    setLed(PURPLE);  // solid purple: no known network, nothing can be reported
    delay(5000);
    ESP.restart();
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
  server.on("/fwcheck", handleFwCheck);
  server.on("/verify", handleVerify);
  server.on("/rawprobe", handleRawProbe);
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
