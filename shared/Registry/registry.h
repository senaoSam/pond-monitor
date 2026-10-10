// Which board am I? Answered by the registry, not by the build.
//
// The heartbeat boards all run one identical image -- twenty of them, each
// watching a different power supply. Baking an id into the build would mean a
// separate build per board and a wrong label the moment two got swapped. So
// the identity is looked up at boot by the chip's own MAC, which is unique and
// cannot be mixed up:
//
//   RTDB  /registry/<MAC>  { "id": "A05", "note": "...", "canary": true }
//
// written once per board on the bench by tools/register.sh, when the label is
// stuck on. Replacing a dead board is then a matter of registering the new one
// under the old label; nothing is rebuilt.
//
// The answer is cached in NVS, so a boot that cannot reach RTDB still knows
// who it is. A board that has never been registered reports as "esp-<mac>":
// unique, so it can never overwrite a real board's heartbeat, and obviously
// unlabelled, so the watchdog can say so.

#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

struct Identity {
  String mac;          // 12 uppercase hex digits, no separators
  String id;           // "A05", or "esp-<mac>" when unregistered
  String note;         // what this board watches; free text, may be empty
  bool canary = false; // pulls the canary firmware group
  bool registered = false;
  String source;       // "registry", "cache" or "unregistered" -- for the status page
};

static Preferences regPrefs;

// The station MAC, which is what esptool prints as "MAC:" when the board is
// on the bench -- so tools/register.sh and the board agree on the key.
static String registryMac() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3],
           m[4], m[5]);
  return String(buf);
}

// Ids become RTDB keys and Discord text, so only plain characters are allowed;
// anything else would make an unreachable path or a broken message.
static bool registryIdValid(const String &id) {
  if (id.length() < 1 || id.length() > 16) return false;
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  }
  return true;
}

// Pulls a string field out of a flat JSON object. The registry entry is a
// handful of keys, so a full parser would be weight with no purpose.
static String registryField(const String &json, const char *key) {
  String k = String("\"") + key + "\":\"";
  int s = json.indexOf(k);
  if (s < 0) return "";
  s += k.length();
  int e = json.indexOf('"', s);
  return e < 0 ? "" : json.substring(s, e);
}

// Looks this board up. Never fails: the worst case is an unregistered identity
// that is still unique.
static Identity registryResolve(const char *rtdbHost) {
  Identity me;
  me.mac = registryMac();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  String url = String("https://") + rtdbHost + "/registry/" + me.mac + ".json";
  int code = -1;
  String body;
  if (http.begin(client, url)) {
    http.setTimeout(10000);
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
  }

  if (code == 200 && body != "null") {
    String id = registryField(body, "id");
    if (registryIdValid(id)) {
      me.id = id;
      me.note = registryField(body, "note");
      me.canary = body.indexOf("\"canary\":true") >= 0;
      me.registered = true;
      me.source = "registry";
      // Only rewritten when something changed: NVS is flash, and this runs
      // on every boot.
      regPrefs.begin("reg", false);
      if (regPrefs.getString("id", "") != me.id) regPrefs.putString("id", me.id);
      if (regPrefs.getString("note", "") != me.note)
        regPrefs.putString("note", me.note);
      if (regPrefs.getBool("canary", false) != me.canary)
        regPrefs.putBool("canary", me.canary);
      regPrefs.end();
      return me;
    }
  }

  // RTDB answered "not registered" (null). Deliberately forget any cached id:
  // a board that was unregistered on purpose -- moved to another label, or
  // retired -- must not keep publishing under its old name.
  if (code == 200 && body == "null") {
    regPrefs.begin("reg", false);
    regPrefs.clear();
    regPrefs.end();
  } else {
    // RTDB could not be asked. Trust what it said last time.
    regPrefs.begin("reg", true);
    String id = regPrefs.getString("id", "");
    if (registryIdValid(id)) {
      me.id = id;
      me.note = regPrefs.getString("note", "");
      me.canary = regPrefs.getBool("canary", false);
      me.registered = true;
      me.source = "cache";
      regPrefs.end();
      return me;
    }
    regPrefs.end();
  }

  me.id = "esp-" + me.mac;
  me.id.toLowerCase();
  me.source = "unregistered";
  return me;
}

// The site an id belongs to is its leading letter: A05 is at pond A. Empty for
// an unregistered board, which belongs nowhere yet.
static String registrySite(const Identity &me) {
  if (!me.registered || !me.id.length() || !isalpha((unsigned char)me.id[0]))
    return "";
  return String((char)toupper((unsigned char)me.id[0]));
}
