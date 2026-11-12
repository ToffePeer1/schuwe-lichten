// Unit tests for the text command parser.

#include <unity.h>

#include <cstring>

#include "protocol.h"

using namespace laser;

void setUp() {}
void tearDown() {}

static CommandType typeOf(const char* line) { return parseCommand(line).type; }

void test_channel_value_pairs() {
  const Command c = parseCommand("  10   255 \r\n");
  TEST_ASSERT_EQUAL(CommandType::SetChannel, c.type);
  TEST_ASSERT_EQUAL_UINT8(10, c.channel);
  TEST_ASSERT_EQUAL_UINT16(255, c.value);
  TEST_ASSERT_EQUAL(CommandType::SetChannel, typeOf("7 40"));
}

void test_simple_words_are_case_insensitive() {
  TEST_ASSERT_EQUAL(CommandType::Blackout, typeOf("0"));
  TEST_ASSERT_EQUAL(CommandType::LaserOff, typeOf("OFF"));
  TEST_ASSERT_EQUAL(CommandType::Heartbeat, typeOf("hb"));
  TEST_ASSERT_EQUAL(CommandType::Status, typeOf("?"));
  TEST_ASSERT_EQUAL(CommandType::Logout, typeOf("Logout"));
}

void test_auth_and_pin_commands() {
  const Command auth = parseCommand("auth 01234567");
  TEST_ASSERT_EQUAL(CommandType::Auth, auth.type);
  TEST_ASSERT_EQUAL_STRING("01234567", auth.pin);
  const Command change = parseCommand("pin 123456 87654321");
  TEST_ASSERT_EQUAL(CommandType::ChangePin, change.type);
  TEST_ASSERT_EQUAL_STRING("123456", change.pin);
  TEST_ASSERT_EQUAL_STRING("87654321", change.newPin);
  TEST_ASSERT_EQUAL(CommandType::PinReset, typeOf("pin reset"));
}

void test_bad_pins_are_invalid() {
  const char* bad[] = {"auth 12345", "auth 123456789", "auth 12a456", "auth", "pin 123456", "pin 123456 12",
                       "pin 1234567 abcdefg"};
  for (const char* line : bad) {
    TEST_ASSERT_EQUAL_MESSAGE(CommandType::Invalid, typeOf(line), line);
  }
}

void test_admin_settings_commands() {
  const Command addr = parseCommand("addr 503");
  TEST_ASSERT_EQUAL(CommandType::SetAddress, addr.type);
  TEST_ASSERT_EQUAL_UINT16(503, addr.value);
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("addr 504"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("addr 0"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("auto on"));  // auto/sound removed (rule 7)
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("auto off"));
}

void test_calibration_commands() {
  TEST_ASSERT_EQUAL(CommandType::CalStart, typeOf("cal start"));
  TEST_ASSERT_EQUAL(CommandType::CalStop, typeOf("cal stop"));
  TEST_ASSERT_EQUAL(CommandType::CalAlive, typeOf("cal alive"));
  TEST_ASSERT_EQUAL(CommandType::CalLookSquare, typeOf("cal look square"));
  TEST_ASSERT_EQUAL(CommandType::CalLookDiamond, typeOf("cal look diamond"));
  TEST_ASSERT_EQUAL(CommandType::CalLookSpin, typeOf("cal look spin"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("cal look round"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("cal spin on"));   // replaced by "cal look"
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("cal color 13"));  // CH9 can't pick a colour
  TEST_ASSERT_EQUAL(CommandType::DbgStart, typeOf("dbg start"));
  TEST_ASSERT_EQUAL(CommandType::DbgStop, typeOf("dbg stop"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("dbg"));
  TEST_ASSERT_EQUAL(CommandType::MarkLow7, typeOf("lim mark low7"));
  TEST_ASSERT_EQUAL(CommandType::MarkHigh7, typeOf("lim mark high7"));
  TEST_ASSERT_EQUAL(CommandType::MarkLeft6, typeOf("lim mark left6"));
  TEST_ASSERT_EQUAL(CommandType::MarkRight6, typeOf("lim mark right6"));
  TEST_ASSERT_EQUAL(CommandType::HorizontalOn, typeOf("lim h on"));
  TEST_ASSERT_EQUAL(CommandType::HorizontalOff, typeOf("lim h off"));
  TEST_ASSERT_EQUAL(CommandType::LimCheck, typeOf("lim check"));
  TEST_ASSERT_EQUAL(CommandType::LimSave, typeOf("lim save"));
  TEST_ASSERT_EQUAL(CommandType::LimRedo, typeOf("lim redo"));
}

void test_malformed_lines_are_invalid() {
  const char* bad[] = {"", "   ", "7", "0 5", "11 5", "7 256", "7 -1", "-7 1", "7 40 1", "a b", "7 4x", "7x 4",
                       "1000 1", "on", "7,40", "0x7 40", "cal", "cal go", "lim mark low", "lim", "a b c d e"};
  for (const char* line : bad) {
    TEST_ASSERT_EQUAL_MESSAGE(CommandType::Invalid, typeOf(line), line);
  }
  TEST_ASSERT_EQUAL(CommandType::Invalid, parseCommand(nullptr).type);
}

void test_valid_pin_format() {
  TEST_ASSERT_TRUE(isValidPin("000000"));
  TEST_ASSERT_TRUE(isValidPin("12345678"));
  TEST_ASSERT_FALSE(isValidPin("12345"));
  TEST_ASSERT_FALSE(isValidPin("123456789"));
  TEST_ASSERT_FALSE(isValidPin("12 456"));
  TEST_ASSERT_FALSE(isValidPin(nullptr));
}

void test_admin_classification() {
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::SetChannel));
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::LaserOff));
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::Blackout));
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::Auth));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::CalStart));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::SetAddress));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::LimSave));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::PinReset));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::DbgStart));
  TEST_ASSERT_TRUE(isAdminCommand(CommandType::CalLookDiamond));
}


