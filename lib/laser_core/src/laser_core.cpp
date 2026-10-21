#include "laser_core.h"

#include <cctype>
#include <cstring>

namespace laser {

namespace {

constexpr size_t MAX_LINE_LENGTH = 32;
constexpr uint8_t MAX_CHANNEL_VALUE = 255;

bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Parses an unsigned decimal number of 1-3 digits. Advances *cursor past it.
bool parseNumber(const char** cursor, unsigned* out) {
  const char* p = *cursor;
  unsigned value = 0;
  size_t digits = 0;
  while (std::isdigit(static_cast<unsigned char>(*p))) {
    if (++digits > 3) {
      return false;
    }
    value = value * 10 + static_cast<unsigned>(*p - '0');
    ++p;
  }
  if (digits == 0) {
    return false;
  }
  *cursor = p;
  *out = value;
  return true;
}

bool equalsIgnoreCase(const char* a, size_t aLength, const char* b) {
  if (aLength != std::strlen(b)) {
    return false;
  }
  for (size_t i = 0; i < aLength; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) {
      return false;
    }
  }
  return true;
}

Command parseWord(const char* word, size_t length) {
  Command command;
  if (equalsIgnoreCase(word, length, "off")) {
    command.type = CommandType::LaserOff;
  } else if (equalsIgnoreCase(word, length, "hb")) {
    command.type = CommandType::Heartbeat;
  } else if (equalsIgnoreCase(word, length, "?")) {
    command.type = CommandType::Status;
  }
  return command;
}

Command parseNumbers(const char* begin, const char* end) {
  const Command invalid;
  const char* p = begin;
  unsigned first = 0;
  if (!parseNumber(&p, &first)) {
    return invalid;
  }
  if (p == end) {
    if (first != 0) {
      return invalid;
    }
    Command command;
    command.type = CommandType::Blackout;
    return command;
  }
  if (!isSpace(*p)) {
    return invalid;
  }
  while (p < end && isSpace(*p)) {
    ++p;
  }
  unsigned second = 0;
  if (!parseNumber(&p, &second) || p != end) {
    return invalid;
  }
  if (first < 1 || first > CHANNEL_COUNT || second > MAX_CHANNEL_VALUE) {
    return invalid;
  }
  Command command;
  command.type = CommandType::SetChannel;
  command.channel = static_cast<uint8_t>(first);
  command.value = static_cast<uint8_t>(second);
  return command;
}

}  // namespace

Command parseCommand(const char* line) {
  if (line == nullptr) {
    return Command{};
  }
  const size_t length = strnlen(line, MAX_LINE_LENGTH + 1);
  if (length > MAX_LINE_LENGTH) {
    return Command{};
  }
  const char* begin = line;
  const char* end = line + length;
  while (begin < end && isSpace(*begin)) {
    ++begin;
  }
  while (end > begin && isSpace(*(end - 1))) {
    --end;
  }
  if (begin == end) {
    return Command{};
  }
  if (std::isdigit(static_cast<unsigned char>(*begin))) {
    return parseNumbers(begin, end);
  }
  return parseWord(begin, static_cast<size_t>(end - begin));
}

uint8_t clampCh7(uint8_t value, const Limits& limits) {
  if (!limits.isCh7ClampEnabled) {
    return value;
  }
  if (value < limits.ch7Min) {
    return limits.ch7Min;
  }
  if (value > limits.ch7Max) {
    return limits.ch7Max;
  }
  return value;
}

State applyCommand(const State& state, const Command& command, uint32_t nowMs,
                   const Limits& limits) {
  State next = state;
  switch (command.type) {
    case CommandType::Invalid:
    case CommandType::Status:
      return state;
    case CommandType::SetChannel: {
      const uint8_t value = command.channel == CH_VERTICAL_POSITION
                                ? clampCh7(command.value, limits)
                                : command.value;
      next.channels[command.channel - 1] = value;
      break;
    }
    case CommandType::Blackout:
      next.channels.fill(0);
      break;
    case CommandType::LaserOff:
      next.channels[CH_MODE - 1] = 0;
      break;
    case CommandType::Heartbeat:
      break;
  }
  next.lastValidCommandMs = nowMs;
  return next;
}

State blackout(const State& state) {
  State next = state;
  next.channels.fill(0);
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

bool isFailsafeDue(const State& state, uint32_t nowMs, uint32_t timeoutMs) {
  const uint32_t elapsedMs = nowMs - state.lastValidCommandMs;
  return elapsedMs >= timeoutMs && !isAllZero(state.channels);
}

Channels toDmxFrame(const State& state, const Limits& limits) {
  Channels frame = state.channels;
  frame[CH_VERTICAL_POSITION - 1] = clampCh7(frame[CH_VERTICAL_POSITION - 1], limits);
  return frame;
}

}  // namespace laser
