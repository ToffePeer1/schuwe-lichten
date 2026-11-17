#include "library.h"

#include <cstdio>
#include <cstring>

#include "settings_blob.h"  // crc32

namespace laser {

namespace {

constexpr size_t CRC_BYTES = 4;
constexpr size_t NAME_BYTES = NAME_MAX_BYTES + 1;

// Version 2 records.
constexpr size_t PRESET_RECORD = 1 + 1 + NAME_BYTES + MAX_FIXTURE_CHANNELS;  // used, type, name, channels
constexpr size_t STEP_RECORD = MAX_FIXTURES + 1 + 2;                         // cues, nested show, hold (LE)
constexpr size_t SHOW_RECORD = 1 + NAME_BYTES + 1 + SHOW_MAX_STEPS * STEP_RECORD;

// Version 1: 32 laser presets of 10 channels; one preset and a hold per step.
constexpr uint8_t V1_VERSION = 1;
constexpr size_t V1_PRESET_SLOTS = 32;
constexpr size_t V1_PRESET_RECORD = 1 + NAME_BYTES + CHANNEL_COUNT;
constexpr size_t V1_STEP_RECORD = 3;
constexpr size_t V1_SHOW_RECORD = 1 + NAME_BYTES + 1 + SHOW_MAX_STEPS * V1_STEP_RECORD;

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
  std::memset(dest, 0, NAME_BYTES);
  std::strncpy(dest, src, NAME_MAX_BYTES);
}

bool isNameRecordValid(const uint8_t* record) {
  return record[NAME_MAX_BYTES] == '\0' && isValidName(reinterpret_cast<const char*>(record));
}

bool isStoredType(uint8_t type) {
  return type == static_cast<uint8_t>(FixtureType::Laser) || type == static_cast<uint8_t>(FixtureType::Head);
}

// A nested step has no hold of its own; every other step holds 0.1 s .. 10 min.
bool isValidTiming(uint16_t subShow, uint16_t holdTenths) {
  return subShow != 0 ? holdTenths == 0 : holdTenths >= STEP_HOLD_MIN && holdTenths <= STEP_HOLD_MAX;
}

// --- unfolding a show ----------------------------------------------------------

// Walks a show and its nested shows into a flat list of steps. `replacement`
// stands in for the show in `replacedSlot`, so a show can be checked before it
// is saved without copying the library.
struct Unfolder {
  const Library& library;
  uint16_t replacedSlot;
  const Show* replacement;
  ActiveShow* out;
  uint8_t pending[MAX_FIXTURES] = {NO_LOOK, NO_LOOK, NO_LOOK, NO_LOOK};  // cues for the next flat step
  LibraryStatus status = LibraryStatus::Ok;
};

const Show* showAt(const Unfolder& u, uint16_t slot) {
  if (!isValidShowSlot(slot)) {
    return nullptr;
  }
  if (slot == u.replacedSlot) {
    return u.replacement;
  }
  const Show& show = u.library.shows[slot - 1];
  return show.isUsed ? &show : nullptr;
}

bool fail(Unfolder& u, LibraryStatus status) {
  u.status = status;
  return false;
}

// The look index for a preset, copying it in the first time.
bool addLook(Unfolder& u, uint16_t presetSlot, uint8_t* index) {
  if (!isValidPresetSlot(presetSlot) || !u.library.presets[presetSlot - 1].isUsed) {
    return fail(u, LibraryStatus::BadSteps);
  }
  ActiveShow& out = *u.out;
  for (uint8_t i = 0; i < out.lookCount; ++i) {
    if (out.looks[i].presetSlot == presetSlot) {
      *index = i;
      return true;
    }
  }
  if (out.lookCount == SHOW_MAX_LOOKS) {
    return fail(u, LibraryStatus::ShowTooLong);
  }
  const Preset& preset = u.library.presets[presetSlot - 1];
  out.looks[out.lookCount] = ShowLook{preset.type, static_cast<uint8_t>(presetSlot), preset.channels};
  out.presetMask |= uint64_t{1} << (presetSlot - 1);
  *index = out.lookCount++;
  return true;
}

// A step's cues wait in `pending` for the next flat step, so a nested step's
// cues land on the nested show's first step (whose own cues win per fixture).
bool unfoldShow(Unfolder& u, uint16_t slot, uint8_t depth, uint16_t path) {
  const Show* show = showAt(u, slot);
  if (show == nullptr) {
    return fail(u, LibraryStatus::BadSteps);
  }
  const uint16_t bit = static_cast<uint16_t>(1u << (slot - 1));
  if ((path & bit) != 0) {
    return fail(u, LibraryStatus::ShowLoop);
  }
  if (depth > SHOW_MAX_DEPTH) {
    return fail(u, LibraryStatus::ShowTooLong);
  }
  u.out->showMask |= bit;
  for (uint8_t s = 0; s < show->stepCount; ++s) {
    const ShowStep& step = show->steps[s];
    for (uint8_t k = 0; k < MAX_FIXTURES; ++k) {
      if (step.cues[k] != 0 && !addLook(u, step.cues[k], &u.pending[k])) {
        return false;
      }
    }
    if (step.subShow != 0) {
      if (!unfoldShow(u, step.subShow, static_cast<uint8_t>(depth + 1), path | bit)) {
        return false;
      }
      continue;
    }
    ActiveShow& out = *u.out;
    if (out.stepCount == SHOW_MAX_FLAT_STEPS) {
      return fail(u, LibraryStatus::ShowTooLong);
    }
    FlatStep& flat = out.steps[out.stepCount++];
    std::memcpy(flat.looks, u.pending, sizeof(flat.looks));
    flat.holdTenths = step.holdTenths;
    std::memset(u.pending, NO_LOOK, sizeof(u.pending));
  }
  return true;
}

LibraryStatus unfold(const Library& library, uint16_t slot, uint16_t replacedSlot, const Show* replacement,
                     ActiveShow* out) {
  *out = ActiveShow{};
  Unfolder u{library, replacedSlot, replacement, out};
  if (!unfoldShow(u, slot, 1, 0)) {
    *out = ActiveShow{};
    return u.status;
  }
  return LibraryStatus::Ok;
}

Show showFromCommand(const Command& command) {
  Show show;
  show.isUsed = true;
  copyName(show.name, command.name);
  show.stepCount = command.stepCount;
  for (uint8_t i = 0; i < command.stepCount; ++i) {
    const StepSpec& spec = command.steps[i];
    ShowStep& step = show.steps[i];
    for (uint8_t k = 0; k < MAX_FIXTURES; ++k) {
      step.cues[k] = static_cast<uint8_t>(spec.cues[k]);
    }
    step.subShow = static_cast<uint8_t>(spec.subShow);
    step.holdTenths = spec.hasHold ? spec.holdTenths : 0;
  }
  return show;
}

// Each step on its own: timing, cue presets exist, the nested show exists.
LibraryStatus checkSteps(const Library& library, const Command& command) {
  for (uint8_t i = 0; i < command.stepCount; ++i) {
    const StepSpec& spec = command.steps[i];
    if (spec.hasHold == (spec.subShow != 0) || !isValidTiming(spec.subShow, spec.hasHold ? spec.holdTenths : 0)) {
      return LibraryStatus::BadSteps;
    }
    for (const uint16_t cue : spec.cues) {
      if (cue != 0 && (!isValidPresetSlot(cue) || !library.presets[cue - 1].isUsed)) {
        return LibraryStatus::BadSteps;
      }
    }
    const bool isNestedMissing = spec.subShow != 0 && spec.subShow != command.slot &&
                                 (!isValidShowSlot(spec.subShow) || !library.shows[spec.subShow - 1].isUsed);
    if (isNestedMissing) {
      return LibraryStatus::BadSteps;
    }
  }
  return LibraryStatus::Ok;
}

// --- file records ---------------------------------------------------------------

void encodeStep(const ShowStep& step, uint8_t* r) {
  std::memcpy(r, step.cues, MAX_FIXTURES);
  r[MAX_FIXTURES] = step.subShow;
  r[MAX_FIXTURES + 1] = static_cast<uint8_t>(step.holdTenths & 0xFF);
  r[MAX_FIXTURES + 2] = static_cast<uint8_t>(step.holdTenths >> 8);
}

bool decodeStep(const uint8_t* r, ShowStep* out) {
  for (uint8_t k = 0; k < MAX_FIXTURES; ++k) {
    if (r[k] > PRESET_SLOTS) {
      return false;
    }
  }
  const uint16_t hold = readU16(r + MAX_FIXTURES + 1);
  if (r[MAX_FIXTURES] > SHOW_SLOTS || !isValidTiming(r[MAX_FIXTURES], hold)) {
    return false;
  }
  std::memcpy(out->cues, r, MAX_FIXTURES);
  out->subShow = r[MAX_FIXTURES];
  out->holdTenths = hold;
  return true;
}

bool hasValidFrame(const uint8_t* in, size_t size, size_t expected, uint8_t version) {
  return size == expected && in[0] == version && readU32(in + size - CRC_BYTES) == crc32(in, size - CRC_BYTES);
}

bool decodePresetsV2(const uint8_t* in, Preset* decoded) {
  for (size_t i = 0; i < PRESET_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * PRESET_RECORD;
    if (r[0] == 0) {
      continue;
    }
    if (r[0] != 1 || !isStoredType(r[1]) || !isNameRecordValid(r + 2)) {
      return false;
    }
    Preset& p = decoded[i];
    p.isUsed = true;
    p.type = static_cast<FixtureType>(r[1]);
    std::memcpy(p.name, r + 2, NAME_BYTES);
    std::memcpy(p.channels.data(), r + 2 + NAME_BYTES, MAX_FIXTURE_CHANNELS);
    if (p.type == FixtureType::Laser && p.channels[CH_MODE - 1] >= MODE_AUTO_MIN) {
      return false;  // never stored by this firmware
    }
  }
  return true;
}

bool decodePresetsV1(const uint8_t* in, Preset* decoded) {
  for (size_t i = 0; i < V1_PRESET_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * V1_PRESET_RECORD;
    if (r[0] == 0) {
      continue;
    }
    if (r[0] != 1 || !isNameRecordValid(r + 1) || r[1 + NAME_BYTES + CH_MODE - 1] >= MODE_AUTO_MIN) {
      return false;
    }
    Preset& p = decoded[i];
    p.isUsed = true;
    p.type = FixtureType::Laser;  // version 1 knew only the laser
    std::memcpy(p.name, r + 1, NAME_BYTES);
    std::memcpy(p.channels.data(), r + 1 + NAME_BYTES, CHANNEL_COUNT);
  }
  return true;
}

bool decodeShowsV2(const uint8_t* in, Show* decoded) {
  for (size_t i = 0; i < SHOW_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * SHOW_RECORD;
    if (r[0] == 0) {
      continue;
    }
    const uint8_t count = r[1 + NAME_BYTES];
    if (r[0] != 1 || !isNameRecordValid(r + 1) || count == 0 || count > SHOW_MAX_STEPS) {
      return false;
    }
    Show& show = decoded[i];
    show.isUsed = true;
    std::memcpy(show.name, r + 1, NAME_BYTES);
    show.stepCount = count;
    for (uint8_t k = 0; k < count; ++k) {
      if (!decodeStep(r + 1 + NAME_BYTES + 1 + k * STEP_RECORD, &show.steps[k])) {
        return false;
      }
    }
  }
  return true;
}

// A version 1 step showed one laser preset: it becomes a cue on slot 1, where
// the settings migration also puts the laser.
bool decodeShowsV1(const uint8_t* in, Show* decoded) {
  for (size_t i = 0; i < SHOW_SLOTS; ++i) {
    const uint8_t* r = in + 1 + i * V1_SHOW_RECORD;
    if (r[0] == 0) {
      continue;
    }
    const uint8_t count = r[1 + NAME_BYTES];
    if (r[0] != 1 || !isNameRecordValid(r + 1) || count == 0 || count > SHOW_MAX_STEPS) {
      return false;
    }
    Show& show = decoded[i];
    show.isUsed = true;
    std::memcpy(show.name, r + 1, NAME_BYTES);
    show.stepCount = count;
    for (uint8_t k = 0; k < count; ++k) {
      const uint8_t* step = r + 1 + NAME_BYTES + 1 + k * V1_STEP_RECORD;
      const uint16_t hold = readU16(step + 1);
      if (step[0] < 1 || step[0] > V1_PRESET_SLOTS || hold < STEP_HOLD_MIN || hold > STEP_HOLD_MAX) {
        return false;
      }
      show.steps[k].cues[0] = step[0];
      show.steps[k].holdTenths = hold;
    }
  }
  return true;
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
    for (uint8_t i = 0; show.isUsed && i < show.stepCount; ++i) {
      for (const uint8_t cue : show.steps[i].cues) {
        if (cue == slot) {
          return true;
        }
      }
    }
  }
  return false;
}

