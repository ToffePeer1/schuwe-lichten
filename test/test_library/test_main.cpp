// Unit tests for presets and shows: validation, nested shows, unfolding, file format.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "library.h"
#include "moving_head.h"
#include "settings_blob.h"  // crc32

using namespace laser;

namespace {

FixtureChannels laserLook(uint8_t mode, uint8_t pattern, uint8_t ch7) {
  Channels c{};
  c[CH_MODE - 1] = mode;
  c[CH_PATTERN - 1] = pattern;
  c[CH_VERTICAL_POSITION - 1] = ch7;
  return toFixtureChannels(c);
}

FixtureChannels headLook(uint8_t dimmer, uint8_t pan) {
  FixtureChannels c{};
  c[HEAD_CH_DIMMER - 1] = dimmer;
  c[HEAD_CH_PAN - 1] = pan;
  return c;
}

// Presets 1-2 laser looks, 3-4 head looks.
Library withPresets() {
  Library lib;
  storePreset(lib, 1, "Blauwe tunnel", FixtureType::Laser, laserLook(96, 37, 70));
  storePreset(lib, 2, "Langzame ster", FixtureType::Laser, laserLook(96, 12, 60));
  storePreset(lib, 3, "Rood draaiend", FixtureType::Head, headLook(255, 10));
  storePreset(lib, 4, "Strobo", FixtureType::Head, headLook(200, 200));
  return lib;
}

LibraryStatus trySave(const Library& lib, const char* line) { return checkShowSave(lib, parseCommand(line)); }

void save(Library& lib, const char* line) {
  const Command command = parseCommand(line);
  TEST_ASSERT_EQUAL_MESSAGE(LibraryStatus::Ok, checkShowSave(lib, command), line);
  storeShow(lib, command);
}

void writeCrc(uint8_t* blob, size_t size) {
  const uint32_t crc = crc32(blob, size - 4);
  for (int i = 0; i < 4; ++i) {
    blob[size - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
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
  TEST_ASSERT_TRUE(isValidPresetSlot(48));
  TEST_ASSERT_FALSE(isValidPresetSlot(0));
  TEST_ASSERT_FALSE(isValidPresetSlot(49));
  TEST_ASSERT_TRUE(isValidShowSlot(16));
  TEST_ASSERT_FALSE(isValidShowSlot(17));
}

void test_laser_looks_are_sanitised_and_head_looks_kept() {
  Limits limits;
  limits.ch7Low = 30;
  limits.ch7High = 100;
  const FixtureChannels laser = sanitizeLook(FixtureType::Laser, laserLook(200, 5, 5), limits);
  TEST_ASSERT_EQUAL_UINT8(0, laser[CH_MODE - 1]);                 // never auto/sound
  TEST_ASSERT_EQUAL_UINT8(30, laser[CH_VERTICAL_POSITION - 1]);   // inside the height limit
  FixtureChannels head = headLook(255, 250);
  head[HEAD_CH_MACRO - 1] = 120;
  TEST_ASSERT_TRUE(head == sanitizeLook(FixtureType::Head, head, limits));  // no limits on the head
}

void test_preset_save_checks() {
  Command c = parseCommand("preset save 48 Rood");
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetSave(c));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSlot, checkPresetSave(parseCommand("preset save 49 Rood")));
  TEST_ASSERT_EQUAL(LibraryStatus::BadName, checkPresetSave(parseCommand("preset save 1 quote\"d")));
  TEST_ASSERT_EQUAL(LibraryStatus::BadName,
                    checkPresetSave(parseCommand("preset save 1 123456789012345678901234567890123")));
}

void test_store_and_erase_typed_presets() {
  Library lib = withPresets();
  TEST_ASSERT_EQUAL(FixtureType::Head, lib.presets[2].type);
  TEST_ASSERT_EQUAL_UINT8(255, lib.presets[2].channels[HEAD_CH_DIMMER - 1]);
  erasePreset(lib, 3);
  TEST_ASSERT_FALSE(lib.presets[2].isUsed);
  TEST_ASSERT_EQUAL(LibraryStatus::EmptySlot, checkPresetSlot(lib, 3));
}

void test_show_step_checks() {
  const Library lib = withPresets();
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, trySave(lib, "show save 1 1=1+2=3:60,2=4:30,:10 Opwarmen"));
  TEST_ASSERT_EQUAL(LibraryStatus::BadSlot, trySave(lib, "show save 17 1=1:60 x"));
  TEST_ASSERT_EQUAL(LibraryStatus::BadName, trySave(lib, "show save 1 1=1:60 quote\"d"));
  const char* badSteps[] = {
      "show save 1 1=9:60 x",     // empty preset slot
      "show save 1 1=49:60 x",    // no such slot
      "show save 1 1=1:0 x",      // too short
      "show save 1 1=1:6001 x",   // too long
      "show save 1 1=1 x",        // no hold and no nested show
      "show save 1 s2 x",         // nested show doesn't exist
      "show save 1 1=1+s2:50 x",  // malformed: a nested step has no hold of its own
  };
  for (const char* line : badSteps) {
    TEST_ASSERT_EQUAL_MESSAGE(LibraryStatus::BadSteps, trySave(lib, line), line);
  }
}

