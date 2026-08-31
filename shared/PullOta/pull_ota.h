// Pull-based firmware update.
//
// The espota path (host connects in to the board) cannot work once the boards
// live at a relative's house on the far side of someone else's router. Here the
// board reaches out instead: it polls a version number in RTDB, and when that
// number is higher than its own it downloads the image over HTTPS and reboots
// into it. Outbound-only, so no port forwarding and no NAT traversal.
//
// Rollback is the part that matters at 300km: a bad image that cannot reach
// WiFi would otherwise mean posting the board back. So an update is committed
// only after the new firmware proves itself -- see otaMarkRunningFirmwareGood.
// Until that call the ESP32 keeps the previous image and reverts on the next
// boot.
//
// A failed version is also recorded, so the board does not download the same
// broken image forever.
//
//   RTDB  /firmware/<device>/version   : integer, bump to trigger an update
//         /firmware/<device>/url       : https URL of the .bin
//   NVS   fw.running / fw.pending / fw.bad
//
// Usage from the sketch:
//   otaPullBegin(DEVICE_ID, FW_VERSION_CODE);   // in setup(), after WiFi
//   otaMarkRunningFirmwareGood();               // after the first good cycle
//   otaPullCheck();                             // periodically, when idle

#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

// ---- state ------------------------------------------------------------
static Preferences otaPrefs;
static const char *otaDeviceId = "";
static uint32_t otaRunningVersion = 0;
static bool otaMarkedGood = false;
static String otaLastStatus = "idle";
static uint32_t otaAttempts = 0, otaFailures = 0;

// Exposed so a status page can show why an update is or is not happening.
static String otaPullStatus() { return otaLastStatus; }
static uint32_t otaPullAttempts() { return otaAttempts; }
static uint32_t otaPullFailures() { return otaFailures; }
static uint32_t otaPullVersion() { return otaRunningVersion; }

// ---- rollback ---------------------------------------------------------

// Call once the new firmware has demonstrably worked -- WiFi up and a real
// publish succeeded. Before this call the ESP32 still considers the running
// image unverified and will revert on the next reboot, which is what makes a
// remote update safe.
static void otaMarkRunningFirmwareGood() {
  if (otaMarkedGood) return;
  otaMarkedGood = true;

  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    otaLastStatus = "new firmware confirmed good";
  }

  // A version that got this far is known-good; clear any bad-version marker
  // for it so a later legitimate re-release of the same number is not skipped.
  otaPrefs.begin("fw", false);
  if (otaPrefs.getUInt("bad", 0) == otaRunningVersion) otaPrefs.remove("bad");
  otaPrefs.putUInt("running", otaRunningVersion);
  otaPrefs.end();
}

static void otaPullBegin(const char *deviceId, uint32_t versionCode) {
  otaDeviceId = deviceId;
  otaRunningVersion = versionCode;

  // If we booted into an image that is still pending verification and then
  // rebooted without confirming, the ESP32 has already reverted us. Record the
  // version that failed so we stop re-downloading it.
  otaPrefs.begin("fw", false);
  uint32_t pending = otaPrefs.getUInt("pending", 0);
  if (pending && pending != versionCode) {
    // We asked for `pending` but are running something else -- it rolled back.
    otaPrefs.putUInt("bad", pending);
    otaPrefs.remove("pending");
    otaLastStatus = "v" + String(pending) + " rolled back, marked bad";
  }
  otaPrefs.end();
}

// ---- update -----------------------------------------------------------

// Reads the target version and URL for this device from RTDB.
static bool otaFetchTarget(const char *rtdbHost, uint32_t *version,
                           String *url) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);

  HTTPClient http;
  String u = String("https://") + rtdbHost + "/firmware/" + otaDeviceId +
             ".json";
  if (!http.begin(client, u)) return false;
  http.setTimeout(12000);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  int vk = body.indexOf("\"version\":");
  if (vk < 0) return false;
  *version = (uint32_t)body.substring(vk + 10).toInt();

  int uk = body.indexOf("\"url\":\"");
  if (uk < 0) return false;
  int s = uk + 7, e = body.indexOf('"', s);
  if (e < 0) return false;
  *url = body.substring(s, e);
  return true;
}

// Downloads and installs an image. Returns only on failure: success reboots.
static bool otaDownloadAndApply(const String &url, uint32_t version) {
  otaAttempts++;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  HTTPClient http;
  // GitHub release assets redirect to a CDN host, so redirects must be taken.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    otaLastStatus = "begin failed";
    otaFailures++;
    return false;
  }
  http.setTimeout(20000);

  int code = http.GET();
  if (code != 200) {
    otaLastStatus = "GET " + String(code);
    otaFailures++;
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    otaLastStatus = "no content-length";
    otaFailures++;
    http.end();
    return false;
  }

  if (!Update.begin(len)) {
    otaLastStatus = "Update.begin: " + String(Update.errorString());
    otaFailures++;
    http.end();
    return false;
  }

  // Remember the version we are moving to *before* rebooting, so a rollback is
  // detectable on the next boot.
  otaPrefs.begin("fw", false);
  otaPrefs.putUInt("pending", version);
  otaPrefs.end();

  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();

  if (written != (size_t)len) {
    otaLastStatus = "short write " + String(written) + "/" + String(len);
    otaFailures++;
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    otaLastStatus = "Update.end: " + String(Update.errorString());
    otaFailures++;
    return false;
  }

  otaLastStatus = "installed v" + String(version) + ", rebooting";
  delay(500);
  ESP.restart();
  return true;  // not reached
}

// Poll for a newer version and install it. Safe to call often; it only acts
// when RTDB advertises a higher, not-known-bad version.
static void otaPullCheck(const char *rtdbHost) {
  uint32_t target = 0;
  String url;
  if (!otaFetchTarget(rtdbHost, &target, &url)) {
    otaLastStatus = "no /firmware entry";
    return;
  }

  if (target <= otaRunningVersion) {
    otaLastStatus = "up to date (v" + String(otaRunningVersion) + ")";
    return;
  }

  otaPrefs.begin("fw", true);
  uint32_t bad = otaPrefs.getUInt("bad", 0);
  otaPrefs.end();
  if (target == bad) {
    otaLastStatus = "v" + String(target) + " known bad, skipping";
    return;
  }

  otaLastStatus = "updating v" + String(otaRunningVersion) + " -> v" +
                  String(target);
  otaDownloadAndApply(url, target);
}
