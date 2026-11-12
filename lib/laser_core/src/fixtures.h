#pragma once

// The lights on the DMX line: fixture types and the patch, which says which
// type sits at which DMX address. The order on the cable doesn't matter in DMX,
// only the addresses, so any set of fixtures in any order is just a patch.

#include <array>
#include <cstddef>
#include <cstdint>

namespace laser {

enum class FixtureType : uint8_t { None, Laser, Head };

constexpr uint8_t MAX_FIXTURES = 4;           // patch slots 1..4
constexpr uint8_t MAX_FIXTURE_CHANNELS = 19;  // the moving head
constexpr uint16_t DMX_UNIVERSE_SIZE = 512;
constexpr uint8_t NO_FIXTURE = 0xFF;

using FixtureChannels = std::array<uint8_t, MAX_FIXTURE_CHANNELS>;  // index 0 = the fixture's CH1
using Universe = std::array<uint8_t, DMX_UNIVERSE_SIZE + 1>;        // [0] = start code, [n] = DMX slot n

uint8_t channelCount(FixtureType type);  // 0 for None
const char* fixtureTypeName(FixtureType type);

struct PatchSlot {
  FixtureType type = FixtureType::None;
  uint16_t address = 1;  // the fixture's CH1 on the DMX line
};

struct Patch {
  PatchSlot slots[MAX_FIXTURES];
};

bool operator==(const PatchSlot& a, const PatchSlot& b);
bool operator==(const Patch& a, const Patch& b);

enum class PatchStatus : uint8_t { Ok, BadAddress, Overlap, TwoLasers };

Patch laserOnlyPatch(uint16_t address);
// Every footprint inside 1..512, no two overlapping, and at most one laser
// (its height limit is a single setting). Empty slots are ignored.
PatchStatus checkPatch(const Patch& patch);
uint8_t laserIndex(const Patch& patch);  // 0-based slot index, or NO_FIXTURE
uint16_t lastAddress(const PatchSlot& slot);

}  // namespace laser
