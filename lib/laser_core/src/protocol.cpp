#include "protocol.h"

#include <cctype>
#include <cstring>

#include "laser_core.h"

namespace laser {

namespace {

constexpr size_t MAX_TOKENS = 4;
constexpr uint8_t MAX_CHANNEL_VALUE = 255;

struct Tokens {
  char buffer[MAX_LINE_LENGTH + 1];
  const char* items[MAX_TOKENS];
  size_t count;
};

bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Splits into lower-cased tokens. Returns false for too many tokens.
bool tokenize(const char* line, size_t length, Tokens* out) {
  out->count = 0;
  for (size_t i = 0; i < length; ++i) {
    out->buffer[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
  }
  out->buffer[length] = '\0';
  char* p = out->buffer;
  while (*p != '\0') {
    while (isSpace(*p)) {
      *p++ = '\0';
    }
    if (*p == '\0') {
      break;
    }
    if (out->count == MAX_TOKENS) {
      return false;
    }
    out->items[out->count++] = p;
    while (*p != '\0' && !isSpace(*p)) {
      ++p;
    }
  }
  return true;
}

// Unsigned decimal of 1..maxDigits digits, nothing else.
bool parseNumber(const char* token, size_t maxDigits, unsigned* out) {
  const size_t length = std::strlen(token);
  if (length == 0 || length > maxDigits) {
    return false;
  }
  unsigned value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (!isDigit(token[i])) {
      return false;
    }
    value = value * 10 + static_cast<unsigned>(token[i] - '0');
  }
  *out = value;
  return true;
}

bool is(const char* token, const char* word) { return std::strcmp(token, word) == 0; }

Command make(CommandType type) {
  Command command;
  command.type = type;
  return command;
}

Command parseNumbers(const Tokens& t) {
  unsigned first = 0;
  if (!parseNumber(t.items[0], 3, &first)) {
    return Command{};
  }
  if (t.count == 1) {
    return first == 0 ? make(CommandType::Blackout) : Command{};
  }
  unsigned second = 0;
  if (t.count != 2 || !parseNumber(t.items[1], 3, &second)) {
    return Command{};
  }
  if (first < 1 || first > CHANNEL_COUNT || second > MAX_CHANNEL_VALUE) {
    return Command{};
  }
  Command command = make(CommandType::SetChannel);
  command.channel = static_cast<uint8_t>(first);
  command.value = static_cast<uint16_t>(second);
  return command;
}

Command parsePin(const Tokens& t) {
  if (t.count == 2 && is(t.items[1], "reset")) {
    return make(CommandType::PinReset);
  }
  if (t.count != 3 || !isValidPin(t.items[1]) || !isValidPin(t.items[2])) {
    return Command{};
  }
  Command command = make(CommandType::ChangePin);
  std::strncpy(command.pin, t.items[1], PIN_MAX_DIGITS);
  std::strncpy(command.newPin, t.items[2], PIN_MAX_DIGITS);
  return command;
}

Command parseCal(const Tokens& t) {
  if (t.count == 2) {
    if (is(t.items[1], "start")) return make(CommandType::CalStart);
    if (is(t.items[1], "stop")) return make(CommandType::CalStop);
    if (is(t.items[1], "alive")) return make(CommandType::CalAlive);
    return Command{};
  }
  if (t.count == 3 && is(t.items[1], "look")) {
    if (is(t.items[2], "square")) return make(CommandType::CalLookSquare);
    if (is(t.items[2], "diamond")) return make(CommandType::CalLookDiamond);
    if (is(t.items[2], "spin")) return make(CommandType::CalLookSpin);
  }
  return Command{};
}

Command parseDbg(const Tokens& t) {
  if (t.count == 2 && is(t.items[1], "start")) return make(CommandType::DbgStart);
  if (t.count == 2 && is(t.items[1], "stop")) return make(CommandType::DbgStop);
  return Command{};
}

Command parseLim(const Tokens& t) {
  if (t.count == 2) {
    if (is(t.items[1], "check")) return make(CommandType::LimCheck);
    if (is(t.items[1], "save")) return make(CommandType::LimSave);
    if (is(t.items[1], "redo")) return make(CommandType::LimRedo);
    return Command{};
  }
  if (t.count != 3) {
    return Command{};
  }
  if (is(t.items[1], "mark")) {
    if (is(t.items[2], "low7")) return make(CommandType::MarkLow7);
    if (is(t.items[2], "high7")) return make(CommandType::MarkHigh7);
    if (is(t.items[2], "left6")) return make(CommandType::MarkLeft6);
    if (is(t.items[2], "right6")) return make(CommandType::MarkRight6);
    return Command{};
  }
  if (is(t.items[1], "h")) {
    if (is(t.items[2], "on")) return make(CommandType::HorizontalOn);
    if (is(t.items[2], "off")) return make(CommandType::HorizontalOff);
  }
  return Command{};
}

Command parseWords(const Tokens& t) {
  const char* word = t.items[0];
  if (t.count == 1) {
    if (is(word, "off")) return make(CommandType::LaserOff);
    if (is(word, "hb")) return make(CommandType::Heartbeat);
    if (is(word, "?")) return make(CommandType::Status);
    if (is(word, "logout")) return make(CommandType::Logout);
    return Command{};
  }
  if (is(word, "auth")) {
    if (t.count != 2 || !isValidPin(t.items[1])) {
      return Command{};
    }
    Command command = make(CommandType::Auth);
    std::strncpy(command.pin, t.items[1], PIN_MAX_DIGITS);
    return command;
  }
  if (is(word, "pin")) return parsePin(t);
  if (is(word, "cal")) return parseCal(t);
  if (is(word, "dbg")) return parseDbg(t);
  if (is(word, "lim")) return parseLim(t);
  unsigned address = 0;
  if (is(word, "addr") && t.count == 2 && parseNumber(t.items[1], 3, &address) &&
      address >= DMX_ADDRESS_MIN && address <= DMX_ADDRESS_MAX) {
    Command command = make(CommandType::SetAddress);
    command.value = static_cast<uint16_t>(address);
    return command;
  }
  return Command{};
}

// --- library commands: names keep their case and may contain spaces ---------

struct Cursor {
  const char* p;
  const char* end;
};

void skipSpaces(Cursor* c) {
  while (c->p < c->end && isSpace(*c->p)) {
    ++c->p;
  }
}

// Next whitespace-delimited word, lower-cased into out (truncated to size-1).
size_t nextWord(Cursor* c, char* out, size_t size) {
  skipSpaces(c);
  size_t n = 0;
  while (c->p < c->end && !isSpace(*c->p)) {
    if (n + 1 < size) {
      out[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*c->p)));
    }
    ++c->p;
  }
  out[n] = '\0';
  return n;
}

