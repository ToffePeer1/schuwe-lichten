#pragma once

// Presets and shows: data model, validation and the file format. Pure, so it
// is host-tested. The Library is ~3.5 KB, so the firmware keeps ONE static
// instance and edits it in place on the loop task (never copied per frame).

#include <cstddef>
#include <cstdint>

#include "laser_core.h"
#include "protocol.h"

namespace laser {

constexpr uint8_t PRESET_SLOTS = 32;  // slots 1..32
constexpr uint8_t SHOW_SLOTS = 16;    // slots 1..16
constexpr uint16_t STEP_HOLD_MIN = 1;     // tenths of a second
constexpr uint16_t STEP_HOLD_MAX = 6000;  // 10 minutes

struct Preset {
  bool isUsed = false;
  char name[NAME_MAX_BYTES + 1] = {};
  Channels channels{};
};

struct ShowStep {
  uint8_t preset = 0;        // preset slot, 1-based
  uint16_t holdTenths = 0;
};

struct Show {
  bool isUsed = false;
  char name[NAME_MAX_BYTES + 1] = {};
  uint8_t stepCount = 0;
  ShowStep steps[SHOW_MAX_STEPS];
};

struct Library {
  Preset presets[PRESET_SLOTS];
  Show shows[SHOW_SLOTS];
};

enum class LibraryStatus : uint8_t { Ok, BadSlot, EmptySlot, BadName, BadSteps, PresetInUse };

// A show resolved for playback: every step's look copied in, so the DMX task
// can play it without touching the library.
struct ActiveShow {
  uint8_t stepCount = 0;
  uint8_t presetSlots[SHOW_MAX_STEPS] = {};
  uint16_t holdTenths[SHOW_MAX_STEPS] = {};
  Channels looks[SHOW_MAX_STEPS] = {};
};

// 1-32 bytes of well-formed UTF-8, no control characters, no '"' or '\'.
bool isValidName(const char* name);
bool isValidPresetSlot(uint16_t slot);
bool isValidShowSlot(uint16_t slot);
bool isPresetInUse(const Library& library, uint16_t slot);

// What a preset stores: CH1 never in auto/sound, CH6/CH7 within the limits.
Channels sanitizeLook(const Channels& look, const Limits& limits);

LibraryStatus checkPresetSave(const Command& command);
LibraryStatus checkPresetSlot(const Library& library, uint16_t slot);  // exists?
LibraryStatus checkPresetDelete(const Library& library, uint16_t slot);
LibraryStatus checkShowSave(const Library& library, const Command& command);
LibraryStatus checkShowSlot(const Library& library, uint16_t slot);  // exists?

// In-place edits (call the matching check first).
void storePreset(Library& library, uint16_t slot, const char* name, const Channels& look);
void erasePreset(Library& library, uint16_t slot);
void storeShow(Library& library, const Command& command);
void eraseShow(Library& library, uint16_t slot);

ActiveShow resolveShow(const Library& library, uint16_t slot);

// Erases every show with a step whose preset is missing (the two files load
// separately, so a reset presets file can leave them). Returns how many.
uint8_t dropDanglingShows(Library& library);

// File format: version byte, records, CRC32. Decode leaves *out untouched on failure.
constexpr uint8_t LIBRARY_VERSION = 1;
size_t presetsBlobSize();
size_t showsBlobSize();
size_t encodePresets(const Library& library, uint8_t* out, size_t size);
size_t encodeShows(const Library& library, uint8_t* out, size_t size);
bool decodePresets(const uint8_t* in, size_t size, Library* out);
bool decodeShows(const uint8_t* in, size_t size, Library* out);

}  // namespace laser
