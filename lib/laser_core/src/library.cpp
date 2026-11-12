#include "library.h"

#include <cstring>

#include "settings_blob.h"  // crc32

namespace laser {

namespace {

constexpr size_t PRESET_RECORD = 1 + (NAME_MAX_BYTES + 1) + CHANNEL_COUNT;
constexpr size_t STEP_RECORD = 3;  // preset slot, hold (2 bytes LE)
constexpr size_t SHOW_RECORD = 1 + (NAME_MAX_BYTES + 1) + 1 + SHOW_MAX_STEPS * STEP_RECORD;
constexpr size_t CRC_BYTES = 4;

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

void writeU32(uint8_t* p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// Number of continuation bytes for a UTF-8 lead byte, or -1 if invalid.
int utf8Continuations(uint8_t lead) {
  if (lead < 0x80) return 0;
  if (lead >= 0xC2 && lead <= 0xDF) return 1;
  if (lead >= 0xE0 && lead <= 0xEF) return 2;
  if (lead >= 0xF0 && lead <= 0xF4) return 3;
  return -1;
}

// Allowed range of the byte after a lead byte (rejects overlong forms,
// UTF-16 surrogates and code points above U+10FFFF).
bool isValidSecondByte(uint8_t lead, uint8_t second) {
  switch (lead) {
    case 0xE0: return second >= 0xA0 && second <= 0xBF;
    case 0xED: return second >= 0x80 && second <= 0x9F;
    case 0xF0: return second >= 0x90 && second <= 0xBF;
    case 0xF4: return second >= 0x80 && second <= 0x8F;
    default: return (second & 0xC0) == 0x80;
  }
}

// Browsers drop a WebSocket that receives invalid UTF-8, so names must be well formed.
bool isWellFormedUtf8(const uint8_t* s, size_t length) {
  for (size_t i = 0; i < length;) {
    const int extra = utf8Continuations(s[i]);
    if (extra < 0 || length - i <= static_cast<size_t>(extra)) {
      return false;
    }
    if (extra > 0 && !isValidSecondByte(s[i], s[i + 1])) {
      return false;
    }
    for (int k = 2; k <= extra; ++k) {
      if ((s[i + static_cast<size_t>(k)] & 0xC0) != 0x80) {
        return false;
      }
    }
    i += static_cast<size_t>(extra) + 1;
  }
  return true;
}

void copyName(char* dest, const char* src) {
  std::memset(dest, 0, NAME_MAX_BYTES + 1);
  std::strncpy(dest, src, NAME_MAX_BYTES);
}

bool isNameRecordValid(const uint8_t* record) {
  return record[NAME_MAX_BYTES] == '\0' && isValidName(reinterpret_cast<const char*>(record));
}

}  // namespace

bool isValidName(const char* name) {
  if (name == nullptr) {
    return false;
  }
  const size_t length = strnlen(name, NAME_MAX_BYTES + 1);
  if (length == 0 || length > NAME_MAX_BYTES) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(name);
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] < 0x20 || bytes[i] == 0x7F || bytes[i] == '"' || bytes[i] == '\\') {
      return false;
    }
  }
  return isWellFormedUtf8(bytes, length);
}

bool isValidPresetSlot(uint16_t slot) { return slot >= 1 && slot <= PRESET_SLOTS; }
bool isValidShowSlot(uint16_t slot) { return slot >= 1 && slot <= SHOW_SLOTS; }

bool isPresetInUse(const Library& library, uint16_t slot) {
  for (const Show& show : library.shows) {
    if (!show.isUsed) {
      continue;
    }
    for (uint8_t i = 0; i < show.stepCount; ++i) {
      if (show.steps[i].preset == slot) {
        return true;
      }
    }
  }
  return false;
}

Channels sanitizeLook(const Channels& look, const Limits& limits) {
  Channels out = look;
  if (out[CH_MODE - 1] >= MODE_AUTO_MIN) {
    out[CH_MODE - 1] = 0;
  }
  out[CH_VERTICAL_POSITION - 1] = clampCh7(out[CH_VERTICAL_POSITION - 1], limits);
  out[CH_HORIZONTAL_POSITION - 1] = clampCh6(out[CH_HORIZONTAL_POSITION - 1], limits);
  return out;
}

LibraryStatus checkPresetSave(const Command& command) {
  if (!isValidPresetSlot(command.slot)) return LibraryStatus::BadSlot;
  if (command.isNameTooLong || !isValidName(command.name)) return LibraryStatus::BadName;
  return LibraryStatus::Ok;
}

