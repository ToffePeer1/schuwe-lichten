#pragma once

// Persistent settings as one versioned, CRC-checked byte blob (one NVS entry,
// so a save is atomic). Explicit byte layout, independent of struct padding.

#include <cstddef>
#include <cstdint>

#include "laser_core.h"
#include "protocol.h"

namespace laser {

constexpr uint8_t SETTINGS_VERSION = 1;
constexpr size_t SETTINGS_BLOB_SIZE = 21;

struct Settings {
  Limits limits;
  uint16_t dmxAddress = DMX_ADDRESS_MIN;
  char pin[PIN_MAX_DIGITS + 1] = {};
};

Settings defaultSettings(const char* factoryPin, uint16_t dmxAddress);
bool isValidSettings(const Settings& settings);

// Returns the number of bytes written (SETTINGS_BLOB_SIZE), or 0 if out is too small.
size_t encodeSettings(const Settings& settings, uint8_t* out, size_t size);

// False (and *out untouched) on a wrong size, version, CRC or any out-of-range value.
bool decodeSettings(const uint8_t* in, size_t size, Settings* out);

uint32_t crc32(const uint8_t* data, size_t size);

}  // namespace laser