bool isShowInUse(const Library& library, uint16_t slot) {
  for (const Show& show : library.shows) {
    for (uint8_t i = 0; show.isUsed && i < show.stepCount; ++i) {
      if (show.steps[i].subShow == slot) {
        return true;
      }
    }
  }
  return false;
}

FixtureChannels sanitizeLook(FixtureType type, const FixtureChannels& look, const Limits& limits) {
  if (type != FixtureType::Laser) {
    return look;  // the head has no limits (user decision)
  }
  Channels laser = toLaserChannels(look);
  if (laser[CH_MODE - 1] >= MODE_AUTO_MIN) {
    laser[CH_MODE - 1] = 0;
  }
  laser[CH_VERTICAL_POSITION - 1] = clampCh7(laser[CH_VERTICAL_POSITION - 1], limits);
  laser[CH_HORIZONTAL_POSITION - 1] = clampCh6(laser[CH_HORIZONTAL_POSITION - 1], limits);
  return toFixtureChannels(laser);
}

LibraryStatus checkPresetSave(const Library& library, const Command& command, FixtureType type) {
  if (!isValidPresetSlot(command.slot)) return LibraryStatus::BadSlot;
  if (command.isNameTooLong || !isValidName(command.name)) return LibraryStatus::BadName;
  const Preset& old = library.presets[command.slot - 1];
  const bool isRetyped = old.isUsed && old.type != type;
  return isRetyped && isPresetInUse(library, command.slot) ? LibraryStatus::PresetInUse : LibraryStatus::Ok;
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
  const LibraryStatus steps = checkSteps(library, command);
  if (steps != LibraryStatus::Ok) {
    return steps;
  }
  // The new show must unfold, and so must every show that nests it.
  const Show candidate = showFromCommand(command);
  ActiveShow scratch;
  const LibraryStatus own = unfold(library, command.slot, command.slot, &candidate, &scratch);
  if (own != LibraryStatus::Ok) {
    return own;
  }
  for (uint16_t slot = 1; slot <= SHOW_SLOTS; ++slot) {
    if (slot == command.slot || !library.shows[slot - 1].isUsed) {
      continue;
    }
    const LibraryStatus other = unfold(library, slot, command.slot, &candidate, &scratch);
    if (other != LibraryStatus::Ok) {
      return other;
    }
  }
  return LibraryStatus::Ok;
}

