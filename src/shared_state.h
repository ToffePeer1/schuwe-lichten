#pragma once

#include <freertos/FreeRTOS.h>

#include "laser_core.h"

enum class FailsafeReason : uint8_t {
  None,
  Timeout,             // no valid command for FAILSAFE_TIMEOUT_MS
  ClientDisconnected,  // a WebSocket client went away
  WifiDisconnected,    // a phone left the access point
};

const char* failsafeReasonName(FailsafeReason reason);

// Thread-safe owner of the laser state. Written by the main loop (serial and
// WebSocket commands) and the WiFi event task; read every frame by the DMX task.
// The timestamp for the fail-safe is taken inside the lock so it is monotonic.
class SharedLaserState {
 public:
  explicit SharedLaserState(const laser::Limits& limits) : limits_(limits) {}

  // Applies an already-parsed command and returns the resulting state.
  laser::State apply(const laser::Command& command);

  // Blacks out immediately (disconnect fail-safe).
  void triggerFailsafe(FailsafeReason reason);

  // Called once per DMX frame: runs the timeout fail-safe, returns what to send.
  laser::Channels nextFrame(uint32_t timeoutMs);

  laser::State snapshot() const;

  // Increments whenever the channels change, so the web layer knows when to broadcast.
  uint32_t revision() const;

  // Returns and clears the reason of the latest fail-safe trip, or None.
  FailsafeReason takeFailsafeEvent();

  const laser::Limits& limits() const { return limits_; }

 private:
  void replaceState(const laser::State& next);  // caller holds mux_

  const laser::Limits limits_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  laser::State state_{};
  uint32_t revision_ = 0;
  FailsafeReason pendingFailsafe_ = FailsafeReason::None;
};
