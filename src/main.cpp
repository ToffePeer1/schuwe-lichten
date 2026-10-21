// ESP32 DMX controller for an RGB pattern laser. See CLAUDE.md.
// Stage 2: DMX output at 40 fps + serial console + WiFi AP with web UI.
// Safety: all channels start at 0, CH7 is clamped in firmware, and a 1 s
// fail-safe blacks out when no valid command arrives or a client disconnects.

#include <Arduino.h>

#include "config.h"
#include "dmx_output.h"
#include "serial_console.h"
#include "shared_state.h"
#include "web_ui.h"

namespace {

constexpr uint32_t SERIAL_SETTLE_MS = 200;
constexpr uint32_t DMX_HEALTH_CHECK_INTERVAL_MS = 1000;
constexpr laser::Limits LIMITS{CH7_MIN, CH7_MAX, CH7_CLAMP_ENABLED};

SharedLaserState laserState(LIMITS);
uint32_t lastHealthCheckMs = 0;
uint32_t lastReportedFailures = 0;

void reportDmxFailures(uint32_t nowMs) {
  if (nowMs - lastHealthCheckMs < DMX_HEALTH_CHECK_INTERVAL_MS) {
    return;
  }
  lastHealthCheckMs = nowMs;
  const uint32_t failures = dmxSendFailures();
  if (failures != lastReportedFailures) {
    Serial.printf("WARNING: DMX send failures: %lu (+%lu)\n", static_cast<unsigned long>(failures),
                  static_cast<unsigned long>(failures - lastReportedFailures));
    lastReportedFailures = failures;
  }
}

void printSafetyBanner() {
  Serial.println();
  Serial.println("dmx-controller stage 2 (DMX + serial + WiFi web UI)");
  if (!CH7_CLAMP_ENABLED) {
    Serial.println("!!! CH7 CLAMP DISABLED - STAGE 1 LIMIT TEST BUILD (env:findlimit) !!!");
    Serial.println("!!! Aim the laser at a wall away from people.                      !!!");
  } else {
    Serial.printf("CH7 clamp: %u..%u\n", CH7_MIN, CH7_MAX);
  }
  if (!CH7_LIMITS_MEASURED) {
    Serial.println("WARNING: CH7 limits are placeholders (TODO in config.h), not measured yet");
  }
}

}  // namespace

void setup() {
  // Hold the DMX line idle ("mark") until the UART takes the pin over.
  pinMode(PIN_DMX_TX, OUTPUT);
  digitalWrite(PIN_DMX_TX, HIGH);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.begin(SERIAL_BAUD);
  delay(SERIAL_SETTLE_MS);
  printSafetyBanner();

  if (dmxOutputBegin(&laserState)) {
    Serial.printf("dmx: sending on GPIO%u (UART%d), %lu ms per frame, all channels 0\n",
                  PIN_DMX_TX, DMX_UART_NUM, static_cast<unsigned long>(DMX_FRAME_PERIOD_MS));
  } else {
    Serial.println("ERROR: DMX driver install failed, no DMX output");
  }

  if (!webUiBegin(&laserState)) {
    Serial.println("ERROR: WiFi access point failed to start");
  }
  serialConsoleBegin();
}

void loop() {
  serialConsolePoll(laserState);
  webUiLoop();

  const FailsafeReason reason = laserState.takeFailsafeEvent();
  if (reason != FailsafeReason::None) {
    Serial.printf("FAIL-SAFE (%s): all channels 0\n", failsafeReasonName(reason));
    webUiNotifyFailsafe(reason);
  }
  reportDmxFailures(millis());
  delay(1);
}
