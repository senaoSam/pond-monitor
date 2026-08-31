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
//         /fwlog/<device>/<ts>         : one record per update attempt
//   NVS   fw.running / fw.pending / fw.bad
//
// The log exists because /firmware and meta.fw only ever hold the current
// value. Without it a rollback leaves no trace at all -- and a rollback is
// precisely the event worth knowing about, since it means some version is
// broken. It also lets "it started misbehaving on Tuesday" be checked against
// what was actually shipped.
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
#include <esp_err.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>

// How many times a version that failed to confirm is retried before it is
// treated as genuinely broken. Small, because each retry costs a download.
static const uint32_t OTA_BAD_RETRY_LIMIT = 3;

// ---- state ------------------------------------------------------------
static Preferences otaPrefs;
static const char *otaDeviceId = "";
static uint32_t otaRunningVersion = 0;
static bool otaMarkedGood = false;
// Set at boot when a rollback is detected, logged once the clock is up.
static uint32_t rolledBackFrom = 0;
// Remembered so the deferred log write does not need it passed in again.
static const char *otaRtdbHost = nullptr;
static String otaLastStatus = "idle";
static uint32_t otaAttempts = 0, otaFailures = 0;

// Exposed so a status page can show why an update is or is not happening.
static String otaPullStatus() { return otaLastStatus; }
static uint32_t otaPullAttempts() { return otaAttempts; }
static uint32_t otaPullFailures() { return otaFailures; }
static uint32_t otaPullVersion() { return otaRunningVersion; }

// ---- log --------------------------------------------------------------

// Appends one record under /fwlog/<device>/<unix ts>. Best-effort: a failure
// to log must never interfere with the update itself, so the result is ignored
// by callers.
static void otaLogEvent(const char *rtdbHost, uint32_t fromVer,
                        uint32_t toVer, const char *result,
                        const String &detail) {
  time_t now = time(nullptr);
  if (now < 1600000000) return;  // no clock yet; a key would be meaningless

  String body = "{";
  body += "\"from\":" + String(fromVer);
  body += ",\"to\":" + String(toVer);
  body += ",\"result\":\"" + String(result) + "\"";
  if (detail.length()) {
    // Quotes and backslashes would break the hand-built JSON.
    String d = detail;
    d.replace("\\", " ");
    d.replace("\"", "'");
    body += ",\"detail\":\"" + d + "\"";
  }
  body += "}";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  HTTPClient http;
  String u = String("https://") + rtdbHost + "/fwlog/" + otaDeviceId + "/" +
             String((uint32_t)now) + ".json";
  if (!http.begin(client, u)) return;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  http.sendRequest("PUT", body);
  http.end();
}

// ---- rollback ---------------------------------------------------------

// Call once the new firmware has demonstrably worked -- WiFi up and a real
// publish succeeded. Before this call the ESP32 still considers the running
// image unverified and will revert on the next reboot, which is what makes a
// remote update safe.
static void otaMarkRunningFirmwareGood() {
  if (otaMarkedGood) return;
  otaMarkedGood = true;

  // Deferred from otaPullBegin: NTP and the network are both up by now, so a
  // rollback detected at boot can finally be logged with a usable timestamp.
  if (rolledBackFrom && otaRtdbHost) {
    otaLogEvent(otaRtdbHost, rolledBackFrom, otaRunningVersion, "rolled-back",
                "new image did not confirm; reverted");
    rolledBackFrom = 0;
  }

  // Mark valid unconditionally, not just when the state reads back as
  // PENDING_VERIFY. Guarding on that state is what deadlocked the watchdog:
  // esp_ota_erase_last_boot_app_partition() is documented as "when current app
  // is marked as valid then you can erase previous app partition", so an image
  // that never gets marked can never clear the other slot's marking -- and an
  // image arrived at by rollback does not report PENDING_VERIFY, so it was
  // skipped here. The old slot stayed marked, every later set_boot_partition()
  // returned ESP_ERR_OTA_VALIDATE_FAILED, and the only escape was USB. The call
  // is idempotent and safe when nothing is pending, so there is no reason to
  // gate it.
  esp_err_t mk = esp_ota_mark_app_valid_cancel_rollback();
  otaLastStatus = (mk == ESP_OK) ? "running image marked valid"
                                 : "mark_app_valid: " + String(esp_err_to_name(mk));

  // A version that got this far is known-good; clear any bad-version marker
  // for it so a later legitimate re-release of the same number is not skipped.
  otaPrefs.begin("fw", false);
  if (otaPrefs.getUInt("bad", 0) == otaRunningVersion) {
    otaPrefs.remove("bad");
    otaPrefs.remove("badTries");
  }
  otaPrefs.putUInt("running", otaRunningVersion);
  otaPrefs.end();
}