LibraryStatus checkPresetSlot(const Library& library, uint16_t slot) {
  if (!isValidPresetSlot(slot)) return LibraryStatus::BadSlot;
  return library.presets[slot - 1].isUsed ? LibraryStatus::Ok : LibraryStatus::EmptySlot;
}

LibraryStatus checkPresetDelete(const Library& library, uint16_t slot) {
  const LibraryStatus exists = checkPresetSlot(library, slot);
  if (exists != LibraryStatus::Ok) return exists;
  return isPresetInUse(library, slot) ? LibraryStatus::PresetInUse : LibraryStatus::Ok;
}

LibraryStatus checkShowSave(const Library& library, const Command& command) {
  if (!isValidShowSlot(command.slot)) return LibraryStatus::BadSlot;
  if (command.isNameTooLong || !isValidName(command.name)) return LibraryStatus::BadName;
  if (command.areStepsMalformed || command.stepCount == 0 || command.stepCount > SHOW_MAX_STEPS) {
    return LibraryStatus::BadSteps;
  }
  for (uint8_t i = 0; i < command.stepCount; ++i) {
    const uint16_t preset = command.stepPresets[i];
    const uint16_t hold = command.stepHolds[i];
    if (!isValidPresetSlot(preset) || !library.presets[preset - 1].isUsed || hold < STEP_HOLD_MIN ||
        hold > STEP_HOLD_MAX) {
      return LibraryStatus::BadSteps;
    }
  }
  return LibraryStatus::Ok;
}

LibraryStatus checkShowSlot(const Library& library, uint16_t slot) {
  if (!isValidShowSlot(slot)) return LibraryStatus::BadSlot;
  return library.shows[slot - 1].isUsed ? LibraryStatus::Ok : LibraryStatus::EmptySlot;
}

void storePreset(Library& library, uint16_t slot, const char* name, const Channels& look) {
  Preset& preset = library.presets[slot - 1];
  preset.isUsed = true;
  copyName(preset.name, name);
  preset.channels = look;
}

void erasePreset(Library& library, uint16_t slot) { library.presets[slot - 1] = Preset{}; }

void storeShow(Library& library, const Command& command) {
  Show show;
  show.isUsed = true;
  copyName(show.name, command.name);
  show.stepCount = command.stepCount;
  for (uint8_t i = 0; i < command.stepCount; ++i) {
    show.steps[i].preset = static_cast<uint8_t>(command.stepPresets[i]);
    show.steps[i].holdTenths = command.stepHolds[i];
  }
  library.shows[command.slot - 1] = show;
}

void eraseShow(Library& library, uint16_t slot) { library.shows[slot - 1] = Show{}; }

ActiveShow resolveShow(const Library& library, uint16_t slot) {
  ActiveShow active;
  if (!isValidShowSlot(slot) || !library.shows[slot - 1].isUsed) {
    return active;
  }
  const Show& show = library.shows[slot - 1];
  for (uint8_t i = 0; i < show.stepCount; ++i) {
    const ShowStep& step = show.steps[i];
    if (!isValidPresetSlot(step.preset) || !library.presets[step.preset - 1].isUsed) {
      return ActiveShow{};  // a dangling step: refuse the whole show
    }
    active.presetSlots[i] = step.preset;
    active.holdTenths[i] = step.holdTenths;
    active.looks[i] = library.presets[step.preset - 1].channels;
  }
  active.stepCount = show.stepCount;
  return active;
}

uint8_t dropDanglingShows(Library& library) {
  uint8_t dropped = 0;
  for (uint8_t i = 0; i < SHOW_SLOTS; ++i) {
    if (library.shows[i].isUsed && resolveShow(library, i + 1).stepCount == 0) {
      library.shows[i] = Show{};
      ++dropped;
    }
  }
  return dropped;
}

size_t presetsBlobSize() { return 1 + PRESET_SLOTS * PRESET_RECORD + CRC_BYTES; }
size_t showsBlobSize() { return 1 + SHOW_SLOTS * SHOW_RECORD + CRC_BYTES; }

size_t encodePresets(const Library& library, uint8_t* out, size_t size) {
  const size_t total = presetsBlobSize();
  if (out == nullptr || size < total) {
    return 0;
  }
  std::memset(out, 0, total);
  out[0] = LIBRARY_VERSION;
  for (size_t i = 0; i < PRESET_SLOTS; ++i) {
    uint8_t* r = out + 1 + i * PRESET_RECORD;
    const Preset& p = library.presets[i];
    if (!p.isUsed) {
      continue;
    }
    r[0] = 1;
    std::memcpy(r + 1, p.name, NAME_MAX_BYTES + 1);
    std::memcpy(r + 1 + NAME_MAX_BYTES + 1, p.channels.data(), CHANNEL_COUNT);
  }
  writeU32(out + total - CRC_BYTES, crc32(out, total - CRC_BYTES));
  return total;
}

