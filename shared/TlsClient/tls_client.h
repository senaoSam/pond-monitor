// TLS client setup shared by every HTTPS call on the pond boards.
//
// Two defaults in arduino-esp32 2.0.17 turn a flaky link into a reboot:
//
//   - The TLS handshake waits up to 120 s for the server's reply (ssl_client's
//     handshake_timeout); the connect and read timeouts HTTPClient sets do not
//     cover it. 120 s is also the boards' task watchdog, so one handshake lost
//     to a WiFi blip ended in TASK_WDT -- a 3-7 minute outage where a single
//     missed beat was all it needed to be (A01, 11-13 and 11-14).
//   - WiFiClient::setTimeout takes seconds. The setTimeout(10000) these calls
//     used to make asked for 2.7 hours; HTTPClient overrode it on connect,
//     which is the only reason it never showed. HTTPClient's own timeouts are
//     the ones that apply, so the call is gone rather than corrected.
//
// The watchdog is fed here as well, so its budget covers one call rather than
// a whole loop pass: a poll pass makes four calls in a row, and on a bad link
// each can take half a minute to fail. Worst case for one call is now about
// DNS 30 s + connect 5 s + handshake 15 s + request and reply 20 s, well
// inside 120 s. Resetting before the sketch has subscribed to the watchdog
// (calls made from setup) is a silent no-op.

#pragma once

#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

static const unsigned long TLS_HANDSHAKE_TIMEOUT_S = 15;

// RTDB and GitHub over TLS without pinning a CA on the device.
static void tlsClientPrepare(WiFiClientSecure &client) {
  esp_task_wdt_reset();
  client.setInsecure();
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
}