void test_presets_and_shows_in_use_cannot_be_deleted() {
  Library lib = withPresets();
  save(lib, "show save 2 2=3:20 Kop");
  save(lib, "show save 4 1=2+s2 Feest");
  TEST_ASSERT_TRUE(isPresetInUse(lib, 3));  // a cue on any fixture counts
  TEST_ASSERT_TRUE(isPresetInUse(lib, 2));
  TEST_ASSERT_EQUAL(LibraryStatus::PresetInUse, checkPresetDelete(lib, 3));
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkPresetDelete(lib, 1));
  TEST_ASSERT_EQUAL(LibraryStatus::ShowInUse, checkShowDelete(lib, 2));  // nested in show 4
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkShowDelete(lib, 4));
  eraseShow(lib, 4);
  TEST_ASSERT_EQUAL(LibraryStatus::Ok, checkShowDelete(lib, 2));
}

void test_nested_shows_must_not_loop() {
  Library lib = withPresets();
  TEST_ASSERT_EQUAL(LibraryStatus::ShowLoop, trySave(lib, "show save 1 1=1:10,s1 Ik"));
  save(lib, "show save 1 1=1:10 A");
  save(lib, "show save 2 s1 B");                                          // B plays A
  TEST_ASSERT_EQUAL(LibraryStatus::ShowLoop, trySave(lib, "show save 1 s2 A"));  // A would play B plays A
}

void test_nesting_depth_is_limited() {
  Library lib = withPresets();
  save(lib, "show save 1 1=1:10 D1");
  save(lib, "show save 2 s1 D2");
  save(lib, "show save 3 s2 D3");                                          // three deep: fine
  TEST_ASSERT_EQUAL(LibraryStatus::ShowTooLong, trySave(lib, "show save 4 s3 D4"));
}

void test_unfolded_length_is_limited() {
  Library lib = withPresets();
  char line[MAX_LINE_LENGTH + 1] = "show save 1 ";
  for (size_t i = 0; i < SHOW_MAX_STEPS; ++i) {
    std::strcat(line, i == 0 ? "1=1:10" : ",1=2:10");
  }
  std::strcat(line, " Lang");
  save(lib, line);                                                          // 24 steps
  save(lib, "show save 2 s1 Enkel");                                         // 24
  save(lib, "show save 3 s2,s2 Dubbel");                                     // 48: fine
  TEST_ASSERT_EQUAL(LibraryStatus::ShowTooLong, trySave(lib, "show save 4 s1,s1,s1 Drie"));  // 72 > 64
  // Show 2 on its own would be fine at 48, but show 3 plays it twice: 96.
  TEST_ASSERT_EQUAL(LibraryStatus::ShowTooLong, trySave(lib, "show save 2 s1,s1 Enkel"));
}

void test_number_of_different_looks_is_limited() {
  Library lib;
  char line[MAX_LINE_LENGTH + 1] = "show save 1 ";
  for (int i = 1; i <= SHOW_MAX_LOOKS + 1; ++i) {
    char name[8];
    std::snprintf(name, sizeof(name), "P%d", i);
    storePreset(lib, static_cast<uint16_t>(i), name, FixtureType::Head, headLook(static_cast<uint8_t>(i), 0));
  }
  for (int i = 1; i <= SHOW_MAX_LOOKS; ++i) {  // 24 steps, 24 different looks
    char step[12];
    std::snprintf(step, sizeof(step), i == 1 ? "1=%d:5" : ",1=%d:5", i);
    std::strcat(line, step);
  }
  std::strcat(line, " Kleuren");
  save(lib, line);
  save(lib, "show save 2 1=25:5 Nog een");
  TEST_ASSERT_EQUAL(LibraryStatus::ShowTooLong, trySave(lib, "show save 3 s1,s2 Te veel"));  // 25 looks
}

