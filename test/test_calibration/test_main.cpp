// Unit tests for the admin sessions: calibration and debug.

#include <unity.h>

#include "calibration.h"

using namespace laser;

namespace {

const Source OWNER{SourceKind::WebSocket, 2, true};

Limits saved(uint8_t low, uint8_t high) {
  Limits limits;
  limits.ch7Low = low;
  limits.ch7High = high;
  limits.isCalibrated = true;
  return limits;
}

CalSession markedSession(uint8_t low, uint8_t high) {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calSetPosition(s, CH_VERTICAL_POSITION, low, 1).session;
  s = calMark(s, MarkKind::Low7, 2).session;
  s = calSetPosition(s, CH_VERTICAL_POSITION, high, 3).session;
  s = calMark(s, MarkKind::High7, 4).session;
  return s;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_pattern_value_matches_page_formula() {
  TEST_ASSERT_EQUAL_UINT8(2, patternValue(1));
  TEST_ASSERT_EQUAL_UINT8(82, patternValue(17));
  TEST_ASSERT_EQUAL_UINT8(253, patternValue(51));
}

void test_start_at_64_and_no_horizontal_offset_in_ruit_look() {
  const CalSession s = startCalibration(saved(30, 100), OWNER, 100);
  TEST_ASSERT_EQUAL(CalPhase::Adjust, s.phase);
  TEST_ASSERT_EQUAL_UINT8(64, s.ch7);
  TEST_ASSERT_EQUAL_UINT8(0, s.ch6);
  TEST_ASSERT_EQUAL(CalLook::Diamond, s.look);  // no spinning by default
  TEST_ASSERT_TRUE(isOwner(s, OWNER));
  TEST_ASSERT_FALSE(isOwner(s, Source{SourceKind::WebSocket, 3, true}));
  TEST_ASSERT_FALSE(isOwner(s, Source{SourceKind::Serial, 0, true}));
}

void test_horizontal_limit_on_and_pre_marked_full_width() {
  const CalSession s = startCalibration(Limits{}, OWNER, 0);
  TEST_ASSERT_TRUE(s.isHorizontalOn);
  TEST_ASSERT_EQUAL_INT16(0, s.marks.left6);
  TEST_ASSERT_EQUAL_INT16(127, s.marks.right6);
  TEST_ASSERT_EQUAL_INT16(-1, s.marks.low7);
}

void test_ruit_frame_is_square_rotated_64_at_max_size() {
  const CalSession s = startCalibration(Limits{}, OWNER, 0);
  const Channels f = calibrationFrame(s);
  TEST_ASSERT_EQUAL_UINT8(CAL_MODE_VALUE, f[0]);
  TEST_ASSERT_EQUAL_UINT8(patternValue(1), f[1]);
  TEST_ASSERT_EQUAL_UINT8(64, f[2]);
  TEST_ASSERT_EQUAL_UINT8(0, f[5]);
  TEST_ASSERT_EQUAL_UINT8(64, f[6]);
  TEST_ASSERT_EQUAL_UINT8(0, f[7]);  // max size
  TEST_ASSERT_EQUAL_UINT8(CAL_COLOR, f[8]);
  TEST_ASSERT_EQUAL_UINT8(0, f[9]);
}

void test_looks_set_the_rotation() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calSetLook(s, CalLook::Square, 1).session;
  TEST_ASSERT_EQUAL_UINT8(0, calibrationFrame(s)[2]);
  s = calSetLook(s, CalLook::Spin, 2).session;
  TEST_ASSERT_EQUAL_UINT8(CAL_SPIN_VALUE, calibrationFrame(s)[2]);
}

void test_adjust_lets_the_owner_go_below_old_limits() {
  CalSession s = startCalibration(saved(30, 100), OWNER, 0);
  s = calSetPosition(s, CH_VERTICAL_POSITION, 5, 1).session;
  TEST_ASSERT_EQUAL_UINT8(5, calibrationFrame(s)[6]);
}

void test_positions_are_capped_at_127() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calSetPosition(s, CH_VERTICAL_POSITION, 250, 1).session;
  s = calSetPosition(s, CH_HORIZONTAL_POSITION, 200, 1).session;
  TEST_ASSERT_EQUAL_UINT8(127, s.ch7);
  TEST_ASSERT_EQUAL_UINT8(127, calibrationFrame(s)[5]);
}

void test_only_position_channels_can_move() {
  const CalSession s = startCalibration(Limits{}, OWNER, 0);
  TEST_ASSERT_EQUAL(CalStatus::WrongPhase, calSetPosition(s, CH_SIZE, 100, 1).status);
}