void test_preset_commands_keep_name_case_and_spaces() {
  const Command save = parseCommand("preset save 3   Blauwe Tunnel ♫  ");
  TEST_ASSERT_EQUAL(CommandType::PresetSave, save.type);
  TEST_ASSERT_EQUAL_UINT16(3, save.slot);
  TEST_ASSERT_EQUAL_STRING("Blauwe Tunnel ♫", save.name);
  TEST_ASSERT_FALSE(save.isNameTooLong);
  TEST_ASSERT_EQUAL(CommandType::PresetLoad, parseCommand("PRESET LOAD 12").type);
  TEST_ASSERT_EQUAL_UINT16(12, parseCommand("preset delete 12").slot);
  TEST_ASSERT_TRUE(parseCommand("preset save 1 123456789012345678901234567890123").isNameTooLong);
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("preset save 1"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("preset load"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("preset load 1 2"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("preset eat 1"));
}

void test_show_commands_and_steps() {
  const Command save = parseCommand("show save 2 1:50,12:6000,3:1 Mijn show");
  TEST_ASSERT_EQUAL(CommandType::ShowSave, save.type);
  TEST_ASSERT_EQUAL_UINT16(2, save.slot);
  TEST_ASSERT_EQUAL_STRING("Mijn show", save.name);
  TEST_ASSERT_EQUAL_UINT8(3, save.stepCount);
  TEST_ASSERT_EQUAL_UINT16(12, save.stepPresets[1]);
  TEST_ASSERT_EQUAL_UINT16(6000, save.stepHolds[1]);
  TEST_ASSERT_FALSE(save.areStepsMalformed);
  TEST_ASSERT_TRUE(parseCommand("show save 2 1:50, x").areStepsMalformed);
  TEST_ASSERT_TRUE(parseCommand("show save 2 1:50,,2:3 x").areStepsMalformed);
  TEST_ASSERT_TRUE(parseCommand("show save 2 1-50 x").areStepsMalformed);
  TEST_ASSERT_TRUE(parseCommand("show save 2 1:12345 x").areStepsMalformed);
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("show save 2 1:50"));  // no name
  TEST_ASSERT_EQUAL(CommandType::ShowPlay, typeOf("show play 4"));
  TEST_ASSERT_EQUAL(CommandType::ShowStop, typeOf("show stop"));
  TEST_ASSERT_EQUAL(CommandType::ShowDelete, typeOf("show delete 4"));
  TEST_ASSERT_EQUAL(CommandType::LibraryList, typeOf("lib"));
  TEST_ASSERT_EQUAL(CommandType::Invalid, typeOf("lib now"));
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::PresetSave));
  TEST_ASSERT_FALSE(isAdminCommand(CommandType::ShowDelete));
}

void test_max_steps_and_line_length() {
  char line[300] = "show save 1 ";
  for (int i = 0; i < 25; ++i) {
    std::strcat(line, i == 0 ? "1:10" : ",1:10");
  }
  std::strcat(line, " x");
  TEST_ASSERT_TRUE(parseCommand(line).areStepsMalformed);  // 25 steps > 24
  char tooLong[300];
  std::memset(tooLong, 'a', 260);
  tooLong[260] = '\0';
  TEST_ASSERT_EQUAL(CommandType::Invalid, parseCommand(tooLong).type);
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_channel_value_pairs);
  RUN_TEST(test_simple_words_are_case_insensitive);
  RUN_TEST(test_auth_and_pin_commands);
  RUN_TEST(test_bad_pins_are_invalid);
  RUN_TEST(test_admin_settings_commands);
  RUN_TEST(test_calibration_commands);
  RUN_TEST(test_malformed_lines_are_invalid);
  RUN_TEST(test_valid_pin_format);
  RUN_TEST(test_admin_classification);
  RUN_TEST(test_preset_commands_keep_name_case_and_spaces);
  RUN_TEST(test_show_commands_and_steps);
  RUN_TEST(test_max_steps_and_line_length);
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
