#include "laser_core.h"

namespace laser {

namespace {

uint8_t capPosition(uint8_t value) { return value > POSITION_MAX ? POSITION_MAX : value; }

uint8_t clampBetween(uint8_t value, uint8_t endA, uint8_t endB) {
  const uint8_t lo = capPosition(endA < endB ? endA : endB);
  const uint8_t hi = capPosition(endA < endB ? endB : endA);
  const uint8_t capped = capPosition(value);
  if (capped < lo) {
    return lo;
  }
  return capped > hi ? hi : capped;
}

}  // namespace

bool isValidLimits(const Limits& limits) {
  return limits.ch7Low <= POSITION_MAX && limits.ch7High <= POSITION_MAX &&
         limits.ch6Left <= POSITION_MAX && limits.ch6Right <= POSITION_MAX;
}

uint8_t clampCh7(uint8_t value, const Limits& limits) {
  return clampBetween(value, limits.ch7Low, limits.ch7High);
}

uint8_t clampCh6(uint8_t value, const Limits& limits) {
  return limits.ch6Enabled ? clampBetween(value, limits.ch6Left, limits.ch6Right) : value;
}

bool isModeAllowed(uint8_t mode, const Limits& limits) {
  if (mode >= MODE_AUTO_MIN) {
    return false;
  }
  return mode < MODE_ON_MIN || limits.isCalibrated;
}

ApplyResult setChannel(const State& state, uint8_t channel, uint8_t value, const Limits& limits) {
  if (channel < 1 || channel > CHANNEL_COUNT) {
    return {state, ApplyStatus::BadChannel};
  }
  if (channel == CH_MODE && !isModeAllowed(value, limits)) {
    return {state, value >= MODE_AUTO_MIN ? ApplyStatus::AutoBlocked : ApplyStatus::NotCalibrated};
  }
  uint8_t applied = value;
  if (channel == CH_VERTICAL_POSITION) {
    applied = clampCh7(value, limits);
  } else if (channel == CH_HORIZONTAL_POSITION) {
    applied = clampCh6(value, limits);
  }
  State next = state;
  next.channels[channel - 1] = applied;
  return {next, ApplyStatus::Ok};
}

State blackout() { return State{}; }

State laserOff(const State& state) {
  State next = state;
  next.channels[CH_MODE - 1] = 0;
  return next;
}

bool isAllZero(const Channels& channels) {
  for (const uint8_t value : channels) {
    if (value != 0) {
      return false;
    }
  }
  return true;
}

State reclamp(const State& state, const Limits& limits) {
  State next = state;
  next.channels[CH_VERTICAL_POSITION - 1] = clampCh7(state.channels[CH_VERTICAL_POSITION - 1], limits);
  next.channels[CH_HORIZONTAL_POSITION - 1] =
      clampCh6(state.channels[CH_HORIZONTAL_POSITION - 1], limits);
  if (!isModeAllowed(state.channels[CH_MODE - 1], limits)) {
    next.channels[CH_MODE - 1] = 0;
  }
  return next;
}

Channels toDmxFrame(const State& state, const Limits& limits) { return reclamp(state, limits).channels; }

}  // namespace laser