LibraryStatus checkShowSlot(const Library& library, uint16_t slot) {
  if (!isValidShowSlot(slot)) return LibraryStatus::BadSlot;
  return library.shows[slot - 1].isUsed ? LibraryStatus::Ok : LibraryStatus::EmptySlot;
}

LibraryStatus checkShowDelete(const Library& library, uint16_t slot) {
  const LibraryStatus exists = checkShowSlot(library, slot);
  if (exists != LibraryStatus::Ok) return exists;
  return isShowInUse(library, slot) ? LibraryStatus::ShowInUse : LibraryStatus::Ok;
}

void storePreset(Library& library, uint16_t slot, const char* name, FixtureType type, const FixtureChannels& look) {
  Preset& preset = library.presets[slot - 1];
  preset.isUsed = true;
  preset.type = type;
  copyName(preset.name, name);
  preset.channels = look;
}

void erasePreset(Library& library, uint16_t slot) { library.presets[slot - 1] = Preset{}; }

void storeShow(Library& library, const Command& command) { library.shows[command.slot - 1] = showFromCommand(command); }

void eraseShow(Library& library, uint16_t slot) { library.shows[slot - 1] = Show{}; }

ActiveShow resolveShow(const Library& library, uint16_t slot) {
  ActiveShow active;
  unfold(library, slot, 0, nullptr, &active);
  return active;
}

