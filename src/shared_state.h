#pragma once

#include <freertos/FreeRTOS.h>

#include "controller.h"

// Thread-safe owner of the controller context. Commands arrive on the loop
// task (serial and WebSocket); the DMX task ticks it every frame. Every call is
// a short, non-blocking pure function under the lock, and the timestamp is
// taken inside the lock so it is monotonic.
class SharedLaserState {
 public:
  void reset(const laser::Context& ctx);

  laser::Outcome handle(const laser::Command& command, const laser::Source& source);
  laser::Outcome commit(const laser::Settings& saved, laser::CommitKind kind);
  laser::Outcome clientLeft(uint8_t slot);

  // Library actions (see laser::loadPreset and friends).
  laser::Outcome loadPreset(uint16_t slot, const laser::Channels& look);
  laser::Outcome startShow(uint16_t slot, const laser::ActiveShow& show);
  laser::Outcome stopShow();
  laser::Outcome showDeleted(uint16_t slot);
  laser::Outcome presetSaved(uint16_t slot, const laser::Channels& look);
  laser::Outcome presetDeleted(uint16_t slot);

  // DMX task, once per frame: stop button, timeouts, then the universe to send.
  laser::Universe frameTick(bool isStopLevelLow);

  laser::Context snapshot(uint32_t* nowMs) const;
  uint32_t revision() const;

  // Stops raised by frameTick (button, calibration timeout), oldest first.
  laser::StopReason takeStopEvent();

  // Called every loop() pass. A loop stall (e.g. a blocking socket write) must
  // not look like a calibration owner who stopped sending "hb".
  void noteLoopAlive();

 private:
  laser::Outcome storeLocked(const laser::Outcome& outcome, bool isChange);

  static constexpr size_t EVENT_QUEUE_SIZE = 4;
  static constexpr uint32_t LOOP_STALL_MS = 1000;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  laser::Context ctx_{};
  uint32_t revision_ = 0;
  uint32_t lastLoopMs_ = 0;
  laser::StopReason events_[EVENT_QUEUE_SIZE] = {};
  size_t eventCount_ = 0;
};
