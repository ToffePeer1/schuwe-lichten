#pragma once

// Text command protocol shared by the serial console and the WebSocket.
// One command per line/message; see docs/PROTOCOL.md.

#include <cstddef>
#include <cstdint>

namespace laser {

constexpr size_t MAX_LINE_LENGTH = 255;
constexpr size_t PIN_MIN_DIGITS = 6;
constexpr size_t PIN_MAX_DIGITS = 8;
constexpr size_t NAME_MAX_BYTES = 32;   // preset/show names, UTF-8
constexpr size_t SHOW_MAX_STEPS = 24;

enum class CommandType : uint8_t {
  Invalid,
  // Anyone
  SetChannel,  // "<channel> <value>", e.g. "7 40"
  Blackout,    // "0": every channel to 0 (always accepted, ends calibration)
  LaserOff,    // "off": CH1 to 0 (always accepted, ends calibration)
  Heartbeat,   // "hb": keeps a calibration session alive for its owner
  Status,      // "?"
  Auth,        // "auth <pin>"
  Logout,      // "logout"
  // Admin
  ChangePin,      // "pin <old> <new>"
  PinReset,       // "pin reset" (serial only)
  SetAddress,     // "addr <1-503>"
  CalStart,       // "cal start"
  CalStop,        // "cal stop": ends a calibration or debug session
  CalAlive,       // "cal alive": operator action (kept for compatibility; there is no idle limit)
  CalLookSquare,  // "cal look square": #1 Vierkant, straight
  CalLookDiamond, // "cal look diamond": #1 with CH3 = 64 ("Ruit", static worst case)
  CalLookSpin,    // "cal look spin": #1 spinning
  DbgStart,       // "dbg start": raw channel debug session, no limits (rule 9)
  DbgStop,        // "dbg stop"
  MarkLow7,       // "lim mark low7"
  MarkHigh7,      // "lim mark high7"
  MarkLeft6,      // "lim mark left6"
  MarkRight6,     // "lim mark right6"
  HorizontalOn,   // "lim h on"
  HorizontalOff,  // "lim h off"
  LimCheck,       // "lim check"
  LimSave,        // "lim save"
  LimRedo,        // "lim redo"
  // Library (anyone on the WiFi)
  PresetSave,     // "preset save <slot> <name>"
  PresetLoad,     // "preset load <slot>"
  PresetDelete,   // "preset delete <slot>"
  ShowSave,       // "show save <slot> <p:t,p:t,...> <name>"
  ShowPlay,       // "show play <slot>"
  ShowStop,       // "show stop"
  ShowDelete,     // "show delete <slot>"
  LibraryList,    // "lib"
};

struct Command {
  CommandType type = CommandType::Invalid;
  uint8_t channel = 0;   // SetChannel
  uint16_t value = 0;    // SetChannel, SetAddress
  char pin[PIN_MAX_DIGITS + 1] = {};     // Auth, ChangePin (old)
  char newPin[PIN_MAX_DIGITS + 1] = {};  // ChangePin (new)
  // Library commands. Ranges and name content are validated by the library,
  // so it can answer with specific errors (bad-slot, bad-name, bad-steps).
  uint16_t slot = 0;
  char name[NAME_MAX_BYTES + 1] = {};  // original case, trimmed
  bool isNameTooLong = false;
  uint8_t stepCount = 0;
  uint16_t stepPresets[SHOW_MAX_STEPS] = {};
  uint16_t stepHolds[SHOW_MAX_STEPS] = {};  // tenths of a second
  bool areStepsMalformed = false;
};

// Leading/trailing whitespace is ignored, words are case-insensitive.
// Anything not matching the grammar returns CommandType::Invalid.
Command parseCommand(const char* line);

// 6-8 ASCII digits.
bool isValidPin(const char* pin);

// True for commands that need an authenticated admin (or the serial console).
bool isAdminCommand(CommandType type);

}  // namespace laser
