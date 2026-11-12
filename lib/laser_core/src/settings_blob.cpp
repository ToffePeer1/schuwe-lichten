#include "settings_blob.h"

#include <cstring>

namespace laser {

namespace {

// Byte layout.
constexpr size_t OFFSET_VERSION = 0;
constexpr size_t OFFSET_CH7_LOW = 1;
constexpr size_t OFFSET_CH7_HIGH = 2;
constexpr size_t OFFSET_FLAGS = 3;
constexpr size_t OFFSET_CH6_LEFT = 4;
constexpr size_t OFFSET_CH6_RIGHT = 5;
constexpr size_t OFFSET_ADDRESS = 6;  // 2 bytes, little endian
constexpr size_t OFFSET_PIN = 8;      // PIN_MAX_DIGITS + 1 bytes, NUL padded
constexpr size_t OFFSET_CRC = OFFSET_PIN + PIN_MAX_DIGITS + 1;  // 4 bytes, little endian
static_assert(OFFSET_CRC + 4 == SETTINGS_BLOB_SIZE, "blob layout and size must agree");

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

Settings defaultSettings(const char* factoryPin, uint16_t dmxAddress) {
  Settings settings;
  settings.dmxAddress = dmxAddress;
  if (factoryPin != nullptr) {
    std::strncpy(settings.pin, factoryPin, PIN_MAX_DIGITS);
  }
  return settings;
}

bool isValidSettings(const Settings& settings) {
  return isValidLimits(settings.limits) && settings.dmxAddress >= DMX_ADDRESS_MIN &&
         settings.dmxAddress <= DMX_ADDRESS_MAX && isValidPin(settings.pin);
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
  out[OFFSET_ADDRESS] = static_cast<uint8_t>(settings.dmxAddress & 0xFF);
  out[OFFSET_ADDRESS + 1] = static_cast<uint8_t>(settings.dmxAddress >> 8);
  std::strncpy(reinterpret_cast<char*>(out + OFFSET_PIN), settings.pin, PIN_MAX_DIGITS);
  writeU32(out + OFFSET_CRC, crc32(out, OFFSET_CRC));
  return SETTINGS_BLOB_SIZE;
}

bool decodeSettings(const uint8_t* in, size_t size, Settings* out) {
  if (in == nullptr || out == nullptr || size != SETTINGS_BLOB_SIZE) {
    return false;
  }
  if (in[OFFSET_VERSION] != SETTINGS_VERSION || readU32(in + OFFSET_CRC) != crc32(in, OFFSET_CRC)) {
    return false;
  }
  const uint8_t flags = in[OFFSET_FLAGS];
  if ((flags & ~KNOWN_FLAGS) != 0 || in[OFFSET_PIN + PIN_MAX_DIGITS] != '\0') {
    return false;
  }
  Settings decoded;
  decoded.limits.ch7Low = in[OFFSET_CH7_LOW];
  decoded.limits.ch7High = in[OFFSET_CH7_HIGH];
  decoded.limits.ch6Enabled = (flags & FLAG_CH6_ENABLED) != 0;
  decoded.limits.isCalibrated = (flags & FLAG_CALIBRATED) != 0;
  decoded.limits.ch6Left = in[OFFSET_CH6_LEFT];
  decoded.limits.ch6Right = in[OFFSET_CH6_RIGHT];
  decoded.dmxAddress = static_cast<uint16_t>(in[OFFSET_ADDRESS] | in[OFFSET_ADDRESS + 1] << 8);
  std::memcpy(decoded.pin, in + OFFSET_PIN, PIN_MAX_DIGITS + 1);
  if (!isValidSettings(decoded)) {
    return false;
  }
  *out = decoded;
  return true;
}

}  // namespace laser
