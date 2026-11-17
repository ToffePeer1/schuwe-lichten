#pragma once

// Presets and shows: data model, validation, the file format, and unfolding a
// show for playback. Pure, so it is host-tested. The Library is ~6 KB, so the
// firmware keeps one live and one scratch instance on the loop task (never
// copied per frame).

#include <cstddef>
#include <cstdint>

#include "fixtures.h"
#include "laser_core.h"
#include "protocol.h"

namespace laser {

constexpr uint8_t PRESET_SLOTS = 48;  // slots 1..48
constexpr uint8_t SHOW_SLOTS = 16;    // slots 1..16
constexpr uint16_t STEP_HOLD_MIN = 1;     // tenths of a second
constexpr uint16_t STEP_HOLD_MAX = 6000;  // 10 minutes
constexpr uint8_t SHOW_MAX_DEPTH = 3;        // a show in a show in a show
constexpr uint8_t SHOW_MAX_FLAT_STEPS = 64;  // one pass through a show with every nested show unfolded
constexpr uint8_t SHOW_MAX_LOOKS = 24;       // different presets one show may use, nested shows included

// A look for one fixture type: a laser preset reaches the laser, a head preset a head.
struct Preset {
  bool isUsed = false;
  FixtureType type = FixtureType::None;
  char name[NAME_MAX_BYTES + 1] = {};
  FixtureChannels channels{};
};

// Per patch slot a preset (0 = leave that fixture alone), then either hold for
// holdTenths or play a nested show once through.
struct ShowStep {
  uint8_t cues[MAX_FIXTURES] = {};
  uint8_t subShow = 0;      // 0 = none
  uint16_t holdTenths = 0;  // 0 when subShow is set
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

enum class LibraryStatus : uint8_t {
  Ok,
  BadSlot,
  EmptySlot,
  BadName,
  BadSteps,
  PresetInUse,
  ShowInUse,    // nested in another show
  ShowLoop,     // would end up playing itself
  ShowTooLong,  // unfolded: too deep, too many steps or too many different presets
};

constexpr uint8_t NO_LOOK = 0xFF;

// A preset as a playing show holds it.
struct ShowLook {
  FixtureType type = FixtureType::None;
  uint8_t presetSlot = 0;
  FixtureChannels channels{};
};

// One step of an unfolded show: per patch slot an index into ActiveShow::looks
// (NO_LOOK = leave that fixture alone), and how long it holds.
struct FlatStep {
  uint8_t looks[MAX_FIXTURES] = {NO_LOOK, NO_LOOK, NO_LOOK, NO_LOOK};
  uint16_t holdTenths = 0;
};
static_assert(MAX_FIXTURES == 4, "FlatStep::looks lists one NO_LOOK per patch slot");

// A show unfolded for playback: nested shows played out and every look copied
// in, so the DMX task can play it without touching the library.
struct ActiveShow {
  uint8_t stepCount = 0;
  uint8_t lookCount = 0;
  FlatStep steps[SHOW_MAX_FLAT_STEPS];
  ShowLook looks[SHOW_MAX_LOOKS];
  uint64_t presetMask = 0;  // bit (slot - 1) for every preset it was built from
  uint16_t showMask = 0;    // bit (slot - 1) for every show it was built from, nested ones included
};
static_assert(PRESET_SLOTS <= 64 && SHOW_SLOTS <= 16, "the dependency masks hold one bit per slot");

// 1-32 bytes of well-formed UTF-8, no control characters, no '"' or '\'.
bool isValidName(const char* name);
bool isValidPresetSlot(uint16_t slot);
bool isValidShowSlot(uint16_t slot);
bool isPresetInUse(const Library& library, uint16_t slot);
bool isShowInUse(const Library& library, uint16_t slot);  // nested in another show

// What a preset stores. A laser look: CH1 never in auto/sound, CH6/CH7 within
// the limits. A head look: as it is (no limits).
FixtureChannels sanitizeLook(FixtureType type, const FixtureChannels& look, const Limits& limits);

LibraryStatus checkPresetSave(const Command& command);
LibraryStatus checkPresetSlot(const Library& library, uint16_t slot);  // exists?
LibraryStatus checkPresetDelete(const Library& library, uint16_t slot);
// Every step valid, and with the new show in place this show and every show
// that nests it still unfold (no loop, not too deep, long or varied).
LibraryStatus checkShowSave(const Library& library, const Command& command);
LibraryStatus checkShowSlot(const Library& library, uint16_t slot);  // exists?
LibraryStatus checkShowDelete(const Library& library, uint16_t slot);

// In-place edits (call the matching check first).
void storePreset(Library& library, uint16_t slot, const char* name, FixtureType type, const FixtureChannels& look);
void erasePreset(Library& library, uint16_t slot);
void storeShow(Library& library, const Command& command);
void eraseShow(Library& library, uint16_t slot);

// Unfolds a show for playback. stepCount is 0 if it can't: a missing preset or
// nested show, a loop, or too deep, long or varied.
ActiveShow resolveShow(const Library& library, uint16_t slot);

// The steps in the text grammar of "show save", e.g. "1=3+2=7:50,s4".
// False (out empty) if out is too small.
bool formatSteps(const Show& show, char* out, size_t size);

// Erases every show that no longer unfolds (the two files load separately, so a
// reset presets file can leave dangling shows). Returns how many.
uint8_t dropDanglingShows(Library& library);

// File format: version byte, records, CRC32. Decode leaves *out untouched on
// failure, and still reads version 1 files (laser-only presets, one look per step).
constexpr uint8_t LIBRARY_VERSION = 2;
size_t presetsBlobSize();
size_t showsBlobSize();
size_t encodePresets(const Library& library, uint8_t* out, size_t size);
size_t encodeShows(const Library& library, uint8_t* out, size_t size);
bool decodePresets(const uint8_t* in, size_t size, Library* out);
bool decodeShows(const uint8_t* in, size_t size, Library* out);

}  // namespace laser
