// WiFi for a board that gets carried between sites whose networks are not
// known in advance, where nobody on site can be asked to operate a tool.
//
// Lifted from sensor-node (where every path was tested on the board, see the
// handoff), so the heartbeat boards do not grow a third hand-copied variant.
// sensor-node and watchdog still carry their own copies; they move over when
// they are next migrated.
//
// Credentials live in NVS rather than in the build, with these ways to change
// them without a reflash and without being on site:
//
//   /wifi       sets the target network. Served as a form, so the person on
//               site fills in their own network's name and password.
//   /rescue     sets the rescue hotspot itself.
//   site WiFi   RTDB /sites/<letter>/wifi {ssid, pass}: one entry for every
//               board at that pond. Fill it in before swapping a router and
//               each board stores it on its next sync; when the old network
//               then disappears they all land on the new one by themselves,
//               instead of someone visiting ten boards with a phone. A
//               fallback -- the user does not expect to need it.
//
// Connection order at boot, first success wins, cheapest first:
//
//   1. NVS target        what /wifi last stored; the normal case
//   2. NVS target_prev   while the target is unproven, so a mistyped SSID
//                        undoes itself instead of stranding the board
//   3. site WiFi         the pond's entry from RTDB, as last synced
//   4. WIFI_NETWORKS     the known sites from secrets.h
//   5. NVS rescue        the hand-opened phone hotspot, last because it only
//                        exists while a person is holding it open
//
// The sketch must define, before including this header:
//   WIFI_NETWORKS, RESCUE_SSID, RESCUE_PASS, OTA_PASSWORD   (secrets.h)
//   static void logLine(const char *fmt, ...);
//   static void siteWifiWaitTick();   // ~250ms of feedback while connecting

#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

struct WiFiNetwork {
  const char *ssid;
  const char *pass;
};
static const WiFiNetwork SITE_WIFI_KNOWN[] = WIFI_NETWORKS;

// How long to wait for one network before moving on; enough for a slow DHCP.
static const uint32_t SITE_WIFI_ATTEMPT_MS = 12000;

// ---- credential storage ------------------------------------------------
//
// Namespace "wifi", separate from pull_ota's "fw" and the registry's "reg".
//   ssid/pass     current target         pssid/ppass   previous target
//   rssid/rpass   rescue hotspot         sssid/spass   site WiFi (from RTDB)
//   pending       1 while a freshly-set target has not yet connected
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
static Credentials storedSiteWifi() { return nvsRead("sssid", "spass"); }

// Falls back to the compiled-in hotspot until /rescue has been used, so a
// board that has never been configured is still rescuable.
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
// previous one -- that is the whole revert mechanism.
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

// Promotes the previous target back to current, after a pending target
// failed, so the board returns to the network it was last happy on.
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

// Filled in as the boot proceeds; read by the incident record and status page.
static String bootAttempts;
static uint32_t bootWifiMs;
static uint32_t bootPasses;
static String wifiAttemptedSsid = "";
static bool wifiReverted = false;
static bool wifiJustConfirmed = false;  // an unproven target worked this boot
static bool onKnownSite = false;        // target failed, a known site answered
static bool onRescueNetwork = false;
static bool onSiteWifi = false;         // came up on the RTDB site entry
static String connectedSsid = "none";

static bool tryNetwork(const String &ssid, const String &pass) {
  if (!ssid.length()) return false;
  uint32_t started = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t deadline = started + SITE_WIFI_ATTEMPT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline)
    siteWifiWaitTick();

  // Which networks were tried, in what order, and how long each took is what
  // separates "wrong password" (fails fast) from "router not answering" (runs
  // the full timeout) from "weak signal" (connects slowly at a poor RSSI).
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