void test_unfolding_plays_nested_shows_out() {
  Library lib = withPresets();
  save(lib, "show save 2 2=3:30,2=4:50,2=3:30 Kop draait");   // head-only building block
  save(lib, "show save 1 1=1:20,1=2+s2,:10 Avond");           // laser 1, then laser 2 while the block plays, then a pause
  const ActiveShow show = resolveShow(lib, 1);
  TEST_ASSERT_EQUAL_UINT8(5, show.stepCount);  // 1 + 3 nested + the pause
  TEST_ASSERT_EQUAL_UINT8(4, show.lookCount);  // presets 1, 2, 3, 4 once each
  const ShowLook& first = show.looks[show.steps[0].looks[0]];
  TEST_ASSERT_EQUAL(FixtureType::Laser, first.type);
  TEST_ASSERT_EQUAL_UINT8(1, first.presetSlot);
  TEST_ASSERT_EQUAL_UINT8(NO_LOOK, show.steps[0].looks[1]);  // the head is left alone
  TEST_ASSERT_EQUAL_UINT16(20, show.steps[0].holdTenths);
  // The nested step's laser cue lands on the nested show's first step, next to its own head cue.
  TEST_ASSERT_EQUAL_UINT8(2, show.looks[show.steps[1].looks[0]].presetSlot);
  TEST_ASSERT_EQUAL_UINT8(3, show.looks[show.steps[1].looks[1]].presetSlot);
  TEST_ASSERT_EQUAL_UINT16(30, show.steps[1].holdTenths);
  TEST_ASSERT_EQUAL_UINT8(NO_LOOK, show.steps[2].looks[0]);  // the laser keeps preset 2
  TEST_ASSERT_EQUAL_UINT8(4, show.looks[show.steps[2].looks[1]].presetSlot);
  TEST_ASSERT_EQUAL_UINT8(3, show.looks[show.steps[3].looks[1]].presetSlot);  // the same look is reused
  TEST_ASSERT_EQUAL_UINT8(show.steps[1].looks[1], show.steps[3].looks[1]);
  for (uint8_t k = 0; k < MAX_FIXTURES; ++k) {
    TEST_ASSERT_EQUAL_UINT8(NO_LOOK, show.steps[4].looks[k]);  // the pause changes nothing
  }
  TEST_ASSERT_TRUE(show.presetMask == 0xF);      // presets 1-4
  TEST_ASSERT_EQUAL_UINT16(0x3, show.showMask);  // shows 1 and 2
  TEST_ASSERT_EQUAL_UINT8(0, resolveShow(lib, 3).stepCount);  // empty slot
}

void test_a_nested_shows_own_cue_wins() {
  Library lib = withPresets();
  save(lib, "show save 2 2=4:10 Strobo");
  save(lib, "show save 1 2=3+s2 Toch strobo");  // head preset 3, but the nested show starts with 4
  const ActiveShow show = resolveShow(lib, 1);
  TEST_ASSERT_EQUAL_UINT8(1, show.stepCount);
  TEST_ASSERT_EQUAL_UINT8(4, show.looks[show.steps[0].looks[1]].presetSlot);
}

