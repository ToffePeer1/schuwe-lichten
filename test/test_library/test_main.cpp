// Unit tests for presets and shows: validation, edits, file format.

#include <unity.h>

#include <cstring>

#include "library.h"

using namespace laser;

namespace {

Channels look(uint8_t mode, uint8_t pattern, uint8_t ch7) {
  Channels c{};
  c[CH_MODE - 1] = mode;
  c[CH_PATTERN - 1] = pattern;
  c[CH_VERTICAL_POSITION - 1] = ch7;
  return c;
}

Command showCommand(uint16_t slot, const char* name, std::initializer_list<std::pair<uint16_t, uint16_t>> steps) {
  Command c;
  c.type = CommandType::ShowSave;
  c.slot = slot;
  std::strncpy(c.name, name, NAME_MAX_BYTES);
  for (const auto& step : steps) {
    c.stepPresets[c.stepCount] = step.first;
    c.stepHolds[c.stepCount] = step.second;
    ++c.stepCount;
  }
  return c;
}

Library withPresets() {
  Library lib;
  storePreset(lib, 1, "Blauwe tunnel", look(96, 37, 70));
  storePreset(lib, 2, "Langzame ster", look(96, 12, 60));
  return lib;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_names() {
  TEST_ASSERT_TRUE(isValidName("Blauwe tunnel"));
  TEST_ASSERT_TRUE(isValidName("Café ♫"));  // well-formed UTF-8
  TEST_ASSERT_TRUE(isValidName("12345678901234567890123456789012"));
  TEST_ASSERT_FALSE(isValidName("123456789012345678901234567890123"));
  TEST_ASSERT_FALSE(isValidName(""));
  TEST_ASSERT_FALSE(isValidName("quote\"d"));
  TEST_ASSERT_FALSE(isValidName("back\\slash"));
  TEST_ASSERT_FALSE(isValidName("tab\there"));
  TEST_ASSERT_FALSE(isValidName("\xC3"));          // truncated sequence
  TEST_ASSERT_FALSE(isValidName("\xC0\xAF"));      // overlong
  TEST_ASSERT_FALSE(isValidName("\xED\xA0\x80"));  // UTF-16 surrogate
  TEST_ASSERT_FALSE(isValidName(nullptr));
}

void test_slots() {
  TEST_ASSERT_TRUE(isValidPresetSlot(1));
  TEST_ASSERT_TRUE(isValidPresetSlot(32));
  TEST_ASSERT_FALSE(isValidPresetSlot(0));
  TEST_ASSERT_FALSE(isValidPresetSlot(33));
  TEST_ASSERT_TRUE(isValidShowSlot(16));
  TEST_ASSERT_FALSE(isValidShowSlot(17));
}

void test_sanitize_never_stores_auto_and_clamps_positions() {
  Limits limits;
  limits.ch7Low = 30;
  limits.ch7High = 100;
  limits.isCalibrated = true;
  const Channels clean = sanitizeLook(look(200, 5, 5), limits);
  TEST_ASSERT_EQUAL_UINT8(0, clean[CH_MODE - 1]);
  TEST_ASSERT_EQUAL_UINT8(30, clean[CH_VERTICAL_POSITION - 1]);
  TEST_ASSERT_EQUAL_UINT8(5, clean[CH_PATTERN - 1]);
}

void test_preset_save_checks() {
  Command c;
  c.slot = 0;
  std::strcpy(c.name, "ok");
  TEST_ASSERT_EQUAL(LibraryStatus::BadSlot, checkPresetSave(c));
  c.slot = 3;
  c.isNameTooLong = true;
  TEST_ASSERT_EQUAL(LibraryStatus::BadName, checkPresetSave(c));
  c.isNameTooLong = false;
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetSave(c));
}

void test_store_and_erase_preset() {
  Library lib = withPresets();
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetSlot(lib, 1));
  TEST_ASSERT_EQUAL(LibraryStatus::EmptySlot, checkPresetSlot(lib, 3));
  TEST_ASSERT_EQUAL_STRING("Blauwe tunnel", lib.presets[0].name);
  erasePreset(lib, 1);
  TEST_ASSERT_EQUAL(LibraryStatus::EmptySlot, checkPresetSlot(lib, 1));
}

void test_show_save_checks() {
  const Library lib = withPresets();
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkShowSave(lib, showCommand(1, "Opwarmen", {{1, 60}, {2, 30}})));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSlot, checkShowSave(lib, showCommand(17, "x", {{1, 60}})));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSteps, checkShowSave(lib, showCommand(1, "x", {{3, 60}})));  // empty preset
  TEST_ASSERT_EQUAL(LibraryStatus::BadSteps, checkShowSave(lib, showCommand(1, "x", {{1, 0}})));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSteps, checkShowSave(lib, showCommand(1, "x", {{1, 6001}})));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSteps, checkShowSave(lib, showCommand(1, "x", {})));
  Command malformed = showCommand(1, "x", {{1, 60}});
  malformed.areStepsMalformed = true;
  TEST_ASSERT_EQUAL(LibraryStatus::BadSteps, checkShowSave(lib, malformed));
  TEST_ASSERT_EQUAL(LibraryStatus::BadName, checkShowSave(lib, showCommand(1, "", {{1, 60}})));
}