static bool connectWiFi() {
  onRescueNetwork = onKnownSite = onSiteWifi = false;

  Credentials target = storedTarget();
  bool pending = targetPending();
  wifiAttemptedSsid = target.ssid;

  // 1. the stored target
  if (tryNetwork(target.ssid, target.pass)) {
    connectedSsid = target.ssid;
    if (pending) {
      clearPending();
      wifiJustConfirmed = true;
    }
    return true;
  }

  // 2. the target before it -- only while the current one is still unproven.
  //    Once a target has connected, a later failure is the network being down,
  //    and silently moving back to an older network would hide that.
  if (pending) {
    Credentials prev = storedPrevious();
    if (tryNetwork(prev.ssid, prev.pass)) {
      revertTarget();
      wifiReverted = true;
      connectedSsid = prev.ssid;
      return true;
    }
  }

  // 3. the site entry from RTDB. Adopted as the target once it works, so the
  //    next boot goes straight to it instead of timing out on the old router.
  Credentials site = storedSiteWifi();
  if (site.ssid.length() && site.ssid != target.ssid &&
      tryNetwork(site.ssid, site.pass)) {
    storeTarget(site.ssid, site.pass);
    clearPending();  // it just proved itself
    onSiteWifi = true;
    connectedSsid = site.ssid;
    return true;
  }

  // 4. the known sites, which need nobody to do anything: a board that stopped
  //    working gets carried back to the family house and is caught here.
  for (const WiFiNetwork &n : SITE_WIFI_KNOWN) {
    if (tryNetwork(n.ssid, n.pass)) {
      if (pending) {
        revertTarget();
        wifiReverted = true;
      }
      onKnownSite = target.ssid.length() > 0;
      connectedSsid = n.ssid;
      return true;
    }
  }

  // 5. the rescue hotspot.
  Credentials rescue = storedRescue();
  if (tryNetwork(rescue.ssid, rescue.pass)) {
    onRescueNetwork = true;
    if (pending) {
      clearPending();
      wifiReverted = true;
    }
    connectedSsid = rescue.ssid;
    return true;
  }
  return false;
}

// Pulls the pond's shared WiFi entry and stores it if it changed. Cheap and
// safe to call periodically: it touches NVS only when the entry differs, and
// it never switches network by itself -- the entry is only tried when the
// networks ahead of it in connectWiFi() fail.
static void siteWifiSync(const char *rtdbHost, const String &site) {
  if (!site.length()) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  String url = String("https://") + rtdbHost + "/sites/" + site + "/wifi.json";
  if (!http.begin(client, url)) return;
  http.setTimeout(10000);
  int code = http.GET();
  String body = code == 200 ? http.getString() : "";
  http.end();
  if (code != 200 || body == "null") return;

  auto field = [&](const char *key) {
    String k = String("\"") + key + "\":\"";
    int s = body.indexOf(k);
    if (s < 0) return String("");
    s += k.length();
    int e = body.indexOf('"', s);
    return e < 0 ? String("") : body.substring(s, e);
  };
  String ssid = field("ssid"), pass = field("pass");
  if (!ssid.length()) return;

  Credentials cur = storedSiteWifi();
  if (cur.ssid == ssid && cur.pass == pass) return;
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("sssid", ssid);
  wifiPrefs.putString("spass", pass);
  wifiPrefs.end();
  logLine("site %s wifi entry stored: %s", site.c_str(), ssid.c_str());
}

// ---- configuration over http --------------------------------------------
//
// Two callers, completely different people: the operator types a URL and
// knows the OTA password; the person on site taps a link out of Discord and
// fills in two boxes. The form is what makes the second case possible at all.

static WebServer *siteWifiServer = nullptr;

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

// One self-contained string: no external CSS, no JavaScript, no favicon. It is
// served to a phone over a hotspot with no internet route, so anything it
// could not fetch locally would simply hang. utf-8 and the viewport line are
// load-bearing -- without them the Chinese renders as boxes and the inputs
// come out too small to tap.
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
  // The password rides along hidden so the person on site never sees it.
  h += "<input type=\"hidden\" name=\"pass\" value=\"" +
       htmlEscape(String(OTA_PASSWORD)) + "\">";
  h += F("<label>網路名稱 (SSID)</label>");
  h += "<input name=\"ssid\" value=\"" + htmlEscape(currentSsid) +
       "\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">";
  h += F("<label>密碼</label>");
  h += F("<input name=\"wpass\" autocapitalize=\"off\" autocorrect=\"off\" "
         "spellcheck=\"false\">");
  h += F("<button>儲存並重新啟動</button>");
  h += F("</form></div></body></html>");
  return h;
}