void test_steps_print_in_the_same_text_they_were_typed() {
  Library lib = withPresets();
  save(lib, "show save 2 2=3:30 Blok");
  const char* steps = "1=1+2=3:60,2=4:30,s2,1=2+s2,:10";
  char line[MAX_LINE_LENGTH + 1];
  std::snprintf(line, sizeof(line), "show save 1 %s Rondje", steps);
  save(lib, line);
  char out[256];
  TEST_ASSERT_TRUE(formatSteps(lib.shows[0], out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(steps, out);
  char tiny[8];
  TEST_ASSERT_FALSE(formatSteps(lib.shows[0], tiny, sizeof(tiny)));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

void test_dangling_shows_are_dropped_with_their_parents() {
  Library lib = withPresets();
  save(lib, "show save 2 2=3:30 Blok");
  save(lib, "show save 1 1=1:10,s2 Ouder");
  save(lib, "show save 3 1=2:10 Los");
  lib.presets[2] = Preset{};  // e.g. the presets file was reset
  TEST_ASSERT_EQUAL_UINT8(2, dropDanglingShows(lib));
  TEST_ASSERT_FALSE(lib.shows[1].isUsed);
  TEST_ASSERT_FALSE(lib.shows[0].isUsed);
  TEST_ASSERT_TRUE(lib.shows[2].isUsed);
}

void test_presets_round_trip_and_corruption() {
  const Library lib = withPresets();
  std::vector<uint8_t> blob(presetsBlobSize());
  TEST_ASSERT_EQUAL(presetsBlobSize(), encodePresets(lib, blob.data(), blob.size()));
  Library out;
  TEST_ASSERT_TRUE(decodePresets(blob.data(), blob.size(), &out));
  TEST_ASSERT_EQUAL(FixtureType::Head, out.presets[3].type);
  TEST_ASSERT_EQUAL_STRING("Strobo", out.presets[3].name);
  TEST_ASSERT_EQUAL_UINT8(200, out.presets[3].channels[HEAD_CH_PAN - 1]);
  TEST_ASSERT_EQUAL_UINT8(37, out.presets[0].channels[CH_PATTERN - 1]);
  blob[20] ^= 0x01;
  Library untouched = withPresets();
  TEST_ASSERT_FALSE(decodePresets(blob.data(), blob.size(), &untouched));
  TEST_ASSERT_EQUAL_STRING("Blauwe tunnel", untouched.presets[0].name);
}

void test_shows_round_trip_and_corruption() {
  Library lib = withPresets();
  save(lib, "show save 2 2=3:30 Blok");
  save(lib, "show save 1 1=1+2=4:60,s2,:5 Rondje");
  std::vector<uint8_t> blob(showsBlobSize());
  TEST_ASSERT_EQUAL(showsBlobSize(), encodeShows(lib, blob.data(), blob.size()));
  Library out = withPresets();
  TEST_ASSERT_TRUE(decodeShows(blob.data(), blob.size(), &out));
  char steps[64];
  formatSteps(out.shows[0], steps, sizeof(steps));
  TEST_ASSERT_EQUAL_STRING("1=1+2=4:60,s2,:5", steps);
  blob[blob.size() - 1] ^= 0x01;
  TEST_ASSERT_FALSE(decodeShows(blob.data(), blob.size(), &out));
}

void test_version_1_files_still_load() {
  // Version 1: 32 laser presets (used, name, 10 channels), shows of (preset, hold) steps.
  constexpr size_t presetRecord = 1 + 33 + 10;
  std::vector<uint8_t> presets(1 + 32 * presetRecord + 4, 0);
  presets[0] = 1;
  uint8_t* p = &presets[1 + 4 * presetRecord];  // slot 5
  p[0] = 1;
  std::memcpy(p + 1, "Oud", 3);
  p[1 + 33 + CH_MODE - 1] = 96;
  p[1 + 33 + CH_PATTERN - 1] = 44;
  writeCrc(presets.data(), presets.size());
  Library lib;
  TEST_ASSERT_TRUE(decodePresets(presets.data(), presets.size(), &lib));
  TEST_ASSERT_EQUAL(FixtureType::Laser, lib.presets[4].type);
  TEST_ASSERT_EQUAL_STRING("Oud", lib.presets[4].name);
  TEST_ASSERT_EQUAL_UINT8(44, lib.presets[4].channels[CH_PATTERN - 1]);

  constexpr size_t showRecord = 1 + 33 + 1 + 24 * 3;
  std::vector<uint8_t> shows(1 + 16 * showRecord + 4, 0);
  shows[0] = 1;
  uint8_t* s = &shows[1];  // slot 1
  s[0] = 1;
  std::memcpy(s + 1, "Oude show", 9);
  s[1 + 33] = 1;          // one step
  s[1 + 33 + 1] = 5;      // preset 5
  s[1 + 33 + 2] = 40;     // 4.0 s
  writeCrc(shows.data(), shows.size());
  TEST_ASSERT_TRUE(decodeShows(shows.data(), shows.size(), &lib));
  char steps[32];
  formatSteps(lib.shows[0], steps, sizeof(steps));
  TEST_ASSERT_EQUAL_STRING("1=5:40", steps);  // the laser's look, as a cue on slot 1
}

void test_encode_refuses_small_buffers() {
  const Library lib = withPresets();
  uint8_t small[16];
  TEST_ASSERT_EQUAL(0, encodePresets(lib, small, sizeof(small)));
  TEST_ASSERT_EQUAL(0, encodeShows(lib, small, sizeof(small)));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_names);
  RUN_TEST(test_slots);
  RUN_TEST(test_laser_looks_are_sanitised_and_head_looks_kept);
  RUN_TEST(test_preset_save_checks);
  RUN_TEST(test_store_and_erase_typed_presets);
  RUN_TEST(test_show_step_checks);
  RUN_TEST(test_presets_and_shows_in_use_cannot_be_deleted);
  RUN_TEST(test_nested_shows_must_not_loop);
  RUN_TEST(test_nesting_depth_is_limited);
  RUN_TEST(test_unfolded_length_is_limited);
  RUN_TEST(test_number_of_different_looks_is_limited);
  RUN_TEST(test_unfolding_plays_nested_shows_out);
  RUN_TEST(test_a_nested_shows_own_cue_wins);
  RUN_TEST(test_steps_print_in_the_same_text_they_were_typed);
  RUN_TEST(test_dangling_shows_are_dropped_with_their_parents);
  RUN_TEST(test_presets_round_trip_and_corruption);
  RUN_TEST(test_shows_round_trip_and_corruption);
  RUN_TEST(test_version_1_files_still_load);
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