bool atEnd(Cursor* c) {
  skipSpaces(c);
  return c->p == c->end;
}

bool readSlot(Cursor* c, Command* command) {
  char word[8];
  unsigned value = 0;
  if (nextWord(c, word, sizeof(word)) == 0 || !parseNumber(word, 3, &value)) {
    return false;
  }
  command->slot = static_cast<uint16_t>(value);
  return true;
}

// The rest of the line, trimmed, as the name (original case).
void readName(Cursor* c, Command* command) {
  skipSpaces(c);
  const char* end = c->end;
  while (end > c->p && isSpace(*(end - 1))) {
    --end;
  }
  const size_t length = static_cast<size_t>(end - c->p);
  command->isNameTooLong = length > NAME_MAX_BYTES;
  const size_t copied = length > NAME_MAX_BYTES ? NAME_MAX_BYTES : length;
  std::memcpy(command->name, c->p, copied);
  command->name[copied] = '\0';
  c->p = c->end;
}

// "p:t,p:t,...": syntax only; ranges are checked by the library.
void parseSteps(const char* word, Command* command) {
  const char* p = word;
  while (*p != '\0') {
    if (command->stepCount == SHOW_MAX_STEPS) {
      command->areStepsMalformed = true;
      return;
    }
    char part[16];
    size_t n = 0;
    while (*p != '\0' && *p != ',' && n + 1 < sizeof(part)) {
      part[n++] = *p++;
    }
    part[n] = '\0';
    if (*p == ',') {
      ++p;
      if (*p == '\0') {
        command->areStepsMalformed = true;  // trailing comma
        return;
      }
    } else if (*p != '\0') {
      command->areStepsMalformed = true;  // part too long
      return;
    }
    char* colon = std::strchr(part, ':');
    unsigned preset = 0;
    unsigned hold = 0;
    if (colon == nullptr) {
      command->areStepsMalformed = true;
      return;
    }
    *colon = '\0';
    if (!parseNumber(part, 3, &preset) || !parseNumber(colon + 1, 4, &hold)) {
      command->areStepsMalformed = true;
      return;
    }
    command->stepPresets[command->stepCount] = static_cast<uint16_t>(preset);
    command->stepHolds[command->stepCount] = static_cast<uint16_t>(hold);
    ++command->stepCount;
  }
  if (command->stepCount == 0) {
    command->areStepsMalformed = true;
  }
}

