#pragma once

// Pure laser channel logic: channel layout, runtime limits and the clamps that
// keep the beam inside them. No Arduino/ESP dependencies so it can be unit-tested
// on the host. All functions take a value and return a new one.

#include <array>
#include <cstddef>
#include <cstdint>

#include "fixtures.h"

namespace laser {

constexpr size_t CHANNEL_COUNT = 10;
using Channels = std::array<uint8_t, CHANNEL_COUNT>;  // index 0 = CH1

constexpr uint8_t CH_MODE = 1;
constexpr uint8_t CH_PATTERN = 2;
constexpr uint8_t CH_ROTATION = 3;
constexpr uint8_t CH_HORIZONTAL_FLIP = 4;
constexpr uint8_t CH_VERTICAL_FLIP = 5;
constexpr uint8_t CH_HORIZONTAL_POSITION = 6;
constexpr uint8_t CH_VERTICAL_POSITION = 7;
constexpr uint8_t CH_SIZE = 8;
constexpr uint8_t CH_COLOR = 9;
constexpr uint8_t CH_DOTS = 10;

constexpr uint8_t POSITION_MAX = 127;   // CH6/CH7 128-255 = automatic sweep, never allowed when limited
constexpr uint8_t MODE_ON_MIN = 64;     // CH1 64-127 manual
constexpr uint8_t MODE_AUTO_MIN = 128;  // CH1 128-255 auto/sound: ignores the height limit, never allowed (rule 7)

// Venue limits, set by calibration. ch7Low is the CH7 value where the beam is
// lowest; it may be above ch7High when the laser hangs upside down. The
// horizontal limit is on by default at full width, which still blocks the
// CH6 128-255 automatic sweep.
struct Limits {
  uint8_t ch7Low = 0;
  uint8_t ch7High = POSITION_MAX;
  bool ch6Enabled = true;
  uint8_t ch6Left = 0;
  uint8_t ch6Right = POSITION_MAX;
  bool isCalibrated = false;
};

struct State {
  Channels channels{};
};

enum class ApplyStatus : uint8_t { Ok, AutoBlocked, NotCalibrated, BadChannel };

struct ApplyResult {
  State state;
  ApplyStatus status;
};

bool isValidLimits(const Limits& limits);

// CH7 is always clamped (and hard-capped at POSITION_MAX); CH6 only when enabled.
uint8_t clampCh7(uint8_t value, const Limits& limits);
uint8_t clampCh6(uint8_t value, const Limits& limits);

// Rule 7 (auto/sound never) and rule 8 (no light until calibrated).
bool isModeAllowed(uint8_t mode, const Limits& limits);

// Sets one channel (1-based). A forbidden CH1 value is rejected, not clamped.
ApplyResult setChannel(const State& state, uint8_t channel, uint8_t value, const Limits& limits);

State blackout();
State laserOff(const State& state);
bool isAllZero(const Channels& channels);

// The laser's 10 channels at the start of a fixture's channel array, and back.
Channels toLaserChannels(const FixtureChannels& channels);
FixtureChannels toFixtureChannels(const Channels& channels);

// Re-applies every limit: CH6/CH7 clamps, and CH1 to 0 if its mode is not allowed.
State reclamp(const State& state, const Limits& limits);

// The channels to put on the wire in normal operation. Same as reclamp(), so
// the output can never leave the limits even if the stored state somehow did.
Channels toDmxFrame(const State& state, const Limits& limits);

}  // namespace laser
