// Board verifier: reports what a board actually contains.
//
// Written after a board silkscreened ESP32-S3-N16R8 turned out to have no
// PSRAM at all. Declaring absent PSRAM in platformio.ini breaks OTA and
// nothing else, which is very hard to diagnose later -- so check first and
// write the project config to match what is really there.
//
// Flash over USB, then read the result from the RGB LED colour. No WiFi, so
// this works on a bench with no network:
//   green  = PSRAM present and sized as expected (>= 2MB)
//   yellow = no PSRAM (a plain N16 -- fine, but do not declare PSRAM)
//   red    = PSRAM present but tiny/odd, inspect the reported numbers
//
// The exact figures also go out over UART0 and USB CDC for whichever console
// the board happens to expose.

#include <Arduino.h>

static const int LED_PIN = 48;

static void led(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(LED_PIN, r, g, b);
}

static void report() {
  size_t psram = ESP.getPsramSize();
  size_t flash = ESP.getFlashChipSize();

  char buf[320];
  snprintf(buf, sizeof(buf),
           "\n=== board check ===\n"
           "chip      : %s rev%d, %d core(s) @ %luMHz\n"
           "flash     : %u bytes (%uMB)\n"
           "psram     : %u bytes (%uMB)\n"
           "heap      : %u free of %u\n"
           "mac       : %llX\n"
           "verdict   : %s\n"
           "===================\n",
           ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
           (unsigned long)getCpuFrequencyMhz(), (unsigned)flash,
           (unsigned)(flash / 1048576), (unsigned)psram,
           (unsigned)(psram / 1048576), (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getHeapSize(), ESP.getEfuseMac(),
           psram == 0 ? "NO PSRAM -- omit psram_type and BOARD_HAS_PSRAM"
                      : "PSRAM present -- psram_type/BOARD_HAS_PSRAM are safe");

  Serial.print(buf);
  Serial0.print(buf);

  if (psram == 0)        led(40, 25, 0);   // yellow
  else if (psram < 2097152) led(40, 0, 0); // red
  else                   led(0, 40, 0);    // green
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(2000);
  report();
}

void loop() {
  // Re-print periodically so a console attached late still sees the result.
  delay(5000);
  report();
}
