// ESP32 DMX controller for an RGB pattern laser. See CLAUDE.md.
// Stage 3 step 1: runtime venue limits with PIN-protected calibration, IO0 stop
// button, settings that survive reflashes, and settings that keep running when
// phones leave. All safety logic lives in lib/laser_core (host-tested).

#include <Arduino.h>

#include <cstring>

#include "config.h"
#include "dmx_output.h"
#include "library_service.h"
#include "library_store.h"
#include "serial_console.h"
#include "settings_store.h"
#include "shared_state.h"
#include "status_led.h"
#include "web_ui.h"
#include "wifi_secrets.h"

namespace {

constexpr uint32_t SERIAL_SETTLE_MS = 200;
constexpr size_t SERIAL_RX_BUFFER_BYTES = 1024;  // a long "show save" line during a flash write
constexpr uint32_t DMX_HEALTH_CHECK_INTERVAL_MS = 1000;
constexpr uint32_t STATION_POLL_INTERVAL_MS = 250;  // asking the WiFi driver every loop pass is wasteful

SharedLaserState laserState;
uint32_t lastHealthCheckMs = 0;
uint32_t lastReportedFailures = 0;
bool isStuckButtonReported = false;
bool isWifiUp = false;
uint8_t stationCount = 0;
uint32_t lastStationPollMs = 0;
bool isLedLit = false;

void printBanner(const settings_store::LoadResult& loaded) {
  const laser::Limits& l = loaded.settings.limits;
  Serial.println();
  Serial.println("dmx-controller stage 3 step 1 (limits + /admin + debug + IO0 stop)");
  if (loaded.wasReset) {
    Serial.println("WARNING: saved settings were invalid; uncalibrated defaults loaded. Calibrate again in /admin.");
  }
  if (l.isCalibrated) {
    Serial.printf("limits: ch7 low=%u high=%u%s, ch6 %s\n", l.ch7Low, l.ch7High,
                  l.ch7Low > l.ch7High ? " (inverted)" : "", l.ch6Enabled ? "limited" : "free");
  } else {
    Serial.println("limits: NOT calibrated, the laser stays off until a limit is saved in /admin");
  }
  Serial.printf("DMX address %u. IO0 = stop button. Auto/sound modes are disabled.\n", loaded.settings.dmxAddress);
}

// Presets and shows. Static: the library is ~3.5 KB, too big for the stack.
void loadLibrary() {
  static laser::Library loadedLibrary;
  const library_store::LoadResult result = library_store::load(&loadedLibrary);
  const bool wasReset = result.wasPresetsReset || result.wasShowsReset;
  library_service::begin(loadedLibrary, result.wasPresetsReset, result.wasShowsReset);
  if (!result.isMounted) {
    Serial.println("ERROR: LittleFS could not be mounted; presets and shows can't be saved");
    return;
  }
  if (result.droppedShows > 0) {
    Serial.printf("WARNING: %u show(s) dropped because a preset they use is missing\n",
                  static_cast<unsigned>(result.droppedShows));
  }
  size_t presets = 0;
  size_t shows = 0;
  for (const laser::Preset& p : loadedLibrary.presets) presets += p.isUsed ? 1 : 0;
  for (const laser::Show& s : loadedLibrary.shows) shows += s.isUsed ? 1 : 0;
  Serial.printf("library: %u presets, %u shows%s\n", static_cast<unsigned>(presets), static_cast<unsigned>(shows),
                wasReset ? " (WARNING: a corrupt file was reset to empty)" : "");
}

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

// IO0 held low after boot means the stop button can't arm: a stuck button,
// or the USB-serial DTR/RTS circuit holding it.
void reportStuckButton(uint32_t nowMs) {
  if (isStuckButtonReported || nowMs < STOP_BUTTON_STUCK_WARNING_MS) {
    return;
  }
  isStuckButtonReported = true;
  if (!laserState.snapshot(nullptr).button.isArmed) {
    Serial.println("WARNING: IO0 held low since boot: stop button not armed (check the button or serial DTR/RTS)");
  }
}

// A short flash once a second = the firmware runs and the WiFi is up, but no phone has joined.
void updateStatusLed(uint32_t nowMs) {
  if (isWifiUp && nowMs - lastStationPollMs >= STATION_POLL_INTERVAL_MS) {
    lastStationPollMs = nowMs;
    stationCount = webUiStationCount();
  }
  const bool isOn = laser::isStatusLedOn(isWifiUp, stationCount, nowMs);
  if (isOn != isLedLit) {
    isLedLit = isOn;
    digitalWrite(PIN_STATUS_LED, isOn ? HIGH : LOW);
  }
}

void drainStopEvents() {
  for (laser::StopReason reason = laserState.takeStopEvent(); reason != laser::StopReason::None;
       reason = laserState.takeStopEvent()) {
    Serial.printf("STOP (%s): laser off\n", laser::stopReasonName(reason));
    webUiNotifyStop(reason);
  }
}

}  // namespace

// The default 8 KB is tight: a library command copies the Context (~0.5 KB,
// a running show included) several times down the call chain. "?" prints the
// unused stack of both tasks so this can be checked on the board.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() {
  // Hold the DMX line idle ("mark") until the UART takes the pin over.
  pinMode(PIN_DMX_TX, OUTPUT);
  digitalWrite(PIN_DMX_TX, HIGH);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.setRxBufferSize(SERIAL_RX_BUFFER_BYTES);  // must come before begin()
  Serial.begin(SERIAL_BAUD);
  delay(SERIAL_SETTLE_MS);

  const settings_store::LoadResult loaded = settings_store::load(ADMIN_PIN, DEFAULT_DMX_ADDRESS);
  laserState.reset(laser::makeContext(loaded.settings, ADMIN_PIN));
  printBanner(loaded);
  loadLibrary();

  if (dmxOutputBegin(&laserState)) {
    Serial.printf("dmx: sending on GPIO%u (UART%d), %lu ms per frame, laser off\n", PIN_DMX_TX, DMX_UART_NUM,
                  static_cast<unsigned long>(DMX_FRAME_PERIOD_MS));
  } else {
    Serial.println("ERROR: DMX driver install failed, no DMX output");
  }
  isWifiUp = webUiBegin(&laserState, loaded.wasReset);
  if (!isWifiUp) {
    Serial.println("ERROR: WiFi access point failed to start");  // the LED stays off
  }
  serialConsoleBegin();
  // Only the DMX task is on the task watchdog: it alone drives the output. The
  // loop is NOT watched on purpose, because library socket writes to a phone that
  // vanished can block it for ~10 s, and a reboot there would switch the laser
  // off (rule 3: settings keep running when phones leave).
}

void loop() {
  laserState.noteLoopAlive();
  serialConsolePoll(laserState);
  webUiLoop();
  drainStopEvents();
  const uint32_t nowMs = millis();
  reportStuckButton(nowMs);
  reportDmxFailures(nowMs);
  updateStatusLed(nowMs);
  delay(1);
}
