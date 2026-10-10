-- Air724UG dialer: rings a phone over VoLTE when the ESP32 watchdog says so.
--
-- The split: the ESP32 (watchdog/) does everything that needs the internet --
-- reading heartbeats, Discord, OTA -- over the house WiFi, which costs
-- nothing. This module only places calls, over 4G. It is on a zero-monthly-fee
-- prepaid card that charges by use, so it deliberately makes no data
-- connections at all; an unanswered call, which is the alarm, is not billed.
--
-- Wiring (8-pin harness of the Yinerda DTU board, V1.03):
--   red VCC, black GND     -> 12V supply (5-16V, >10W). Never to the ESP32.
--   grey GND               -> ESP32 GND (same ground as black on the board)
--   yellow RXD1            <- ESP32 GPIO17 (TX)
--   green TXD1             -> ESP32 GPIO18 (RX)
--   blue PWR_EN            <- ESP32 GPIO16; low cuts the module's power, high
--                             or floating powers it -- the ESP32's way to
--                             hard-reset a module that stopped answering
-- Micro USB is for flashing and logs only; it does not power the module.
--
-- Flash with Luatools: script download only (keeps the VoLTE core,
-- LuatOS-Air_V4022_RDA8910_TTS_NOLVGL_FLOAT), default lib added.

PROJECT = "AIR_DIALER"
VERSION = "1.0.3"

require "log"
LOG_LEVEL = log.LOGLEVEL_INFO

require "sys"
require "net"
require "ril"

-- Signal is queried once a minute so PING can report it; AT only, no data.
net.startQueryAll(60000, 60000)

-- Otherwise the module shows up as a network card when plugged into a PC for
-- flashing, and Windows may route the PC's traffic through it -- straight off
-- the prepaid card.
ril.request("AT+RNDISCALL=0,1")

require "dialer"

sys.init(0, 0)
sys.run()