static void otaPullBegin(const char *rtdbHost, const char *deviceId,
                         uint32_t versionCode) {
  otaRtdbHost = rtdbHost;
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
    rolledBackFrom = pending;
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
    otaLogEvent(otaRtdbHost, otaRunningVersion, version, "download-failed",
                "HTTP " + String(code));
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    otaLastStatus = "no content-length";
    otaFailures++;
    http.end();
    return false;
  }

  // Clear any INVALID/ABORTED marking on the slot we are about to write, so a
  // slot that once failed verification is not refused forever.
  //
  // Two calls, because they do different things and the first one is not
  // enough. esp_ota_erase_last_boot_app_partition() acts on the *previous boot*
  // partition, which is not necessarily the one we are about to write, and it
  // only works when the running app is already marked valid. Erasing the target
  // directly has neither restriction. Failures here are not fatal on their own
  // -- the write may still succeed -- so both are recorded and we continue.
  esp_err_t clr = esp_ota_erase_last_boot_app_partition();
  if (clr != ESP_OK && clr != ESP_ERR_NOT_FOUND)
    otaLastStatus = "erase_last_boot: " + String(esp_err_to_name(clr));

  const esp_partition_t *tgt = esp_ota_get_next_update_partition(nullptr);
  if (tgt) {
    // Erasing the whole slot is slower than letting Update.begin() do it, but
    // it is what actually clears a stale header, and this runs at most once
    // per update.
    esp_err_t te = esp_partition_erase_range(tgt, 0, tgt->size);
    if (te != ESP_OK)
      otaLastStatus = "erase target: " + String(esp_err_to_name(te));
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
    // Update.errorString() collapses every activate failure into one message,
    // so ask esp_ota_set_boot_partition() directly for the underlying reason.
    String why = Update.errorString();
    // ESP_ERR_OTA_VALIDATE_FAILED means the slot does not read back as a valid
    // image. Verify the first bytes directly so a bad flash region is
    // distinguishable from OTA bookkeeping: a mismatch here is hardware.
    {
      const esp_partition_t *t = esp_ota_get_next_update_partition(nullptr);
      uint8_t hdr[16] = {0};
      if (t && esp_partition_read(t, 0, hdr, sizeof(hdr)) == ESP_OK) {
        char hex[40];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X", hdr[0], hdr[1], hdr[2],
                 hdr[3]);
        why += " slot_hdr=" + String(hex);  // a good image starts E9
      }
    }
    // Why this much detail: set_boot_partition() has refused on this board
    // across every theory tried so far, and target_state has never once
    // appeared in the recorded message -- meaning esp_ota_get_state_partition()
    // itself keeps failing, which nothing has chased down. Report the state of
    // BOTH slots with their error codes, plus the image header the bootloader
    // actually validates, so the next failure says which check rejects it
    // instead of leaving it to be guessed at again.
    {
      const esp_partition_t *a0 = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
      const esp_partition_t *a1 = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
      for (int i = 0; i < 2; i++) {
        const esp_partition_t *pp = i ? a1 : a0;
        const char *nm = i ? " app1" : " app0";
        if (!pp) { why += String(nm) + "=missing"; continue; }
        esp_ota_img_states_t st;
        esp_err_t se = esp_ota_get_state_partition(pp, &st);
        why += String(nm) + "_state=";
        why += (se == ESP_OK) ? String((int)st)
                              : String("ERR:") + esp_err_to_name(se);
        // The bootloader validates this header, not just the magic byte.
        esp_image_header_t ih = {};
        if (esp_partition_read(pp, 0, &ih, sizeof(ih)) == ESP_OK) {
          char buf[48];
          snprintf(buf, sizeof(buf), "(magic=%02X chip=%u seg=%u)", ih.magic,
                   (unsigned)ih.chip_id, (unsigned)ih.segment_count);
          why += buf;
        }
      }
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target) {
      why += " | target=" + String(target->label) + "@0x" +
             String(target->address, HEX) + " size=" + String(target->size);
      esp_err_t e = esp_ota_set_boot_partition(target);
      why += " | set_boot_partition=" + String((int)e) + " (" +
             String(esp_err_to_name(e)) + ")";

      esp_ota_img_states_t st;
      if (esp_ota_get_state_partition(target, &st) == ESP_OK)
        why += " target_state=" + String((int)st);

      const esp_partition_t *run = esp_ota_get_running_partition();
      if (run) why += " running=" + String(run->label);
    }
    otaLastStatus = "install failed: " + why;
    otaFailures++;
    otaLogEvent(otaRtdbHost, otaRunningVersion, version, "install-failed", why);
    return false;
    // Note: no retry here. The image is written; if activation was refused,
    // the cause is recorded above and a later attempt starts clean thanks to
    // the erase at the top of this function.
  }

  otaLastStatus = "installed v" + String(version) + ", rebooting";
  // Written before the reboot: after it, this firmware no longer exists to
  // report what it did.
  otaLogEvent(otaRtdbHost, otaRunningVersion, version, "installed", "");
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

  // A version that failed to come up is skipped -- but not forever. The marker
  // cannot tell "this build genuinely crashes on boot" apart from "the power
  // was cut between writing the marker and confirming the boot", and treating
  // the second case as permanent strands the board on an old version with no
  // way back. Allow a bounded number of retries, so a genuinely broken build
  // still stops being retried while an unlucky one gets another chance.
  otaPrefs.begin("fw", false);
  uint32_t bad = otaPrefs.getUInt("bad", 0);
  uint32_t badTries = otaPrefs.getUInt("badTries", 0);
  bool skip = false;
  if (target == bad) {
    if (badTries >= OTA_BAD_RETRY_LIMIT) {
      otaLastStatus = "v" + String(target) + " failed " + String(badTries) +
                      "x, skipping";
      skip = true;
    } else {
      otaPrefs.putUInt("badTries", badTries + 1);
      otaLastStatus = "v" + String(target) + " retry " +
                      String(badTries + 1) + "/" + String(OTA_BAD_RETRY_LIMIT);
    }
  }
  otaPrefs.end();
  if (skip) return;

  otaLastStatus = "updating v" + String(otaRunningVersion) + " -> v" +
                  String(target);
  otaDownloadAndApply(url, target);
}
