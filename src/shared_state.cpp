#include "shared_state.h"

#include <Arduino.h>

const char* failsafeReasonName(FailsafeReason reason) {
  switch (reason) {
    case FailsafeReason::None:
      return "none";
    case FailsafeReason::Timeout:
      return "timeout";
    case FailsafeReason::ClientDisconnected:
      return "client-disconnected";
    case FailsafeReason::WifiDisconnected:
      return "wifi-disconnected";
  }
  return "unknown";
}

void SharedLaserState::replaceState(const laser::State& next) {
  if (next.channels != state_.channels) {
    ++revision_;
  }
  state_ = next;
}

laser::State SharedLaserState::apply(const laser::Command& command) {
  portENTER_CRITICAL(&mux_);
  replaceState(laser::applyCommand(state_, command, millis(), limits_));
  const laser::State result = state_;
  portEXIT_CRITICAL(&mux_);
  return result;
}

void SharedLaserState::triggerFailsafe(FailsafeReason reason) {
  portENTER_CRITICAL(&mux_);
  if (!laser::isAllZero(state_.channels)) {
    pendingFailsafe_ = reason;
  }
  replaceState(laser::blackout(state_));
  portEXIT_CRITICAL(&mux_);
}

laser::Channels SharedLaserState::nextFrame(uint32_t timeoutMs) {
  portENTER_CRITICAL(&mux_);
  if (laser::isFailsafeDue(state_, millis(), timeoutMs)) {
    pendingFailsafe_ = FailsafeReason::Timeout;
    replaceState(laser::blackout(state_));
  }
  const laser::Channels frame = laser::toDmxFrame(state_, limits_);
  portEXIT_CRITICAL(&mux_);
  return frame;
}

laser::State SharedLaserState::snapshot() const {
  portENTER_CRITICAL(&mux_);
  const laser::State copy = state_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

uint32_t SharedLaserState::revision() const {
  portENTER_CRITICAL(&mux_);
  const uint32_t value = revision_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

FailsafeReason SharedLaserState::takeFailsafeEvent() {
  portENTER_CRITICAL(&mux_);
  const FailsafeReason reason = pendingFailsafe_;
  pendingFailsafe_ = FailsafeReason::None;
  portEXIT_CRITICAL(&mux_);
  return reason;
}