size_t encodeShows(const Library& library, uint8_t* out, size_t size) {
  const size_t total = showsBlobSize();
  if (out == nullptr || size < total) {
    return 0;
  }
  std::memset(out, 0, total);
  out[0] = LIBRARY_VERSION;
  for (size_t i = 0; i < SHOW_SLOTS; ++i) {
    uint8_t* r = out + 1 + i * SHOW_RECORD;
    const Show& s = library.shows[i];
    if (!s.isUsed) {
      continue;
    }
    r[0] = 1;
    std::memcpy(r + 1, s.name, NAME_MAX_BYTES + 1);
    r[1 + NAME_MAX_BYTES + 1] = s.stepCount;
    uint8_t* steps = r + 1 + NAME_MAX_BYTES + 1 + 1;
    for (uint8_t k = 0; k < s.stepCount; ++k) {
      steps[k * STEP_RECORD] = s.steps[k].preset;
      steps[k * STEP_RECORD + 1] = static_cast<uint8_t>(s.steps[k].holdTenths & 0xFF);
      steps[k * STEP_RECORD + 2] = static_cast<uint8_t>(s.steps[k].holdTenths >> 8);
    }
  }
  writeU32(out + total - CRC_BYTES, crc32(out, total - CRC_BYTES));
  return total;
}

bool decodePresets(const uint8_t* in, size_t size, Library* out) {
  const size_t total = presetsBlobSize();
  if (in == nullptr || out == nullptr || size != total || in[0] != LIBRARY_VERSION ||
      readU32(in + total - CRC_BYTES) != crc32(in, total - CRC_BYTES)) {
    return false;
  }
  Preset decoded[PRESET_SLOTS];
  for (size_t i = 0; i < PRESET_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * PRESET_RECORD;
    if (r[0] == 0) {
      continue;
    }
    if (r[0] != 1 || !isNameRecordValid(r + 1)) {
      return false;
    }
    decoded[i].isUsed = true;
    std::memcpy(decoded[i].name, r + 1, NAME_MAX_BYTES + 1);
    std::memcpy(decoded[i].channels.data(), r + 1 + NAME_MAX_BYTES + 1, CHANNEL_COUNT);
    if (decoded[i].channels[CH_MODE - 1] >= MODE_AUTO_MIN) {
      return false;  // never stored by this firmware
    }
  }
  std::memcpy(out->presets, decoded, sizeof(decoded));
  return true;
}

bool decodeShows(const uint8_t* in, size_t size, Library* out) {
  const size_t total = showsBlobSize();
  if (in == nullptr || out == nullptr || size != total || in[0] != LIBRARY_VERSION ||
      readU32(in + total - CRC_BYTES) != crc32(in, total - CRC_BYTES)) {
    return false;
  }
  Show decoded[SHOW_SLOTS];
  for (size_t i = 0; i < SHOW_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * SHOW_RECORD;
    if (r[0] == 0) {
      continue;
    }
    const uint8_t count = r[1 + NAME_MAX_BYTES + 1];
    if (r[0] != 1 || !isNameRecordValid(r + 1) || count == 0 || count > SHOW_MAX_STEPS) {
      return false;
    }
    decoded[i].isUsed = true;
    std::memcpy(decoded[i].name, r + 1, NAME_MAX_BYTES + 1);
    decoded[i].stepCount = count;
    const uint8_t* steps = r + 1 + NAME_MAX_BYTES + 1 + 1;
    for (uint8_t k = 0; k < count; ++k) {
      const uint8_t preset = steps[k * STEP_RECORD];
      const uint16_t hold =
          static_cast<uint16_t>(steps[k * STEP_RECORD + 1] | steps[k * STEP_RECORD + 2] << 8);
      if (!isValidPresetSlot(preset) || hold < STEP_HOLD_MIN || hold > STEP_HOLD_MAX) {
        return false;
      }
      decoded[i].steps[k].preset = preset;
      decoded[i].steps[k].holdTenths = hold;
    }
  }
  std::memcpy(out->shows, decoded, sizeof(decoded));
  return true;
}

}  // namespace laser