void test_marks_need_a_worst_case_look() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calSetLook(s, CalLook::Square, 1).session;
  TEST_ASSERT_EQUAL(CalStatus::NeedWorstCaseLook, calMark(s, MarkKind::Low7, 2).status);
  s = calSetLook(s, CalLook::Spin, 3).session;
  TEST_ASSERT_EQUAL(CalStatus::Ok, calMark(s, MarkKind::Low7, 4).status);
}

void test_candidate_needs_both_vertical_marks() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calMark(s, MarkKind::Low7, 1).session;
  TEST_ASSERT_EQUAL(CalStatus::NeedBothMarks, makeCandidate(s, Limits{}).status);
  TEST_ASSERT_EQUAL(CalStatus::NeedBothMarks, calCheck(s, Limits{}, 2).status);
}

void test_candidate_rejects_equal_ends() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  s = calMark(s, MarkKind::Low7, 1).session;
  s = calMark(s, MarkKind::High7, 2).session;  // same position twice
  TEST_ASSERT_EQUAL(CalStatus::EqualEnds, makeCandidate(s, Limits{}).status);
}

void test_candidate_uses_full_width_horizontal_by_default() {
  const CandidateResult c = makeCandidate(markedSession(30, 100), Limits{});
  TEST_ASSERT_EQUAL(CalStatus::Ok, c.status);
  TEST_ASSERT_TRUE(c.limits.ch6Enabled);
  TEST_ASSERT_EQUAL_UINT8(0, c.limits.ch6Left);
  TEST_ASSERT_EQUAL_UINT8(127, c.limits.ch6Right);
  TEST_ASSERT_TRUE(c.limits.isCalibrated);
}

void test_candidate_can_switch_the_horizontal_limit_off() {
  CalSession s = markedSession(30, 100);
  s = calSetHorizontal(s, false, 5).session;
  TEST_ASSERT_FALSE(makeCandidate(s, Limits{}).limits.ch6Enabled);
}

void test_stale_saved_ends_are_never_reused() {
  // A previous venue saved 30..100; this session marks only the low end.
  CalSession s = startCalibration(saved(30, 100), OWNER, 0);
  s = calSetPosition(s, CH_VERTICAL_POSITION, 80, 1).session;
  s = calMark(s, MarkKind::Low7, 2).session;
  TEST_ASSERT_EQUAL(CalStatus::NeedBothMarks, makeCandidate(s, saved(30, 100)).status);
}

void test_optional_check_runs_stress_steps_at_low_end() {
  CalSession s = markedSession(90, 10);  // inverted mount
  const CalResult checked = calCheck(s, Limits{}, 10);
  TEST_ASSERT_EQUAL(CalStatus::Ok, checked.status);
  s = checked.session;
  TEST_ASSERT_EQUAL(CalPhase::Check, s.phase);
  TEST_ASSERT_EQUAL_UINT8(90, s.candidate.ch7Low);
  const Channels f = calibrationFrame(s);
  TEST_ASSERT_EQUAL_UINT8(90, f[6]);
  TEST_ASSERT_EQUAL_UINT8(0, f[5]);  // no horizontal offset
  TEST_ASSERT_EQUAL_UINT8(CAL_SPIN_VALUE, f[2]);
  s = advanceCheck(s, 10 + CAL_CHECK_STEP_MS);
  TEST_ASSERT_EQUAL_UINT8(1, s.checkStep);
  TEST_ASSERT_EQUAL_UINT8(patternValue(CHECK_STEPS[1].pattern), calibrationFrame(s)[1]);
}

void test_check_completes_only_after_the_last_step() {
  CalSession s = calCheck(markedSession(30, 100), Limits{}, 0).session;
  uint32_t now = 0;
  for (size_t i = 0; i + 1 < CHECK_STEP_COUNT; ++i) {
    now += CAL_CHECK_STEP_MS;
    s = advanceCheck(s, now);
    TEST_ASSERT_FALSE(s.isCheckComplete);
  }
  s = advanceCheck(s, now + CAL_CHECK_STEP_MS);
  TEST_ASSERT_TRUE(s.isCheckComplete);
  TEST_ASSERT_EQUAL_UINT8(0, s.checkStep);
}

void test_redo_returns_to_adjust_and_keeps_marks() {
  CalSession s = calCheck(markedSession(30, 100), Limits{}, 0).session;
  s = calRedo(s, 1).session;
  TEST_ASSERT_EQUAL(CalPhase::Adjust, s.phase);
  TEST_ASSERT_EQUAL_INT16(30, s.marks.low7);
}

void test_heartbeat_timeout_for_websocket_owner() {
  CalSession s = startCalibration(Limits{}, OWNER, 1000);
  TEST_ASSERT_FALSE(isCalExpired(s, 1000 + CAL_HEARTBEAT_TIMEOUT_MS - 1));
  TEST_ASSERT_TRUE(isCalExpired(s, 1000 + CAL_HEARTBEAT_TIMEOUT_MS));
  s = calHeartbeat(s, 3500);
  TEST_ASSERT_FALSE(isCalExpired(s, 5000));
}

