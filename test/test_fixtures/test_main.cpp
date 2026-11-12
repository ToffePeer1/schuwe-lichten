// Unit tests for the fixture types, the patch and the moving head layout.

#include <unity.h>

#include "fixtures.h"
#include "moving_head.h"

using namespace laser;

namespace {

Patch patchOf(PatchSlot a, PatchSlot b = {}, PatchSlot c = {}, PatchSlot d = {}) {
  Patch patch;
  patch.slots[0] = a;
  patch.slots[1] = b;
  patch.slots[2] = c;
  patch.slots[3] = d;
  return patch;
}

constexpr PatchSlot LASER_AT_1{FixtureType::Laser, 1};
constexpr PatchSlot HEAD_AT_11{FixtureType::Head, 11};

}  // namespace

void setUp() {}
void tearDown() {}

void test_types_know_their_size_and_name() {
  TEST_ASSERT_EQUAL_UINT8(0, channelCount(FixtureType::None));
  TEST_ASSERT_EQUAL_UINT8(10, channelCount(FixtureType::Laser));
  TEST_ASSERT_EQUAL_UINT8(HEAD_CHANNEL_COUNT, channelCount(FixtureType::Head));
  TEST_ASSERT_EQUAL_STRING("laser", fixtureTypeName(FixtureType::Laser));
  TEST_ASSERT_EQUAL_STRING("head", fixtureTypeName(FixtureType::Head));
  TEST_ASSERT_EQUAL_STRING("none", fixtureTypeName(FixtureType::None));
}

void test_default_patch_is_one_laser() {
  const Patch patch = laserOnlyPatch(1);
  TEST_ASSERT_EQUAL(FixtureType::Laser, patch.slots[0].type);
  TEST_ASSERT_EQUAL_UINT16(1, patch.slots[0].address);
  for (uint8_t i = 1; i < MAX_FIXTURES; ++i) {
    TEST_ASSERT_EQUAL(FixtureType::None, patch.slots[i].type);
  }
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patch));
  TEST_ASSERT_EQUAL_UINT8(0, laserIndex(patch));
}

void test_fixtures_in_any_slot_and_order() {
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf(LASER_AT_1, HEAD_AT_11)));
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf({FixtureType::Head, 1}, {}, {FixtureType::Laser, 20})));
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf(HEAD_AT_11)));  // the head on its own
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(Patch{}));              // nothing patched
  TEST_ASSERT_EQUAL_UINT8(2, laserIndex(patchOf({FixtureType::Head, 1}, {}, {FixtureType::Laser, 20})));
  TEST_ASSERT_EQUAL_UINT8(NO_FIXTURE, laserIndex(patchOf(HEAD_AT_11)));
}

void test_footprints_must_fit_the_universe() {
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf({FixtureType::Laser, 503})));   // 503-512
  TEST_ASSERT_EQUAL(PatchStatus::BadAddress, checkPatch(patchOf({FixtureType::Laser, 504})));
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf({FixtureType::Head, 494})));     // 494-512
  TEST_ASSERT_EQUAL(PatchStatus::BadAddress, checkPatch(patchOf({FixtureType::Head, 495})));
  TEST_ASSERT_EQUAL(PatchStatus::BadAddress, checkPatch(patchOf({FixtureType::Head, 0})));
  TEST_ASSERT_EQUAL_UINT16(29, lastAddress(HEAD_AT_11));
}

void test_footprints_must_not_overlap() {
  TEST_ASSERT_EQUAL(PatchStatus::Overlap, checkPatch(patchOf(LASER_AT_1, {FixtureType::Head, 10})));  // 10 is the laser's CH10
  TEST_ASSERT_EQUAL(PatchStatus::Overlap, checkPatch(patchOf(HEAD_AT_11, {}, {FixtureType::Laser, 29})));
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf(HEAD_AT_11, {}, {FixtureType::Laser, 30})));
  TEST_ASSERT_EQUAL(PatchStatus::Overlap, checkPatch(patchOf(HEAD_AT_11, HEAD_AT_11)));
}

void test_only_one_laser_because_the_limits_are_single() {
  TEST_ASSERT_EQUAL(PatchStatus::TwoLasers, checkPatch(patchOf(LASER_AT_1, {FixtureType::Laser, 100})));
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patchOf(HEAD_AT_11, {FixtureType::Head, 40})));  // two heads are fine
}

void test_empty_slots_are_ignored_whatever_their_address() {
  Patch patch = patchOf(LASER_AT_1);
  patch.slots[1].address = 5;  // inside the laser, but the slot is empty
  TEST_ASSERT_EQUAL(PatchStatus::Ok, checkPatch(patch));
}

void test_head_off_darkens_light_and_macro_but_keeps_position() {
  FixtureChannels lit{};
  for (uint8_t ch = 1; ch <= HEAD_CHANNEL_COUNT; ++ch) {
    lit[ch - 1] = static_cast<uint8_t>(100 + ch);
  }
  const FixtureChannels off = headOff(lit);
  for (uint8_t ch = HEAD_CH_PAN; ch <= HEAD_CH_MOTOR_3; ++ch) {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(lit[ch - 1], off[ch - 1], "positions and motors stay");
  }
  for (uint8_t ch = HEAD_CH_DIMMER; ch <= HEAD_CH_MACRO; ++ch) {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, off[ch - 1], "light channels and macro go to 0");
  }
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_types_know_their_size_and_name);
  RUN_TEST(test_default_patch_is_one_laser);
  RUN_TEST(test_fixtures_in_any_slot_and_order);
  RUN_TEST(test_footprints_must_fit_the_universe);
  RUN_TEST(test_footprints_must_not_overlap);
  RUN_TEST(test_only_one_laser_because_the_limits_are_single);
  RUN_TEST(test_empty_slots_are_ignored_whatever_their_address);
  RUN_TEST(test_head_off_darkens_light_and_macro_but_keeps_position);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
  delay(2000);
  runAllTests();
}
void loop() {}
#else
int main() { return runAllTests(); }
#endif
