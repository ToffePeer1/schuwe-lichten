#pragma once

// Pure safety logic for the laser: command parsing, CH7 clamp and fail-safe.
// No Arduino/ESP dependencies so it can be unit-tested on the host.
// All functions take the current state and return a new one.

#include <array>
#include <cstddef>
#include <cstdint>

namespace laser {

constexpr size_t CHANNEL_COUNT = 10;
using Channels = std::array<uint8_t, CHANNEL_COUNT>;  // index 0 = CH1

constexpr uint8_t CH_MODE = 1;
constexpr uint8_t CH_VERTICAL_POSITION = 7;

struct Limits {
  uint8_t ch7Min;
  uint8_t ch7Max;
  bool isCh7ClampEnabled;
};

struct State {
  Channels channels{};  // as commanded, CH7 already clamped
  uint32_t lastValidCommandMs = 0;
};

enum class CommandType {
  Invalid,
  SetChannel,  // "<channel> <value>", e.g. "7 40"
  Blackout,    // "0": every channel to 0
  LaserOff,    // "off": CH1 to 0, other channels kept
  Heartbeat,   // "hb": keeps the fail-safe from tripping, changes nothing
  Status,      // "?": query only, does NOT feed the fail-safe
};

struct Command {
  CommandType type = CommandType::Invalid;
  uint8_t channel = 0;  // 1-based, SetChannel only
  uint8_t value = 0;    // SetChannel only
};

// Parses one line of text. Leading/trailing whitespace is ignored.
// Anything not matching the grammar above returns CommandType::Invalid.
Command parseCommand(const char* line);

uint8_t clampCh7(uint8_t value, const Limits& limits);

// Applies a parsed command. Invalid commands return the state unchanged and do
// not feed the fail-safe. CH1 only ever changes through SetChannel/Blackout/LaserOff.
State applyCommand(const State& state, const Command& command, uint32_t nowMs,
                   const Limits& limits);

// Every channel to 0. Used for the fail-safe and on disconnect.
State blackout(const State& state);

bool isAllZero(const Channels& channels);

// True when no valid command arrived for timeoutMs and the output is not
// already dark. Unsigned arithmetic, so millis() wrap-around is handled.
bool isFailsafeDue(const State& state, uint32_t nowMs, uint32_t timeoutMs);

// Channels to put on the wire. Re-applies the CH7 clamp as a second line of
// defence, so the output can never leave CH7_MIN..CH7_MAX while clamping is on.
Channels toDmxFrame(const State& state, const Limits& limits);

}  // namespace laser
