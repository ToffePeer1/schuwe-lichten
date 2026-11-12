#pragma once

#include "library.h"

// Presets and shows on LittleFS (default "spiffs" partition), one CRC-checked
// file each. Never use PlatformIO "upload filesystem": it would wipe them.
namespace library_store {

struct LoadResult {
  bool isMounted;
  bool wasPresetsReset;  // a file existed but was invalid: loaded as empty
  bool wasShowsReset;     // also set when shows were dropped
  uint8_t droppedShows;   // shows erased because a step's preset is missing
};

// Mounts LittleFS (formats it on first use) and loads into *library.
LoadResult load(laser::Library* library);

// Write to a temp file, read it back and compare, then atomically rename over
// the old file. False if anything failed (the old file is then untouched).
bool savePresets(const laser::Library& library);
bool saveShows(const laser::Library& library);

}  // namespace library_store
