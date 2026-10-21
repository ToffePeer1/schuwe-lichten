// Unit tests for lib/laser_core. Host: uv run pio test -e native (needs g++).
// On the board: uv run pio test -e esp32dev -f test_laser_core (hold IO0).

#include <unity.h>

#include "laser_core.h"

using laser::Channels;
using laser::Command;
using laser::CommandType;
using laser::Limits;
using laser::State;

namespace {

constexpr Limits CLAMPED{20, 90, true};
constexpr Limits UNCLAMPED{20, 90, false};
constexpr uint32_t TIMEOUT_MS = 1000;

State withChannel(uint8_t channel, uint8_t value, uint32_t nowMs = 0) {
  Command command;
  command.type = CommandType::SetChannel;
  command.channel = channel;
  command.value = value;
  return laser::applyCommand(State{}, command, nowMs, UNCLAMPED);
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- parsing ---------------------------------------------------------------

void test_parses_channel_value_pair() {
  const Command command = laser::parseCommand("7 40");
  TEST_ASSERT_EQUAL(CommandType::SetChannel, command.type);
  TEST_ASSERT_EQUAL_UINT8(7, command.channel);
  TEST_ASSERT_EQUAL_UINT8(40, command.value);
}

void test_parse_ignores_surrounding_whitespace_and_crlf() {
  const Command command = laser::parseCommand("  10   255 \r\n");
  TEST_ASSERT_EQUAL(CommandType::SetChannel, command.type);
  TEST_ASSERT_EQUAL_UINT8(10, command.channel);
  TEST_ASSERT_EQUAL_UINT8(255, command.value);
}

void test_parses_keywords() {
  TEST_ASSERT_EQUAL(CommandType::Blackout, laser::parseCommand("0").type);
  TEST_ASSERT_EQUAL(CommandType::LaserOff, laser::parseCommand("off").type);
  TEST_ASSERT_EQUAL(CommandType::LaserOff, laser::parseCommand("OFF").type);
  TEST_ASSERT_EQUAL(CommandType::Heartbeat, laser::parseCommand("hb").type);
  TEST_ASSERT_EQUAL(CommandType::Status, laser::parseCommand("?").type);
}

void test_rejects_malformed_commands() {
  const char* bad[] = {"",      "   ",    "7",      "0 5",  "11 5",   "7 256",
                       "7 -1",  "-7 1",   "7 40 1", "a b",  "7 4x",   "7x 4",
                       "1000 1", "7 1000", "on",    "7,40", "0x7 40",
                       "1234567890123456789012345678901234567890"};
  for (const char* line : bad) {
    TEST_ASSERT_EQUAL_MESSAGE(CommandType::Invalid, laser::parseCommand(line).type, line);
  }
  TEST_ASSERT_EQUAL(CommandType::Invalid, laser::parseCommand(nullptr).type);
}

// --- applying commands -------------------------------------------------------

void test_state_starts_all_zero() {
  TEST_ASSERT_TRUE(laser::isAllZero(State{}.channels));
}

void test_set_channel_changes_only_that_channel_and_feeds_watchdog() {
  const State state = laser::applyCommand(State{}, laser::parseCommand("3 100"), 500, CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(100, state.channels[2]);
  TEST_ASSERT_EQUAL_UINT8(0, state.channels[0]);
  TEST_ASSERT_EQUAL_UINT32(500, state.lastValidCommandMs);
}

void test_apply_does_not_mutate_input_state() {
  const State before{};
  const State after = laser::applyCommand(before, laser::parseCommand("2 9"), 1, CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(0, before.channels[1]);
  TEST_ASSERT_EQUAL_UINT8(9, after.channels[1]);
}

void test_invalid_and_status_change_nothing_and_do_not_feed_watchdog() {
  const State start = withChannel(1, 96, 100);
  const State afterInvalid = laser::applyCommand(start, laser::parseCommand("junk"), 900, CLAMPED);
  const State afterStatus = laser::applyCommand(start, laser::parseCommand("?"), 900, CLAMPED);
  TEST_ASSERT_EQUAL_UINT32(100, afterInvalid.lastValidCommandMs);
  TEST_ASSERT_EQUAL_UINT32(100, afterStatus.lastValidCommandMs);
  TEST_ASSERT_EQUAL_UINT8(96, afterStatus.channels[0]);
}

void test_heartbeat_feeds_watchdog_without_changing_channels() {
  const State start = withChannel(2, 50, 0);
  const State state = laser::applyCommand(start, laser::parseCommand("hb"), 700, CLAMPED);
  TEST_ASSERT_EQUAL_UINT32(700, state.lastValidCommandMs);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(start.channels.data(), state.channels.data(), laser::CHANNEL_COUNT);
}

void test_ch1_only_changes_on_explicit_ch1_command() {
  State state{};
  const char* others[] = {"2 255", "3 255", "7 80", "9 255", "10 255", "hb", "?"};
  for (const char* line : others) {
    state = laser::applyCommand(state, laser::parseCommand(line), 0, CLAMPED);
  }
  TEST_ASSERT_EQUAL_UINT8(0, state.channels[0]);
}

void test_off_zeroes_ch1_and_keeps_others() {
  State state = withChannel(1, 96);
  state = laser::applyCommand(state, laser::parseCommand("4 30"), 0, CLAMPED);
  state = laser::applyCommand(state, laser::parseCommand("off"), 0, CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(0, state.channels[0]);
  TEST_ASSERT_EQUAL_UINT8(30, state.channels[3]);
}

void test_zero_command_blacks_out_everything() {
  State state = withChannel(1, 96);
  state = laser::applyCommand(state, laser::parseCommand("8 40"), 0, CLAMPED);
  state = laser::applyCommand(state, laser::parseCommand("0"), 0, CLAMPED);
  TEST_ASSERT_TRUE(laser::isAllZero(state.channels));
}

// --- CH7 clamp --------------------------------------------------------------

void test_ch7_clamped_to_limits() {
  TEST_ASSERT_EQUAL_UINT8(20, laser::applyCommand(State{}, laser::parseCommand("7 5"), 0, CLAMPED).channels[6]);
  TEST_ASSERT_EQUAL_UINT8(90, laser::applyCommand(State{}, laser::parseCommand("7 200"), 0, CLAMPED).channels[6]);
  TEST_ASSERT_EQUAL_UINT8(55, laser::applyCommand(State{}, laser::parseCommand("7 55"), 0, CLAMPED).channels[6]);
}

void test_clamp_only_touches_ch7() {
  const State state = laser::applyCommand(State{}, laser::parseCommand("6 5"), 0, CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(5, state.channels[5]);
}

void test_ch7_passes_through_when_clamp_disabled() {
  const State state = laser::applyCommand(State{}, laser::parseCommand("7 200"), 0, UNCLAMPED);
  TEST_ASSERT_EQUAL_UINT8(200, state.channels[6]);
}

void test_dmx_frame_reclamps_ch7_even_from_initial_zero_state() {
  const Channels frame = laser::toDmxFrame(State{}, CLAMPED);
  TEST_ASSERT_EQUAL_UINT8(20, frame[6]);
  TEST_ASSERT_EQUAL_UINT8(0, frame[0]);
}

void test_dmx_frame_matches_state_when_clamp_disabled() {
  const State state = withChannel(7, 3);
  TEST_ASSERT_EQUAL_UINT8(3, laser::toDmxFrame(state, UNCLAMPED)[6]);
}

// --- fail-safe --------------------------------------------------------------

void test_failsafe_not_due_before_timeout() {
  const State state = withChannel(1, 96, 1000);
  TEST_ASSERT_FALSE(laser::isFailsafeDue(state, 1999, TIMEOUT_MS));
}

void test_failsafe_due_at_timeout_when_output_not_dark() {
  const State state = withChannel(1, 96, 1000);
  TEST_ASSERT_TRUE(laser::isFailsafeDue(state, 2000, TIMEOUT_MS));
}

void test_failsafe_due_for_any_nonzero_channel() {
  const State state = withChannel(5, 1, 0);
  TEST_ASSERT_TRUE(laser::isFailsafeDue(state, 5000, TIMEOUT_MS));
}

void test_failsafe_not_due_when_already_dark() {
  TEST_ASSERT_FALSE(laser::isFailsafeDue(State{}, 100000, TIMEOUT_MS));
}

void test_failsafe_handles_millis_wraparound() {
  const State state = withChannel(1, 96, 0xFFFFFF00u);
  TEST_ASSERT_FALSE(laser::isFailsafeDue(state, 0x00000010u, TIMEOUT_MS));
  TEST_ASSERT_TRUE(laser::isFailsafeDue(state, 0x00000300u, TIMEOUT_MS));
}

void test_blackout_zeroes_everything() {
  State state = withChannel(1, 200);
  state = laser::applyCommand(state, laser::parseCommand("10 200"), 0, CLAMPED);
  TEST_ASSERT_TRUE(laser::isAllZero(laser::blackout(state).channels));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_parses_channel_value_pair);
  RUN_TEST(test_parse_ignores_surrounding_whitespace_and_crlf);
  RUN_TEST(test_parses_keywords);
  RUN_TEST(test_rejects_malformed_commands);
  RUN_TEST(test_state_starts_all_zero);
  RUN_TEST(test_set_channel_changes_only_that_channel_and_feeds_watchdog);
  RUN_TEST(test_apply_does_not_mutate_input_state);
  RUN_TEST(test_invalid_and_status_change_nothing_and_do_not_feed_watchdog);
  RUN_TEST(test_heartbeat_feeds_watchdog_without_changing_channels);
  RUN_TEST(test_ch1_only_changes_on_explicit_ch1_command);
  RUN_TEST(test_off_zeroes_ch1_and_keeps_others);
  RUN_TEST(test_zero_command_blacks_out_everything);
  RUN_TEST(test_ch7_clamped_to_limits);
  RUN_TEST(test_clamp_only_touches_ch7);
  RUN_TEST(test_ch7_passes_through_when_clamp_disabled);
  RUN_TEST(test_dmx_frame_reclamps_ch7_even_from_initial_zero_state);
  RUN_TEST(test_dmx_frame_matches_state_when_clamp_disabled);
  RUN_TEST(test_failsafe_not_due_before_timeout);
  RUN_TEST(test_failsafe_due_at_timeout_when_output_not_dark);
  RUN_TEST(test_failsafe_due_for_any_nonzero_channel);
  RUN_TEST(test_failsafe_not_due_when_already_dark);
  RUN_TEST(test_failsafe_handles_millis_wraparound);
  RUN_TEST(test_blackout_zeroes_everything);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
  delay(2000);  // let the serial monitor attach
  runAllTests();
}
void loop() {}
#else
int main() { return runAllTests(); }
#endif