void test_no_idle_limit_while_the_page_keeps_sending_hb() {
  CalSession s = startCalibration(Limits{}, OWNER, 0);
  for (uint32_t now = 1000; now <= 30u * 60u * 1000u; now += 1000) {  // half an hour, no actions
    s = calHeartbeat(s, now);
    TEST_ASSERT_FALSE(isCalExpired(s, now));
  }
}

void test_serial_owner_never_expires() {
  const CalSession s = startCalibration(Limits{}, Source{SourceKind::Serial, 0, true}, 0);
  TEST_ASSERT_FALSE(isCalExpired(s, 24u * 3600u * 1000u));
}

void test_loop_stall_pauses_the_heartbeat_rule() {
  const CalSession s = startCalibration(Limits{}, OWNER, 0);
  TEST_ASSERT_FALSE(isCalExpired(s, 10000, true));
  TEST_ASSERT_TRUE(isCalExpired(s, 10000, false));
}

void test_debug_starts_dark_and_sends_raw_channels_without_limits() {
  CalSession s = startDebug(OWNER, 0);
  TEST_ASSERT_EQUAL(CalPhase::Debug, s.phase);
  TEST_ASSERT_TRUE(isAllZero(calibrationFrame(s)));
  s = calSetRaw(s, CH_VERTICAL_POSITION, 200, 1).session;  // sweep range: allowed in debug
  s = calSetRaw(s, CH_MODE, 224, 2).session;               // even auto/sound
  const Channels f = calibrationFrame(s);
  TEST_ASSERT_EQUAL_UINT8(200, f[6]);
  TEST_ASSERT_EQUAL_UINT8(224, f[0]);
}

void test_debug_rejects_calibration_actions() {
  const CalSession s = startDebug(OWNER, 0);
  TEST_ASSERT_EQUAL(CalStatus::WrongPhase, calMark(s, MarkKind::Low7, 1).status);
  TEST_ASSERT_EQUAL(CalStatus::WrongPhase, calSetLook(s, CalLook::Spin, 1).status);
  TEST_ASSERT_EQUAL(CalStatus::WrongPhase, calSetPosition(s, CH_VERTICAL_POSITION, 10, 1).status);
}

void test_raw_only_in_debug() {
  const CalSession s = startCalibration(Limits{}, OWNER, 0);
  TEST_ASSERT_EQUAL(CalStatus::WrongPhase, calSetRaw(s, CH_MODE, 96, 1).status);
}

void test_off_session_never_expires_and_frames_dark() {
  const CalSession off;
  TEST_ASSERT_FALSE(isCalExpired(off, 999999));
  TEST_ASSERT_TRUE(isAllZero(calibrationFrame(off)));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_pattern_value_matches_page_formula);
  RUN_TEST(test_start_at_64_and_no_horizontal_offset_in_ruit_look);
  RUN_TEST(test_horizontal_limit_on_and_pre_marked_full_width);
  RUN_TEST(test_ruit_frame_is_square_rotated_64_at_max_size);
  RUN_TEST(test_looks_set_the_rotation);
  RUN_TEST(test_adjust_lets_the_owner_go_below_old_limits);
  RUN_TEST(test_positions_are_capped_at_127);
  RUN_TEST(test_only_position_channels_can_move);
  RUN_TEST(test_marks_need_a_worst_case_look);
  RUN_TEST(test_candidate_needs_both_vertical_marks);
  RUN_TEST(test_candidate_rejects_equal_ends);
  RUN_TEST(test_candidate_uses_full_width_horizontal_by_default);
  RUN_TEST(test_candidate_can_switch_the_horizontal_limit_off);
  RUN_TEST(test_stale_saved_ends_are_never_reused);
  RUN_TEST(test_optional_check_runs_stress_steps_at_low_end);
  RUN_TEST(test_check_completes_only_after_the_last_step);
  RUN_TEST(test_redo_returns_to_adjust_and_keeps_marks);
  RUN_TEST(test_heartbeat_timeout_for_websocket_owner);
  RUN_TEST(test_no_idle_limit_while_the_page_keeps_sending_hb);
  RUN_TEST(test_serial_owner_never_expires);
  RUN_TEST(test_loop_stall_pauses_the_heartbeat_rule);
  RUN_TEST(test_debug_starts_dark_and_sends_raw_channels_without_limits);
  RUN_TEST(test_debug_rejects_calibration_actions);
  RUN_TEST(test_raw_only_in_debug);
  RUN_TEST(test_off_session_never_expires_and_frames_dark);
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
