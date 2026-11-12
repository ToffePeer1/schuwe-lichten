#include "fixtures.h"

#include "laser_core.h"
#include "moving_head.h"

namespace laser {

uint8_t channelCount(FixtureType type) {
  switch (type) {
    case FixtureType::Laser: return static_cast<uint8_t>(CHANNEL_COUNT);
    case FixtureType::Head: return HEAD_CHANNEL_COUNT;
    case FixtureType::None: return 0;
  }
  return 0;
}

const char* fixtureTypeName(FixtureType type) {
  switch (type) {
    case FixtureType::Laser: return "laser";
    case FixtureType::Head: return "head";
    case FixtureType::None: return "none";
  }
  return "none";
}

bool operator==(const PatchSlot& a, const PatchSlot& b) { return a.type == b.type && a.address == b.address; }

bool operator==(const Patch& a, const Patch& b) {
  for (uint8_t i = 0; i < MAX_FIXTURES; ++i) {
    if (!(a.slots[i] == b.slots[i])) {
      return false;
    }
  }
  return true;
}

Patch laserOnlyPatch(uint16_t address) {
  Patch patch;
  patch.slots[0] = PatchSlot{FixtureType::Laser, address};
  return patch;
}

uint16_t lastAddress(const PatchSlot& slot) {
  return static_cast<uint16_t>(slot.address + channelCount(slot.type) - 1);
}

PatchStatus checkPatch(const Patch& patch) {
  uint8_t lasers = 0;
  for (uint8_t i = 0; i < MAX_FIXTURES; ++i) {
    const PatchSlot& a = patch.slots[i];
    if (a.type == FixtureType::None) {
      continue;
    }
    if (a.address < 1 || lastAddress(a) > DMX_UNIVERSE_SIZE) {
      return PatchStatus::BadAddress;
    }
    lasers += a.type == FixtureType::Laser ? 1 : 0;
    for (uint8_t j = 0; j < i; ++j) {
      const PatchSlot& b = patch.slots[j];
      if (b.type != FixtureType::None && a.address <= lastAddress(b) && b.address <= lastAddress(a)) {
        return PatchStatus::Overlap;
      }
    }
  }
  return lasers > 1 ? PatchStatus::TwoLasers : PatchStatus::Ok;
}

uint8_t laserIndex(const Patch& patch) {
  for (uint8_t i = 0; i < MAX_FIXTURES; ++i) {
    if (patch.slots[i].type == FixtureType::Laser) {
      return i;
    }
  }
  return NO_FIXTURE;
}

}  // namespace laser