// Shown after a save. The board reboots a moment later, so this has to say
// what to expect without being able to report it.
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
  h += F("<h2>✅ 已儲存</h2>");
  h += "<p>";
  h += F("設定為：");
  h += "<b>" + htmlEscape(ssid) + "</b></p>";
  h += F("<p>板子正在重新啟動，約 30 秒。</p>");
  h += F("<p>如果紫燈停止閃爍，代表連線成功。</p>");
  h += F("<p>如果紫燈繼續閃，請保持熱點開啟並回報。</p>");
  h += F("</div></body></html>");
  return h;
}

// The OTA password gates the write, so a stray request on the pond's LAN
// cannot reconfigure a board out from under itself. Not a secrecy measure.
static bool configAuthorised() {
  return siteWifiServer->arg("pass") == OTA_PASSWORD;
}

static void handleWifiConfig() {
  WebServer &server = *siteWifiServer;
  if (!configAuthorised()) {
    server.send(403, "text/plain; charset=utf-8", "bad pass");
    return;
  }
  String ssid = server.arg("ssid");
  if (!ssid.length()) {
    Credentials cur = storedTarget();
    String status;
    if (onRescueNetwork) {
      status = F("目前在救援熱點，請填入現場的 WiFi。");
      if (wifiAttemptedSsid.length()) {
        status += F("<br>上次嘗試：");
        status += htmlEscape(wifiAttemptedSsid);
        status += F("（失敗）");
      }
    } else {
      status = F("目前連線：");
      status += htmlEscape(connectedSsid);
    }
    server.send(200, "text/html; charset=utf-8",
                configFormPage("/wifi", "設定目標 WiFi", cur.ssid, status));
    return;
  }
  storeTarget(ssid, server.arg("wpass"));
  logLine("wifi target set to %s, rebooting", ssid.c_str());
  server.send(200, "text/html; charset=utf-8", savedPage(ssid));
  delay(1200);  // let the response leave before the reset drops the socket
  ESP.restart();
}

static void handleRescueConfig() {
  WebServer &server = *siteWifiServer;
  if (!configAuthorised()) {
    server.send(403, "text/plain; charset=utf-8", "bad pass");
    return;
  }
  String ssid = server.arg("ssid");
  if (!ssid.length()) {
    Credentials cur = storedRescue();
    String status = F("只有在目標網路連不上時才會用到這組。");
    server.send(200, "text/html; charset=utf-8",
                configFormPage("/rescue", "設定救援熱點", cur.ssid, status));
    return;
  }
  storeRescue(ssid, server.arg("wpass"));
  logLine("rescue hotspot set to %s", ssid.c_str());
  // No reboot: the rescue credentials are not in use, and rebooting would
  // drop the very connection being used to set them.
  server.send(200, "text/html; charset=utf-8", savedPage(ssid));
}

static void siteWifiAttach(WebServer &server) {
  siteWifiServer = &server;
  server.on("/wifi", handleWifiConfig);
  server.on("/rescue", handleRescueConfig);
}

// Status-page lines, so every sketch reports WiFi the same way.
static String siteWifiStatus() {
  String b = "ssid: " + connectedSsid;
  if (onRescueNetwork) b += "  [RESCUE HOTSPOT]";
  else if (onSiteWifi) b += "  [FROM SITE ENTRY]";
  else if (onKnownSite) b += "  [NOT THE TARGET NETWORK]";
  b += "\n";
  Credentials t = storedTarget();
  b += "target: " + (t.ssid.length() ? t.ssid : String("(none, using defaults)"));
  if (targetPending()) b += "  [unproven]";
  b += "\n";
  Credentials pv = storedPrevious();
  if (pv.ssid.length()) b += "target prev: " + pv.ssid + "\n";
  Credentials sw = storedSiteWifi();
  if (sw.ssid.length()) b += "site entry: " + sw.ssid + "\n";
  b += "rescue: " + storedRescue().ssid + "\n";
  if (wifiReverted) b += "note: " + wifiAttemptedSsid + " failed, reverted this boot\n";
  if (wifiJustConfirmed) b += "note: target confirmed on this boot\n";
  b += "ip: " + WiFi.localIP().toString() + "\n";
  b += "rssi: " + String(WiFi.RSSI()) + " dBm\n";
  b += "wifi at boot: " + (bootAttempts.length() ? bootAttempts : String("(none)"));
  if (bootPasses) b += "  [" + String(bootPasses) + " failed passes]";
  b += "\n";
  return b;
}