void test_preset_in_use_cannot_be_deleted() {
  Library lib = withPresets();
  storeShow(lib, showCommand(4, "Feest", {{2, 40}}));
  TEST_ASSERT_TRUE(isPresetInUse(lib, 2));
  TEST_ASSERT_EQUAL(LibraryStatus::PresetInUse, checkPresetDelete(lib, 2));
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetDelete(lib, 1));
  eraseShow(lib, 4);
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetDelete(lib, 2));
}

void test_shows_with_missing_presets_are_dropped() {
  Library lib = withPresets();
  storeShow(lib, showCommand(1, "Goed", {{1, 60}, {2, 30}}));
  storeShow(lib, showCommand(2, "Half", {{2, 60}}));
  lib.presets[1] = Preset{};  // e.g. the presets file was reset
  TEST_ASSERT_EQUAL_UINT8(2, dropDanglingShows(lib));
  TEST_ASSERT_FALSE(lib.shows[0].isUsed);
  TEST_ASSERT_FALSE(lib.shows[1].isUsed);
  storeShow(lib, showCommand(3, "Heel", {{1, 60}}));
  TEST_ASSERT_EQUAL_UINT8(0, dropDanglingShows(lib));
  TEST_ASSERT_TRUE(lib.shows[2].isUsed);
}

void test_resolve_show_copies_every_look() {
  Library lib = withPresets();
  storeShow(lib, showCommand(2, "Opwarmen", {{1, 60}, {2, 30}, {1, 5}}));
  const ActiveShow active = resolveShow(lib, 2);
  TEST_ASSERT_EQUAL_UINT8(3, active.stepCount);
  TEST_ASSERT_EQUAL_UINT8(2, active.presetSlots[1]);
  TEST_ASSERT_EQUAL_UINT16(30, active.holdTenths[1]);
  TEST_ASSERT_EQUAL_UINT8(12, active.looks[1][CH_PATTERN - 1]);
  TEST_ASSERT_EQUAL_UINT8(0, resolveShow(lib, 3).stepCount);  // empty slot
}

void test_presets_round_trip_and_corruption() {
  const Library lib = withPresets();
  static uint8_t blob[2048];
  const size_t size = encodePresets(lib, blob, sizeof(blob));
  TEST_ASSERT_EQUAL(presetsBlobSize(), size);
  Library out;
  TEST_ASSERT_TRUE(decodePresets(blob, size, &out));
  TEST_ASSERT_TRUE(out.presets[1].isUsed);
  TEST_ASSERT_EQUAL_STRING("Langzame ster", out.presets[1].name);
  TEST_ASSERT_EQUAL_UINT8(60, out.presets[1].channels[CH_VERTICAL_POSITION - 1]);
  TEST_ASSERT_FALSE(out.presets[2].isUsed);
  blob[40] ^= 0x01;
  Library untouched = withPresets();
  TEST_ASSERT_FALSE(decodePresets(blob, size, &untouched));
  TEST_ASSERT_EQUAL_STRING("Blauwe tunnel", untouched.presets[0].name);
  TEST_ASSERT_FALSE(decodePresets(blob, size - 1, &untouched));
}

void test_shows_round_trip_and_corruption() {
  Library lib = withPresets();
  storeShow(lib, showCommand(16, "Laatste", {{1, 6000}, {2, 1}}));
  static uint8_t blob[4096];
  const size_t size = encodeShows(lib, blob, sizeof(blob));
  TEST_ASSERT_EQUAL(showsBlobSize(), size);
  Library out;
  TEST_ASSERT_TRUE(decodeShows(blob, size, &out));
  TEST_ASSERT_TRUE(out.shows[15].isUsed);
  TEST_ASSERT_EQUAL_UINT8(2, out.shows[15].stepCount);
  TEST_ASSERT_EQUAL_UINT16(6000, out.shows[15].steps[0].holdTenths);
  blob[size - 1] ^= 0x80;  // CRC
  TEST_ASSERT_FALSE(decodeShows(blob, size, &out));
}

void test_encode_refuses_small_buffers() {
  const Library lib;
  uint8_t small[16];
  TEST_ASSERT_EQUAL(0, encodePresets(lib, small, sizeof(small)));
  TEST_ASSERT_EQUAL(0, encodeShows(lib, small, sizeof(small)));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_names);
  RUN_TEST(test_slots);
  RUN_TEST(test_sanitize_never_stores_auto_and_clamps_positions);
  RUN_TEST(test_preset_save_checks);
  RUN_TEST(test_store_and_erase_preset);
  RUN_TEST(test_show_save_checks);
  RUN_TEST(test_preset_in_use_cannot_be_deleted);
  RUN_TEST(test_shows_with_missing_presets_are_dropped);
  RUN_TEST(test_resolve_show_copies_every_look);
  RUN_TEST(test_presets_round_trip_and_corruption);
  RUN_TEST(test_shows_round_trip_and_corruption);
  RUN_TEST(test_encode_refuses_small_buffers);
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
