// Unit tests for laser_core: limits, clamps and the DMX frame.
// Host: bash tools/run_host_tests.sh

#include <unity.h>

#include "laser_core.h"

using namespace laser;

namespace {

Limits calibrated(uint8_t low, uint8_t high) {
  Limits limits;
  limits.ch7Low = low;
  limits.ch7High = high;
  limits.isCalibrated = true;
  return limits;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_default_limits_are_uncalibrated_full_range_without_auto() {
  const Limits limits;
  TEST_ASSERT_FALSE(limits.isCalibrated);
  TEST_ASSERT_TRUE(limits.ch6Enabled);  // horizontal limit on by default, full width
  TEST_ASSERT_EQUAL_UINT8(0, limits.ch6Left);
  TEST_ASSERT_EQUAL_UINT8(127, limits.ch6Right);
  TEST_ASSERT_EQUAL_UINT8(0, limits.ch7Low);
  TEST_ASSERT_EQUAL_UINT8(127, limits.ch7High);
}

void test_ch7_clamped_between_low_and_high() {
  const Limits limits = calibrated(30, 100);
  TEST_ASSERT_EQUAL_UINT8(30, clampCh7(10, limits));
  TEST_ASSERT_EQUAL_UINT8(100, clampCh7(120, limits));
  TEST_ASSERT_EQUAL_UINT8(55, clampCh7(55, limits));
}

void test_ch7_clamp_works_for_inverted_mount() {
  const Limits limits = calibrated(90, 10);  // low end is the higher number
  TEST_ASSERT_EQUAL_UINT8(90, clampCh7(120, limits));
  TEST_ASSERT_EQUAL_UINT8(10, clampCh7(0, limits));
  TEST_ASSERT_EQUAL_UINT8(50, clampCh7(50, limits));
}

void test_ch7_sweep_range_is_always_blocked() {
  const Limits limits;  // full range
  TEST_ASSERT_EQUAL_UINT8(127, clampCh7(200, limits));
  TEST_ASSERT_EQUAL_UINT8(127, clampCh7(255, limits));
}

void test_ch7_hard_cap_survives_out_of_range_limits() {
  Limits broken = calibrated(10, 250);  // must never happen, but the cap still holds
  TEST_ASSERT_EQUAL_UINT8(127, clampCh7(240, broken));
  broken = calibrated(200, 220);
  TEST_ASSERT_EQUAL_UINT8(127, clampCh7(0, broken));
}

void test_ch6_free_when_disabled_including_sweep() {
  Limits limits;
  limits.ch6Enabled = false;
  TEST_ASSERT_EQUAL_UINT8(200, clampCh6(200, limits));
  TEST_ASSERT_EQUAL_UINT8(3, clampCh6(3, limits));
}

void test_ch6_clamped_and_sweep_blocked_when_enabled() {
  Limits limits = calibrated(0, 127);
  limits.ch6Enabled = true;
  limits.ch6Left = 100;  // inverted on purpose
  limits.ch6Right = 20;
  TEST_ASSERT_EQUAL_UINT8(100, clampCh6(200, limits));
  TEST_ASSERT_EQUAL_UINT8(20, clampCh6(5, limits));
  TEST_ASSERT_EQUAL_UINT8(60, clampCh6(60, limits));
}

void test_mode_rules_auto_and_calibration() {
  Limits limits;  // uncalibrated, no auto
  TEST_ASSERT_TRUE(isModeAllowed(0, limits));
  TEST_ASSERT_TRUE(isModeAllowed(63, limits));
  TEST_ASSERT_FALSE(isModeAllowed(64, limits));
  TEST_ASSERT_FALSE(isModeAllowed(200, limits));
  limits.isCalibrated = true;
  TEST_ASSERT_TRUE(isModeAllowed(96, limits));
  TEST_ASSERT_FALSE(isModeAllowed(128, limits));  // auto/sound ignore the limit: never allowed
  TEST_ASSERT_FALSE(isModeAllowed(255, limits));
}

void test_default_horizontal_limit_blocks_the_sweep() {
  const Limits limits;
  TEST_ASSERT_EQUAL_UINT8(127, clampCh6(200, limits));
  TEST_ASSERT_EQUAL_UINT8(40, clampCh6(40, limits));
}

void test_set_channel_rejects_forbidden_mode_instead_of_clamping() {
  const State start{};
  const ApplyResult automatic = setChannel(start, CH_MODE, 160, calibrated(30, 100));
  TEST_ASSERT_EQUAL(ApplyStatus::AutoBlocked, automatic.status);
  TEST_ASSERT_EQUAL_UINT8(0, automatic.state.channels[0]);
  const ApplyResult uncalibrated = setChannel(start, CH_MODE, 96, Limits{});
  TEST_ASSERT_EQUAL(ApplyStatus::NotCalibrated, uncalibrated.status);
  TEST_ASSERT_EQUAL_UINT8(0, uncalibrated.state.channels[0]);
}

void test_set_channel_allows_off_values_when_uncalibrated() {
  const ApplyResult result = setChannel(State{}, CH_MODE, 40, Limits{});
  TEST_ASSERT_EQUAL(ApplyStatus::Ok, result.status);
  TEST_ASSERT_EQUAL_UINT8(40, result.state.channels[0]);
}

void test_set_channel_clamps_positions_and_leaves_others() {
  Limits limits = calibrated(30, 100);
  limits.ch6Enabled = true;
  limits.ch6Left = 40;
  limits.ch6Right = 80;
  State state = setChannel(State{}, CH_VERTICAL_POSITION, 5, limits).state;
  state = setChannel(state, CH_HORIZONTAL_POSITION, 120, limits).state;
  state = setChannel(state, CH_SIZE, 5, limits).state;
  TEST_ASSERT_EQUAL_UINT8(30, state.channels[6]);
  TEST_ASSERT_EQUAL_UINT8(80, state.channels[5]);
  TEST_ASSERT_EQUAL_UINT8(5, state.channels[7]);
}

void test_set_channel_rejects_bad_channel_numbers() {
  TEST_ASSERT_EQUAL(ApplyStatus::BadChannel, setChannel(State{}, 0, 1, Limits{}).status);
  TEST_ASSERT_EQUAL(ApplyStatus::BadChannel, setChannel(State{}, 11, 1, Limits{}).status);
}

void test_set_channel_does_not_mutate_input() {
  const State before{};
  const ApplyResult after = setChannel(before, CH_PATTERN, 9, Limits{});
  TEST_ASSERT_EQUAL_UINT8(0, before.channels[1]);
  TEST_ASSERT_EQUAL_UINT8(9, after.state.channels[1]);
}

void test_blackout_and_off() {
  State state{};
  state.channels.fill(77);
  TEST_ASSERT_TRUE(isAllZero(blackout().channels));
  const State off = laserOff(state);
  TEST_ASSERT_EQUAL_UINT8(0, off.channels[0]);
  TEST_ASSERT_EQUAL_UINT8(77, off.channels[3]);
}

void test_frame_reclamps_a_state_that_left_the_limits() {
  State state{};
  state.channels[CH_MODE - 1] = 200;  // auto, not allowed
  state.channels[CH_VERTICAL_POSITION - 1] = 250;
  const Channels frame = toDmxFrame(state, calibrated(30, 100));
  TEST_ASSERT_EQUAL_UINT8(0, frame[0]);  // sent as 0, never as "manual"
  TEST_ASSERT_EQUAL_UINT8(100, frame[6]);
}

void test_frame_forces_light_off_when_uncalibrated() {
  State state{};
  state.channels[CH_MODE - 1] = 96;
  TEST_ASSERT_EQUAL_UINT8(0, toDmxFrame(state, Limits{})[0]);
  TEST_ASSERT_EQUAL_UINT8(96, toDmxFrame(state, calibrated(0, 127))[0]);
}

void test_frame_lifts_ch7_to_low_end_from_zero_state() {
  const Channels frame = toDmxFrame(State{}, calibrated(30, 100));
  TEST_ASSERT_EQUAL_UINT8(30, frame[6]);
  TEST_ASSERT_EQUAL_UINT8(0, frame[0]);
}

void test_valid_limits() {
  TEST_ASSERT_TRUE(isValidLimits(Limits{}));
  TEST_ASSERT_FALSE(isValidLimits(calibrated(0, 128)));
  Limits limits;
  limits.ch6Right = 200;
  TEST_ASSERT_FALSE(isValidLimits(limits));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_default_limits_are_uncalibrated_full_range_without_auto);
  RUN_TEST(test_ch7_clamped_between_low_and_high);
  RUN_TEST(test_ch7_clamp_works_for_inverted_mount);
  RUN_TEST(test_ch7_sweep_range_is_always_blocked);
  RUN_TEST(test_ch7_hard_cap_survives_out_of_range_limits);
  RUN_TEST(test_ch6_free_when_disabled_including_sweep);
  RUN_TEST(test_ch6_clamped_and_sweep_blocked_when_enabled);
  RUN_TEST(test_default_horizontal_limit_blocks_the_sweep);
  RUN_TEST(test_mode_rules_auto_and_calibration);
  RUN_TEST(test_set_channel_rejects_forbidden_mode_instead_of_clamping);
  RUN_TEST(test_set_channel_allows_off_values_when_uncalibrated);
  RUN_TEST(test_set_channel_clamps_positions_and_leaves_others);
  RUN_TEST(test_set_channel_rejects_bad_channel_numbers);
  RUN_TEST(test_set_channel_does_not_mutate_input);
  RUN_TEST(test_blackout_and_off);
  RUN_TEST(test_frame_reclamps_a_state_that_left_the_limits);
  RUN_TEST(test_frame_forces_light_off_when_uncalibrated);
  RUN_TEST(test_frame_lifts_ch7_to_low_end_from_zero_state);
  RUN_TEST(test_valid_limits);
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