Command parsePresetCommand(Cursor* c) {
  char action[8];
  nextWord(c, action, sizeof(action));
  Command command;
  if (is(action, "save")) {
    if (!readSlot(c, &command) || atEnd(c)) {
      return Command{};
    }
    readName(c, &command);
    command.type = CommandType::PresetSave;
    return command;
  }
  const CommandType type = is(action, "load") ? CommandType::PresetLoad
                           : is(action, "delete") ? CommandType::PresetDelete
                                                  : CommandType::Invalid;
  if (type == CommandType::Invalid || !readSlot(c, &command) || !atEnd(c)) {
    return Command{};
  }
  command.type = type;
  return command;
}

Command parseShowCommand(Cursor* c) {
  char action[8];
  nextWord(c, action, sizeof(action));
  Command command;
  if (is(action, "stop")) {
    return atEnd(c) ? make(CommandType::ShowStop) : Command{};
  }
  if (is(action, "save")) {
    char steps[MAX_LINE_LENGTH + 1];
    if (!readSlot(c, &command) || nextWord(c, steps, sizeof(steps)) == 0 || atEnd(c)) {
      return Command{};
    }
    parseSteps(steps, &command);
    readName(c, &command);
    command.type = CommandType::ShowSave;
    return command;
  }
  const CommandType type = is(action, "play") ? CommandType::ShowPlay
                           : is(action, "delete") ? CommandType::ShowDelete
                                                  : CommandType::Invalid;
  if (type == CommandType::Invalid || !readSlot(c, &command) || !atEnd(c)) {
    return Command{};
  }
  command.type = type;
  return command;
}

// Returns true and fills *out if the line is a library command (valid or not).
bool parseLibraryCommand(const char* line, size_t length, Command* out) {
  Cursor c{line, line + length};
  char first[8];
  nextWord(&c, first, sizeof(first));
  if (is(first, "preset")) {
    *out = parsePresetCommand(&c);
    return true;
  }
  if (is(first, "show")) {
    *out = parseShowCommand(&c);
    return true;
  }
  if (is(first, "lib")) {
    *out = atEnd(&c) ? make(CommandType::LibraryList) : Command{};
    return true;
  }
  return false;
}


}  // namespace

bool isValidPin(const char* pin) {
  if (pin == nullptr) {
    return false;
  }
  const size_t length = strnlen(pin, PIN_MAX_DIGITS + 1);
  if (length < PIN_MIN_DIGITS || length > PIN_MAX_DIGITS) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!isDigit(pin[i])) {
      return false;
    }
  }
  return true;
}

Command parseCommand(const char* line) {
  if (line == nullptr) {
    return Command{};
  }
  const size_t length = strnlen(line, MAX_LINE_LENGTH + 1);
  if (length > MAX_LINE_LENGTH) {
    return Command{};
  }
  Command library;
  if (parseLibraryCommand(line, length, &library)) {
    return library;
  }
  Tokens tokens;
  if (!tokenize(line, length, &tokens) || tokens.count == 0) {
    return Command{};
  }
  return isDigit(tokens.items[0][0]) ? parseNumbers(tokens) : parseWords(tokens);
}

bool isAdminCommand(CommandType type) {
  switch (type) {
    case CommandType::Invalid:
    case CommandType::SetChannel:
    case CommandType::Blackout:
    case CommandType::LaserOff:
    case CommandType::Heartbeat:
    case CommandType::Status:
    case CommandType::Auth:
    case CommandType::Logout:
    case CommandType::PresetSave:  // the library is open to everyone on the WiFi
    case CommandType::PresetLoad:
    case CommandType::PresetDelete:
    case CommandType::ShowSave:
    case CommandType::ShowPlay:
    case CommandType::ShowStop:
    case CommandType::ShowDelete:
    case CommandType::LibraryList:
      return false;
    default:
      return true;
  }
}

}  // namespace laser
