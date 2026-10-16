// Why the board restarted, as one line for the incident record and the status
// page. Shared by every sketch: the three hand-copied versions it replaces all
// read the chip's raw code alone, and so got two things wrong.
//
//   1. A crash looked like our own restart. A panic, and the task watchdog
//      (which panics), restart through software, so the chip records the
//      same SW reset as ESP.restart(). Only IDF's note, left in an RTC
//      register before it restarts, tells them apart -- esp_reset_reason().
//   2. Timer group 1 is the interrupt watchdog, not the task watchdog: IDF
//      runs the 120s task watchdog on group 0 and the 300ms interrupt
//      watchdog on group 1. A hardware reset from group 1 means a CPU stopped
//      answering for long enough that not even the panic handler ran -- on
//      these boards, almost always the supply sagging without quite reaching
//      the brownout threshold.
//
// Callers sort records by keyword, so every label keeps one:
//   POWERON  power applied        BROWNOUT / GLITCH  the supply dipped
//   hang     a watchdog fired     crash              a panic, or unknown
// and our own restart carries none of them.

#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <rom/rtc.h>

static const char *resetReasonName() {
  // Hardware resets first: for these the chip's own code is unambiguous, and
  // IDF has no note to add.
  switch (rtc_get_reset_reason(0)) {
    case POWERON_RESET:          return "POWERON (power applied)";
    case RTCWDT_BROWN_OUT_RESET: return "BROWNOUT (power dip)";
    case POWER_GLITCH_RESET:
    case GLITCH_RTC_RESET:       return "GLITCH (power dip)";
    case TG1WDT_SYS_RESET:
    case TG1WDT_CPU_RESET:       return "INT_WDT (CPU froze, hang)";
    case TG0WDT_SYS_RESET:
    case TG0WDT_CPU_RESET:       return "TG0_WDT (CPU froze, hang)";
    case RTCWDT_SYS_RESET:
    case RTCWDT_CPU_RESET:
    case RTCWDT_RTC_RESET:
    case SUPER_WDT_RESET:        return "OTHER_WDT (hang)";
    case DEEPSLEEP_RESET:        return "DEEPSLEEP";
    case USB_UART_CHIP_RESET:    return "USB_UART (host reset)";
    case USB_JTAG_CHIP_RESET:    return "USB_JTAG (host reset)";
    case EFUSE_RESET:            return "EFUSE";
    case INTRUSION_RESET:        return "INTRUSION";
    default:                     break;
  }

  // A software reset: IDF noted why before it restarted.
  switch (esp_reset_reason()) {
    case ESP_RST_SW:       return "SW (our ESP.restart)";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_TASK_WDT: return "TASK_WDT (task stuck, hang)";
    case ESP_RST_INT_WDT:  return "INT_WDT (CPU froze, hang)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dip)";
    default:               break;
  }

  // Neither knows. Keep both raw codes so it can be looked up later.
  static char unknown[40];
  snprintf(unknown, sizeof(unknown), "UNKNOWN(rtc %d, idf %d, crash)",
           (int)rtc_get_reset_reason(0), (int)esp_reset_reason());
  return unknown;
}
