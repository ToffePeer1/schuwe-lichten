#pragma once

#include "settings_blob.h"

// Persists laser::Settings as one CRC-checked NVS blob (atomic per entry).
namespace settings_store {

struct LoadResult {
  laser::Settings settings;
  bool wasMissing;  // first boot: defaults, nothing saved yet
  bool wasReset;    // a blob existed but was invalid: defaults loaded instead
};

// First boot: one laser at defaultLaserAddress. A version 1 blob is migrated on load
// (and rewritten as version 2 by the next save).
LoadResult load(const char* factoryPin, uint16_t defaultLaserAddress);

// Writes the blob, reads it back and compares. False if anything went wrong.
bool save(const laser::Settings& settings);

}  // namespace settings_store