bool formatSteps(const Show& show, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return false;
  }
  size_t length = 0;
  bool isOk = true;
  // Appends "<prefix><value>"; after the first overflow nothing is written any more.
  const auto add = [&](const char* prefix, unsigned value) {
    if (!isOk) {
      return;
    }
    const int written = std::snprintf(out + length, size - length, "%s%u", prefix, value);
    isOk = written >= 0 && static_cast<size_t>(written) < size - length;
    length += isOk ? static_cast<size_t>(written) : 0;
  };
  for (uint8_t i = 0; i < show.stepCount; ++i) {
    const ShowStep& step = show.steps[i];
    const char* separator = i == 0 ? "" : ",";
    for (uint8_t k = 0; k < MAX_FIXTURES; ++k) {
      if (step.cues[k] != 0) {
        add(separator, k + 1u);
        add("=", step.cues[k]);
        separator = "+";
      }
    }
    if (step.subShow != 0) {
      add(*separator == '+' ? "+s" : (i == 0 ? "s" : ",s"), step.subShow);
    } else {
      add(*separator == '+' ? ":" : (i == 0 ? ":" : ",:"), step.holdTenths);
    }
  }
  if (!isOk) {
    out[0] = '\0';
  }
  return isOk;
}

uint8_t dropDanglingShows(Library& library) {
  uint8_t dropped = 0;
  bool isChanged = true;
  ActiveShow scratch;
  while (isChanged) {  // dropping a show can strand the shows that nest it
    isChanged = false;
    for (uint8_t i = 0; i < SHOW_SLOTS; ++i) {
      if (library.shows[i].isUsed && unfold(library, i + 1, 0, nullptr, &scratch) != LibraryStatus::Ok) {
        library.shows[i] = Show{};
        ++dropped;
        isChanged = true;
      }
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
    const Preset& p = library.presets[i];
    if (!p.isUsed) {
      continue;
    }
    uint8_t* r = out + 1 + i * PRESET_RECORD;
    r[0] = 1;
    r[1] = static_cast<uint8_t>(p.type);
    std::memcpy(r + 2, p.name, NAME_BYTES);
    std::memcpy(r + 2 + NAME_BYTES, p.channels.data(), MAX_FIXTURE_CHANNELS);
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
    const Show& s = library.shows[i];
    if (!s.isUsed) {
      continue;
    }
    uint8_t* r = out + 1 + i * SHOW_RECORD;
    r[0] = 1;
    std::memcpy(r + 1, s.name, NAME_BYTES);
    r[1 + NAME_BYTES] = s.stepCount;
    for (uint8_t k = 0; k < s.stepCount; ++k) {
      encodeStep(s.steps[k], r + 1 + NAME_BYTES + 1 + k * STEP_RECORD);
    }
  }
  writeU32(out + total - CRC_BYTES, crc32(out, total - CRC_BYTES));
  return total;
}

bool decodePresets(const uint8_t* in, size_t size, Library* out) {
  if (in == nullptr || out == nullptr) {
    return false;
  }
  Preset decoded[PRESET_SLOTS];
  const bool isDecoded =
      (hasValidFrame(in, size, presetsBlobSize(), LIBRARY_VERSION) && decodePresetsV2(in, decoded)) ||
      (hasValidFrame(in, size, 1 + V1_PRESET_SLOTS * V1_PRESET_RECORD + CRC_BYTES, V1_VERSION) &&
       decodePresetsV1(in, decoded));
  if (!isDecoded) {
    return false;
  }
  std::memcpy(out->presets, decoded, sizeof(decoded));
  return true;
}

bool decodeShows(const uint8_t* in, size_t size, Library* out) {
  if (in == nullptr || out == nullptr) {
    return false;
  }
  Show decoded[SHOW_SLOTS];
  const bool isDecoded =
      (hasValidFrame(in, size, showsBlobSize(), LIBRARY_VERSION) && decodeShowsV2(in, decoded)) ||
      (hasValidFrame(in, size, 1 + SHOW_SLOTS * V1_SHOW_RECORD + CRC_BYTES, V1_VERSION) && decodeShowsV1(in, decoded));
  if (!isDecoded) {
    return false;
  }
  std::memcpy(out->shows, decoded, sizeof(decoded));
  return true;
}

}  // namespace laser
