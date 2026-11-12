#include "settings_blob.h"

#include <cstring>

namespace laser {

namespace {

// Byte layout. Bytes 0-5 are the same in both versions.
constexpr size_t OFFSET_VERSION = 0;
constexpr size_t OFFSET_CH7_LOW = 1;
constexpr size_t OFFSET_CH7_HIGH = 2;
constexpr size_t OFFSET_FLAGS = 3;
constexpr size_t OFFSET_CH6_LEFT = 4;
constexpr size_t OFFSET_CH6_RIGHT = 5;
constexpr size_t PIN_BYTES = PIN_MAX_DIGITS + 1;  // NUL padded
constexpr size_t CRC_BYTES = 4;                   // little endian, over everything before it
// Version 2: per patch slot one type byte and a 2-byte little-endian address.
constexpr size_t OFFSET_PATCH = 6;
constexpr size_t PATCH_SLOT_BYTES = 3;
constexpr size_t OFFSET_PIN = OFFSET_PATCH + MAX_FIXTURES * PATCH_SLOT_BYTES;
constexpr size_t OFFSET_CRC = OFFSET_PIN + PIN_BYTES;
static_assert(OFFSET_CRC + CRC_BYTES == SETTINGS_BLOB_SIZE, "blob layout and size must agree");
// Version 1: the laser's address at 6, the PIN at 8.
constexpr size_t V1_OFFSET_ADDRESS = 6;
constexpr size_t V1_OFFSET_PIN = 8;
constexpr size_t V1_OFFSET_CRC = V1_OFFSET_PIN + PIN_BYTES;
static_assert(V1_OFFSET_CRC + CRC_BYTES == SETTINGS_BLOB_SIZE_V1, "version 1 layout and size must agree");

constexpr uint8_t FLAG_CH6_ENABLED = 1 << 0;
constexpr uint8_t FLAG_CALIBRATED = 1 << 1;
constexpr uint8_t FLAG_LEGACY_AUTO = 1 << 2;  // old "allow auto/sound"; auto is gone, so it is ignored
constexpr uint8_t KNOWN_FLAGS = FLAG_CH6_ENABLED | FLAG_CALIBRATED | FLAG_LEGACY_AUTO;

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

void writeU32(uint8_t* p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

void writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>(value >> 8);
}

bool isKnownType(uint8_t type) { return type <= static_cast<uint8_t>(FixtureType::Head); }

// Version, CRC, flags and the PIN terminator; then the limits and PIN, which both versions share.
bool decodeCommon(const uint8_t* in, uint8_t version, size_t pinOffset, size_t crcOffset, Settings* out) {
  const uint8_t flags = in[OFFSET_FLAGS];
  if (in[OFFSET_VERSION] != version || readU32(in + crcOffset) != crc32(in, crcOffset) ||
      (flags & ~KNOWN_FLAGS) != 0 || in[pinOffset + PIN_MAX_DIGITS] != '\0') {
    return false;
  }
  out->limits.ch7Low = in[OFFSET_CH7_LOW];
  out->limits.ch7High = in[OFFSET_CH7_HIGH];
  out->limits.ch6Enabled = (flags & FLAG_CH6_ENABLED) != 0;
  out->limits.isCalibrated = (flags & FLAG_CALIBRATED) != 0;
  out->limits.ch6Left = in[OFFSET_CH6_LEFT];
  out->limits.ch6Right = in[OFFSET_CH6_RIGHT];
  std::memcpy(out->pin, in + pinOffset, PIN_BYTES);
  return true;
}

bool decodeV1(const uint8_t* in, Settings* out) {
  if (!decodeCommon(in, 1, V1_OFFSET_PIN, V1_OFFSET_CRC, out)) {
    return false;
  }
  out->patch = laserOnlyPatch(readU16(in + V1_OFFSET_ADDRESS));
  return true;
}

bool decodeV2(const uint8_t* in, Settings* out) {
  if (!decodeCommon(in, SETTINGS_VERSION, OFFSET_PIN, OFFSET_CRC, out)) {
    return false;
  }
  for (size_t i = 0; i < MAX_FIXTURES; ++i) {
    const uint8_t* slot = in + OFFSET_PATCH + i * PATCH_SLOT_BYTES;
    if (!isKnownType(slot[0])) {
      return false;
    }
    const FixtureType type = static_cast<FixtureType>(slot[0]);
    out->patch.slots[i] = type == FixtureType::None ? PatchSlot{} : PatchSlot{type, readU16(slot + 1)};
  }
  return true;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

Settings defaultSettings(const char* factoryPin, uint16_t laserAddress) {
  Settings settings;
  settings.patch = laserOnlyPatch(laserAddress);
  if (factoryPin != nullptr) {
    std::strncpy(settings.pin, factoryPin, PIN_MAX_DIGITS);
  }
  return settings;
}

bool isValidSettings(const Settings& settings) {
  return isValidLimits(settings.limits) && checkPatch(settings.patch) == PatchStatus::Ok && isValidPin(settings.pin);
}

size_t encodeSettings(const Settings& settings, uint8_t* out, size_t size) {
  if (out == nullptr || size < SETTINGS_BLOB_SIZE) {
    return 0;
  }
  std::memset(out, 0, SETTINGS_BLOB_SIZE);
  const Limits& l = settings.limits;
  out[OFFSET_VERSION] = SETTINGS_VERSION;
  out[OFFSET_CH7_LOW] = l.ch7Low;
  out[OFFSET_CH7_HIGH] = l.ch7High;
  out[OFFSET_FLAGS] = static_cast<uint8_t>((l.ch6Enabled ? FLAG_CH6_ENABLED : 0) |
                                           (l.isCalibrated ? FLAG_CALIBRATED : 0));
  out[OFFSET_CH6_LEFT] = l.ch6Left;
  out[OFFSET_CH6_RIGHT] = l.ch6Right;
  for (size_t i = 0; i < MAX_FIXTURES; ++i) {
    uint8_t* slot = out + OFFSET_PATCH + i * PATCH_SLOT_BYTES;
    slot[0] = static_cast<uint8_t>(settings.patch.slots[i].type);
    writeU16(slot + 1, settings.patch.slots[i].address);
  }
  std::strncpy(reinterpret_cast<char*>(out + OFFSET_PIN), settings.pin, PIN_MAX_DIGITS);
  writeU32(out + OFFSET_CRC, crc32(out, OFFSET_CRC));
  return SETTINGS_BLOB_SIZE;
}

bool decodeSettings(const uint8_t* in, size_t size, Settings* out) {
  if (in == nullptr || out == nullptr) {
    return false;
  }
  Settings decoded;
  const bool isDecoded = (size == SETTINGS_BLOB_SIZE && decodeV2(in, &decoded)) ||
                         (size == SETTINGS_BLOB_SIZE_V1 && decodeV1(in, &decoded));
  if (!isDecoded || !isValidSettings(decoded)) {
    return false;
  }
  *out = decoded;
  return true;
}

}  // namespace laser
