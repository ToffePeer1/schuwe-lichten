// Tests for the controller: every safety rule in CLAUDE.md, end to end.

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "controller.h"

using namespace laser;

namespace {

constexpr const char* FACTORY_PIN = "246810";
const Source GUEST{SourceKind::WebSocket, 1, false};
const Source ADMIN{SourceKind::WebSocket, 2, true};
const Source OTHER_ADMIN{SourceKind::WebSocket, 3, true};
const Source SERIAL{SourceKind::Serial, 0, true};

Context boot() { return makeContext(defaultSettings(FACTORY_PIN, 1), FACTORY_PIN); }

Outcome run(const Context& ctx, const char* line, const Source& source, uint32_t nowMs = 0) {
  return handleCommand(ctx, parseCommand(line), source, nowMs);
}

Context must(const Context& ctx, const char* line, const Source& source, uint32_t nowMs = 0) {
  const Outcome o = run(ctx, line, source, nowMs);
  TEST_ASSERT_EQUAL_MESSAGE(ErrorCode::None, o.error, line);
  return o.ctx;
}

// Saves are two-phase; tests commit straight away (the NVS write is the firmware's job).
Context commit(const Outcome& o) {
  TEST_ASSERT_TRUE(o.hasSettingsToSave);
  return commitSettings(o.ctx, o.settingsToSave, o.commitKind).ctx;
}

// The owner watches the whole Controle round (heartbeats keep the session alive).
Context watchFullCheck(Context ctx, const Source& owner, uint32_t startMs) {
  const uint32_t endMs = startMs + CHECK_STEP_COUNT * CAL_CHECK_STEP_MS;
  for (uint32_t t = startMs + 1000; t <= endMs; t += 1000) {
    ctx = run(ctx, "hb", owner, t).ctx;
    ctx = tick(ctx, false, t).ctx;
  }
  return ctx;
}

Context calibrated(uint8_t low, uint8_t high) {
  Context ctx = must(boot(), "cal start", ADMIN, 0);
  char line[16];
  std::snprintf(line, sizeof(line), "7 %u", low);
  ctx = must(ctx, line, ADMIN, 1);
  ctx = must(ctx, "lim mark low7", ADMIN, 2);
  std::snprintf(line, sizeof(line), "7 %u", high);
  ctx = must(ctx, line, ADMIN, 3);
  ctx = must(ctx, "lim mark high7", ADMIN, 4);
  return commit(run(ctx, "lim save", ADMIN, 5));  // the Controle round is optional
}

uint8_t frameCh(const Context& ctx, uint8_t channel) { return frameFor(ctx)[channel - 1]; }

// Laser off: CH1 = 0 and nothing set except the clamped positions (CH6/CH7 sit
// on the limit even when dark, so the beam can never start below it).
bool isDark(const Channels& channels) {
  for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
    const bool isPosition = i == CH_HORIZONTAL_POSITION - 1 || i == CH_VERTICAL_POSITION - 1;
    if (!isPosition && channels[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

void setUp() {}
void tearDown() {}

// Rule 1: boot dark.
void test_boot_is_dark() {
  const Context ctx = boot();
  TEST_ASSERT_TRUE(isAllZero(ctx.state.channels));
  TEST_ASSERT_TRUE(isAllZero(frameFor(ctx)));
  TEST_ASSERT_TRUE(isFactoryPin(ctx));
}

// Rule 8: no light until calibrated.
void test_uncalibrated_laser_cannot_switch_on() {
  const Outcome o = run(boot(), "1 96", GUEST);
  TEST_ASSERT_EQUAL(ErrorCode::NotCalibrated, o.error);
  TEST_ASSERT_EQUAL_UINT8(0, frameCh(o.ctx, CH_MODE));
}

// Rule 7: auto/sound ignore the height limit, so they are never allowed.
void test_auto_and_sound_are_never_allowed() {
  const Context ctx = calibrated(30, 100);
  TEST_ASSERT_EQUAL(ErrorCode::AutoBlocked, run(ctx, "1 224", GUEST).error);
  TEST_ASSERT_EQUAL(ErrorCode::AutoBlocked, run(ctx, "1 128", ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::BadCommand, run(ctx, "auto on", ADMIN).error);  // no toggle any more
}


// Rule 2: the saved limit is enforced on commands and frames.
void test_saved_limit_clamps_every_frame() {
  Context ctx = calibrated(30, 100);
  ctx = must(ctx, "1 96", GUEST);
  ctx = must(ctx, "7 0", GUEST);
  TEST_ASSERT_EQUAL_UINT8(30, frameCh(ctx, CH_VERTICAL_POSITION));
  ctx = must(ctx, "7 200", GUEST);
  TEST_ASSERT_EQUAL_UINT8(100, frameCh(ctx, CH_VERTICAL_POSITION));
}

void test_inverted_calibration_keeps_beam_above_the_low_mark() {
  Context ctx = calibrated(90, 10);  // higher number = lower beam
  ctx = must(ctx, "1 96", GUEST);
  ctx = must(ctx, "7 127", GUEST);
  TEST_ASSERT_EQUAL_UINT8(90, frameCh(ctx, CH_VERTICAL_POSITION));
}

// Rule 3: settings continue when clients leave; no timeout fail-safe any more.
void test_settings_continue_after_disconnect_and_time() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  ctx = clientLeft(ctx, GUEST.slot).ctx;
  for (uint32_t t = 0; t < 600000; t += 25) {
    ctx = tick(ctx, false, t).ctx;
  }
  TEST_ASSERT_EQUAL_UINT8(96, frameCh(ctx, CH_MODE));
}

// Rule 3: off and 0 from anyone, always.
void test_guest_off_ends_someone_elses_calibration() {
  Context ctx = must(boot(), "cal start", ADMIN);
  TEST_ASSERT_EQUAL_UINT8(CAL_MODE_VALUE, frameCh(ctx, CH_MODE));
  const Outcome o = run(ctx, "off", GUEST);
  TEST_ASSERT_EQUAL(ErrorCode::None, o.error);
  TEST_ASSERT_EQUAL(StopReason::Off, o.stop);
  TEST_ASSERT_FALSE(isActive(o.ctx.cal));
  TEST_ASSERT_TRUE(isDark(frameFor(o.ctx)));
}

void test_zero_ends_calibration_and_blacks_out() {
  const Outcome o = run(must(boot(), "cal start", ADMIN), "0", GUEST);
  TEST_ASSERT_EQUAL(StopReason::Blackout, o.stop);
  TEST_ASSERT_TRUE(isDark(frameFor(o.ctx)));
}

void test_off_keeps_other_channels_outside_calibration() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  ctx = must(ctx, "2 40", GUEST);
  ctx = must(ctx, "off", GUEST);
  TEST_ASSERT_EQUAL_UINT8(0, ctx.state.channels[0]);
  TEST_ASSERT_EQUAL_UINT8(40, ctx.state.channels[1]);
}

// IO0 stop button.
void test_stop_button_blacks_out_holds_and_never_resumes() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  ctx = tick(ctx, false, 0).ctx;  // arm
  ctx = tick(ctx, true, 25).ctx;
  const Outcome pressed = tick(ctx, true, 50);
  TEST_ASSERT_EQUAL(StopReason::Button, pressed.stop);
  ctx = pressed.ctx;
  TEST_ASSERT_TRUE(isDark(frameFor(ctx)));
  TEST_ASSERT_EQUAL(ErrorCode::StopHeld, run(ctx, "1 96", GUEST).error);
  TEST_ASSERT_EQUAL(ErrorCode::StopHeld, run(ctx, "cal start", ADMIN).error);
  ctx = tick(ctx, false, 75).ctx;  // released
  TEST_ASSERT_TRUE(isDark(frameFor(ctx)));
  ctx = must(ctx, "1 96", GUEST);  // only an explicit command switches it on again
  TEST_ASSERT_EQUAL_UINT8(96, frameCh(ctx, CH_MODE));
}

void test_stop_button_ends_calibration() {
  Context ctx = tick(boot(), false, 0).ctx;
  ctx = must(ctx, "cal start", ADMIN, 0);
  ctx = tick(ctx, true, 25).ctx;
  const Outcome o = tick(ctx, true, 50);
  TEST_ASSERT_EQUAL(StopReason::Button, o.stop);
  TEST_ASSERT_FALSE(isActive(o.ctx.cal));
}

void test_button_held_from_boot_is_ignored_until_released() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  ctx.button = StopButton{};  // as right after boot: not armed yet
  for (uint32_t t = 0; t < 2000; t += 25) {
    const Outcome o = tick(ctx, true, t);
    TEST_ASSERT_EQUAL(StopReason::None, o.stop);
    ctx = o.ctx;
  }
  TEST_ASSERT_FALSE(ctx.button.isArmed);
}

// Admin protection.
void test_guest_cannot_run_admin_commands() {
  const char* adminLines[] = {"cal start", "dbg start", "addr 20", "pin 246810 135791", "lim save", "cal stop"};
  for (const char* line : adminLines) {
    TEST_ASSERT_EQUAL_MESSAGE(ErrorCode::NotAdmin, run(boot(), line, GUEST).error, line);
  }
}

void test_auth_grants_and_counts_failures() {
  const Outcome bad = run(boot(), "auth 111111", GUEST);
  TEST_ASSERT_EQUAL(AuthEvent::Rejected, bad.auth);
  TEST_ASSERT_EQUAL_UINT8(4, bad.attemptsLeft);
  const Outcome good = run(bad.ctx, "auth 246810", GUEST);
  TEST_ASSERT_EQUAL(AuthEvent::Granted, good.auth);
}

void test_change_pin_needs_old_pin() {
  const Outcome wrong = run(boot(), "pin 111111 135791", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::BadPin, wrong.error);
  TEST_ASSERT_FALSE(wrong.hasSettingsToSave);
  TEST_ASSERT_EQUAL_UINT8(1, wrong.ctx.guard.failures);
  const Context changed = commit(run(boot(), "pin 246810 135791", ADMIN));
  TEST_ASSERT_EQUAL_STRING("135791", changed.settings.pin);
  TEST_ASSERT_FALSE(isFactoryPin(changed));
  TEST_ASSERT_EQUAL(AuthEvent::Granted, run(changed, "auth 135791", GUEST).auth);
}

void test_pin_reset_only_from_serial() {
  const Context changed = commit(run(boot(), "pin 246810 135791", ADMIN));
  TEST_ASSERT_EQUAL(ErrorCode::SerialOnly, run(changed, "pin reset", ADMIN).error);
  const Context reset = commit(run(changed, "pin reset", SERIAL));
  TEST_ASSERT_TRUE(isFactoryPin(reset));
}

// Calibration.
void test_calibration_belongs_to_its_owner() {
  const Context ctx = must(boot(), "cal start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(ctx, "7 20", GUEST).error);
  TEST_ASSERT_EQUAL(ErrorCode::NotOwner, run(ctx, "lim mark low7", OTHER_ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(ctx, "cal start", OTHER_ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::NotOwner, run(ctx, "cal stop", OTHER_ADMIN).error);
}

void test_calibration_pins_every_channel_but_positions() {
  const Context ctx = must(boot(), "cal start", ADMIN);
  const char* pinned[] = {"1 224", "2 100", "3 0", "4 50", "5 200", "8 100", "9 200", "10 200"};
  for (const char* line : pinned) {
    TEST_ASSERT_EQUAL_MESSAGE(ErrorCode::CalibrationBusy, run(ctx, line, ADMIN).error, line);
  }
  TEST_ASSERT_EQUAL(ErrorCode::None, run(ctx, "6 10", ADMIN).error);
}

void test_save_needs_both_marks() {
  Context ctx = must(boot(), "cal start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::NeedBothMarks, run(ctx, "lim save", ADMIN).error);
  ctx = must(ctx, "lim mark low7", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::NeedBothMarks, run(ctx, "lim save", ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::NeedBothMarks, run(ctx, "lim check", ADMIN).error);
}

void test_marks_refused_in_the_flat_square_look() {
  Context ctx = must(boot(), "cal start", ADMIN);
  ctx = must(ctx, "cal look square", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::NeedWorstCaseLook, run(ctx, "lim mark low7", ADMIN).error);
  ctx = must(ctx, "cal look diamond", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::None, run(ctx, "lim mark low7", ADMIN).error);
}

void test_save_without_check_and_mid_check() {
  Context ctx = must(boot(), "cal start", ADMIN);
  ctx = must(ctx, "7 20", ADMIN);
  ctx = must(ctx, "lim mark low7", ADMIN);
  ctx = must(ctx, "7 110", ADMIN);
  ctx = must(ctx, "lim mark high7", ADMIN);
  const Outcome direct = run(ctx, "lim save", ADMIN);  // straight from adjusting
  TEST_ASSERT_TRUE(direct.hasSettingsToSave);
  TEST_ASSERT_EQUAL_UINT8(20, direct.settingsToSave.limits.ch7Low);
  TEST_ASSERT_TRUE(direct.settingsToSave.limits.ch6Enabled);  // full width by default
  ctx = must(ctx, "lim check", ADMIN, 0);
  const Outcome midCheck = run(ctx, "lim save", ADMIN, 1);  // no need to watch the whole round
  TEST_ASSERT_TRUE(midCheck.hasSettingsToSave);
}

void test_cal_stop_discards_marks_and_keeps_old_limits() {
  const Context before = calibrated(30, 100);
  Context ctx = must(before, "cal start", ADMIN, 10);
  ctx = must(ctx, "7 5", ADMIN, 11);
  ctx = must(ctx, "lim mark low7", ADMIN, 12);
  ctx = must(ctx, "cal stop", ADMIN, 13);
  TEST_ASSERT_EQUAL_UINT8(30, ctx.settings.limits.ch7Low);
  ctx = must(ctx, "cal start", ADMIN, 14);
  TEST_ASSERT_EQUAL_INT16(-1, ctx.cal.marks.low7);
}

void test_redo_after_failed_check_does_not_save() {
  Context ctx = must(boot(), "cal start", ADMIN);
  ctx = must(ctx, "7 20", ADMIN);
  ctx = must(ctx, "lim mark low7", ADMIN);
  ctx = must(ctx, "7 110", ADMIN);
  ctx = must(ctx, "lim mark high7", ADMIN);
  ctx = must(ctx, "lim check", ADMIN);
  ctx = must(ctx, "lim redo", ADMIN);
  TEST_ASSERT_FALSE(ctx.settings.limits.isCalibrated);
  TEST_ASSERT_EQUAL(CalPhase::Adjust, ctx.cal.phase);
}

void test_saving_ends_calibration_dark() {
  Context ctx = must(boot(), "cal start", ADMIN);
  ctx = must(ctx, "7 20", ADMIN);
  ctx = must(ctx, "lim mark low7", ADMIN);
  ctx = must(ctx, "7 110", ADMIN);
  ctx = must(ctx, "lim mark high7", ADMIN);
  ctx = must(ctx, "lim check", ADMIN);
  ctx = watchFullCheck(ctx, ADMIN, 0);
  const Outcome save = run(ctx, "lim save", ADMIN, CHECK_STEP_COUNT * CAL_CHECK_STEP_MS + 1);
  TEST_ASSERT_TRUE(isActive(save.ctx.cal));  // still running until the save is verified
  const Outcome committed = commitSettings(save.ctx, save.settingsToSave, save.commitKind);
  TEST_ASSERT_EQUAL(StopReason::CalibrationSaved, committed.stop);
  TEST_ASSERT_FALSE(isActive(committed.ctx.cal));
  TEST_ASSERT_TRUE(committed.ctx.settings.limits.isCalibrated);
  TEST_ASSERT_TRUE(isDark(frameFor(committed.ctx)));
}

void test_calibration_times_out_without_heartbeat() {
  Context ctx = must(boot(), "cal start", ADMIN, 1000);
  ctx = run(ctx, "hb", GUEST, 2000).ctx;  // a guest's heartbeat doesn't count
  const Outcome o = tick(ctx, false, 1000 + CAL_HEARTBEAT_TIMEOUT_MS);
  TEST_ASSERT_EQUAL(StopReason::CalibrationTimeout, o.stop);
  TEST_ASSERT_TRUE(isDark(frameFor(o.ctx)));
}

void test_owner_heartbeat_keeps_calibration_alive() {
  Context ctx = must(boot(), "cal start", ADMIN, 0);
  for (uint32_t t = 1000; t < 60000; t += 1000) {
    ctx = run(ctx, "hb", ADMIN, t).ctx;
    ctx = tick(ctx, false, t).ctx;
  }
  TEST_ASSERT_TRUE(isActive(ctx.cal));
}

void test_owner_leaving_or_logging_out_ends_calibration() {
  const Context ctx = must(boot(), "cal start", ADMIN);
  TEST_ASSERT_TRUE(isActive(clientLeft(ctx, GUEST.slot).ctx.cal));  // another slot leaving changes nothing
  TEST_ASSERT_EQUAL(StopReason::CalibrationOwnerLeft, clientLeft(ctx, ADMIN.slot).stop);
  const Outcome out = run(ctx, "logout", ADMIN);
  TEST_ASSERT_TRUE(out.isLogout);
  TEST_ASSERT_FALSE(isActive(out.ctx.cal));
}

void test_calibration_never_resumes_previous_look() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  ctx = must(ctx, "cal start", ADMIN);
  ctx = must(ctx, "cal stop", ADMIN);
  TEST_ASSERT_TRUE(isDark(frameFor(ctx)));
}

// DMX address.
void test_address_change_uncalibrates_and_blacks_out() {
  Context ctx = must(calibrated(30, 100), "1 96", GUEST);
  const Outcome o = run(ctx, "addr 21", ADMIN);
  TEST_ASSERT_FALSE(o.settingsToSave.limits.isCalibrated);
  const Outcome committed = commitSettings(o.ctx, o.settingsToSave, o.commitKind);
  TEST_ASSERT_EQUAL(StopReason::AddressChanged, committed.stop);
  TEST_ASSERT_EQUAL_UINT16(21, committed.ctx.settings.dmxAddress);
  TEST_ASSERT_TRUE(isDark(frameFor(committed.ctx)));
  TEST_ASSERT_EQUAL(ErrorCode::NotCalibrated, run(committed.ctx, "1 96", GUEST).error);
}

void test_settings_cannot_change_during_calibration() {
  const Context ctx = must(boot(), "cal start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(ctx, "addr 21", ADMIN).error);
}

void test_serial_is_admin_and_can_calibrate() {
  Context ctx = must(boot(), "cal start", SERIAL);
  TEST_ASSERT_TRUE(isOwner(ctx.cal, SERIAL));
  ctx = tick(ctx, false, 10000).ctx;  // no heartbeat needed on serial
  TEST_ASSERT_TRUE(isActive(ctx.cal));
}

void test_bad_command_reports_error() {
  TEST_ASSERT_EQUAL(ErrorCode::BadCommand, run(boot(), "flash the laser", GUEST).error);
}

void test_error_and_stop_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("calibration-busy", errorCodeName(ErrorCode::CalibrationBusy));
  TEST_ASSERT_EQUAL_STRING("need-both-marks", errorCodeName(ErrorCode::NeedBothMarks));
  TEST_ASSERT_EQUAL_STRING("calibration-owner-left", stopReasonName(StopReason::CalibrationOwnerLeft));
  TEST_ASSERT_EQUAL_STRING("button", stopReasonName(StopReason::Button));
}




void test_expired_pin_lock_never_returns_after_millis_wrap() {
  Context ctx = boot();
  for (int i = 0; i < PIN_MAX_FAILURES; ++i) {
    ctx = run(ctx, "auth 111111", GUEST, 0).ctx;
  }
  ctx = tick(ctx, false, PIN_BASE_LOCK_MS).ctx;  // the per-frame tick clears the expired lock
  const uint32_t muchLater = PIN_BASE_LOCK_MS + 0x80000000u + 1000u;
  TEST_ASSERT_EQUAL(AuthEvent::Granted, run(ctx, "auth 246810", GUEST, muchLater).auth);
}


void test_loop_stall_pauses_only_the_heartbeat_rule() {
  Context ctx = must(boot(), "cal start", ADMIN, 0);
  ctx = tick(ctx, false, 5000, true).ctx;  // loop stalled: no hb could be read
  TEST_ASSERT_TRUE(isActive(ctx.cal));
  ctx = afterLoopStall(ctx, 6000);  // loop runs again: fresh heartbeat window
  TEST_ASSERT_TRUE(isActive(tick(ctx, false, 6000 + CAL_HEARTBEAT_TIMEOUT_MS - 1).ctx.cal));
  TEST_ASSERT_EQUAL(StopReason::CalibrationTimeout, tick(ctx, false, 6000 + CAL_HEARTBEAT_TIMEOUT_MS).stop);
}



// Rule 9: debug session, raw channels without limits, admin only.
void test_debug_is_admin_only_and_starts_dark() {
  TEST_ASSERT_EQUAL(ErrorCode::NotAdmin, run(boot(), "dbg start", GUEST).error);
  const Context ctx = must(boot(), "dbg start", ADMIN);
  TEST_ASSERT_EQUAL(CalPhase::Debug, ctx.cal.phase);
  TEST_ASSERT_TRUE(isAllZero(frameFor(ctx)));
}

void test_debug_sends_raw_values_even_uncalibrated() {
  Context ctx = must(boot(), "dbg start", ADMIN);  // uncalibrated laser
  ctx = must(ctx, "7 200", ADMIN);
  ctx = must(ctx, "1 96", ADMIN);
  ctx = must(ctx, "6 180", ADMIN);
  TEST_ASSERT_EQUAL_UINT8(200, frameCh(ctx, CH_VERTICAL_POSITION));
  TEST_ASSERT_EQUAL_UINT8(96, frameCh(ctx, CH_MODE));
  TEST_ASSERT_EQUAL_UINT8(180, frameCh(ctx, CH_HORIZONTAL_POSITION));
  TEST_ASSERT_TRUE(isAllZero(ctx.state.channels));  // normal state untouched
}

void test_debug_belongs_to_its_owner() {
  const Context ctx = must(boot(), "dbg start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(ctx, "7 20", GUEST).error);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(ctx, "cal start", OTHER_ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::NotOwner, run(ctx, "dbg stop", OTHER_ADMIN).error);
}

void test_debug_ends_dark_on_stop_off_and_owner_leaving() {
  const Context ctx = must(must(boot(), "dbg start", ADMIN), "1 96", ADMIN);
  const Outcome stopped = run(ctx, "dbg stop", ADMIN);
  TEST_ASSERT_EQUAL(StopReason::DebugStopped, stopped.stop);
  TEST_ASSERT_TRUE(isAllZero(frameFor(stopped.ctx)));
  const Outcome off = run(ctx, "off", GUEST);
  TEST_ASSERT_FALSE(isActive(off.ctx.cal));
  TEST_ASSERT_TRUE(isAllZero(frameFor(off.ctx)));
  TEST_ASSERT_EQUAL(StopReason::CalibrationOwnerLeft, clientLeft(ctx, ADMIN.slot).stop);
}

void test_debug_and_calibration_exclude_each_other() {
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(must(boot(), "cal start", ADMIN), "dbg start", ADMIN).error);
  const Context debugging = must(boot(), "dbg start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, run(debugging, "cal start", ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::WrongPhase, run(debugging, "lim mark low7", ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::WrongPhase, run(debugging, "lim save", ADMIN).error);
  TEST_ASSERT_EQUAL(ErrorCode::WrongPhase, run(must(boot(), "cal start", ADMIN), "dbg stop", ADMIN).error);
}

void test_stop_button_ends_debug() {
  Context ctx = tick(boot(), false, 0).ctx;
  ctx = must(must(ctx, "dbg start", ADMIN, 0), "1 96", ADMIN, 0);
  ctx = tick(ctx, true, 25).ctx;
  const Outcome o = tick(ctx, true, 50);
  TEST_ASSERT_EQUAL(StopReason::Button, o.stop);
  TEST_ASSERT_TRUE(isAllZero(frameFor(o.ctx)));
}

void test_session_survives_long_idle_with_heartbeats() {
  Context ctx = must(boot(), "cal start", ADMIN, 0);
  for (uint32_t t = 1000; t <= 600000; t += 1000) {  // 10 minutes, heartbeats only
    ctx = run(ctx, "hb", ADMIN, t).ctx;
    ctx = tick(ctx, false, t).ctx;
  }
  TEST_ASSERT_TRUE(isActive(ctx.cal));
}


// --- presets and shows ------------------------------------------------------

Channels presetLook(uint8_t pattern, uint8_t ch7) {
  Channels look{};
  look[CH_MODE - 1] = 96;
  look[CH_PATTERN - 1] = pattern;
  look[CH_VERTICAL_POSITION - 1] = ch7;
  return look;
}

ActiveShow twoStepShow() {
  ActiveShow show;
  show.stepCount = 2;
  show.presetSlots[0] = 1;
  show.presetSlots[1] = 2;
  show.holdTenths[0] = 10;  // 1 s
  show.holdTenths[1] = 20;  // 2 s
  show.looks[0] = presetLook(10, 60);
  show.looks[1] = presetLook(20, 5);  // below the limit: clamped on load and per frame
  return show;
}

void test_load_preset_is_explicit_and_clamped() {
  const Outcome o = loadPreset(calibrated(30, 100), 4, presetLook(10, 5));
  TEST_ASSERT_EQUAL(ErrorCode::None, o.error);
  TEST_ASSERT_EQUAL_UINT8(4, o.ctx.loadedPreset);
  TEST_ASSERT_EQUAL_UINT8(96, frameCh(o.ctx, CH_MODE));
  TEST_ASSERT_EQUAL_UINT8(30, frameCh(o.ctx, CH_VERTICAL_POSITION));
}

void test_library_looks_follow_rule_8() {
  TEST_ASSERT_EQUAL(ErrorCode::NotCalibrated, loadPreset(boot(), 1, presetLook(10, 60)).error);
  TEST_ASSERT_EQUAL(ErrorCode::NotCalibrated, startShow(boot(), 1, twoStepShow(), 0).error);
  Channels dark = presetLook(10, 60);
  dark[CH_MODE - 1] = 0;
  TEST_ASSERT_EQUAL(ErrorCode::None, loadPreset(boot(), 1, dark).error);  // an "off" look is fine
}

void test_library_refused_during_sessions_and_stop() {
  const Context calibrating = must(calibrated(30, 100), "cal start", ADMIN);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, loadPreset(calibrating, 1, presetLook(10, 60)).error);
  TEST_ASSERT_EQUAL(ErrorCode::CalibrationBusy, startShow(calibrating, 1, twoStepShow(), 0).error);
  Context held = tick(calibrated(30, 100), false, 0).ctx;
  held = tick(tick(held, true, 25).ctx, true, 50).ctx;
  TEST_ASSERT_EQUAL(ErrorCode::StopHeld, startShow(held, 1, twoStepShow(), 60).error);
}

void test_show_steps_loop_without_a_phone() {
  Context ctx = startShow(calibrated(30, 100), 3, twoStepShow(), 1000).ctx;
  TEST_ASSERT_TRUE(ctx.playback.isPlaying);
  TEST_ASSERT_EQUAL_UINT8(10, frameCh(ctx, CH_PATTERN));
  ctx = clientLeft(ctx, GUEST.slot).ctx;  // phone gone: keeps playing (rule 3)
  ctx = tick(ctx, false, 1999).ctx;
  TEST_ASSERT_EQUAL_UINT8(10, frameCh(ctx, CH_PATTERN));
  ctx = tick(ctx, false, 2000).ctx;
  TEST_ASSERT_EQUAL_UINT8(20, frameCh(ctx, CH_PATTERN));
  TEST_ASSERT_EQUAL_UINT8(30, frameCh(ctx, CH_VERTICAL_POSITION));  // clamped
  TEST_ASSERT_EQUAL_UINT8(2, ctx.loadedPreset);
  TEST_ASSERT_EQUAL_UINT32(1500, holdLeftMs(ctx, 2500));
  ctx = tick(ctx, false, 4000).ctx;  // loops back to step 1
  TEST_ASSERT_EQUAL_UINT8(0, ctx.playback.step);
  TEST_ASSERT_EQUAL_UINT8(10, frameCh(ctx, CH_PATTERN));
}

void test_off_and_stop_button_end_a_show_for_good() {
  const Context playing = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  const Outcome off = run(playing, "off", GUEST, 10);
  TEST_ASSERT_FALSE(off.ctx.playback.isPlaying);
  TEST_ASSERT_EQUAL_UINT8(0, frameCh(tick(off.ctx, false, 5000).ctx, CH_MODE));  // never resumes
  Context ctx = tick(playing, false, 10).ctx;
  ctx = tick(ctx, true, 35).ctx;
  ctx = tick(ctx, true, 60).ctx;
  TEST_ASSERT_FALSE(ctx.playback.isPlaying);
  ctx = tick(ctx, false, 5000).ctx;
  TEST_ASSERT_TRUE(isDark(frameFor(ctx)));
}

void test_manual_change_and_sessions_take_over_from_a_show() {
  const Context playing = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  const Context manual = must(playing, "8 40", GUEST, 10);
  TEST_ASSERT_FALSE(manual.playback.isPlaying);
  TEST_ASSERT_EQUAL_UINT8(0, manual.loadedPreset);
  TEST_ASSERT_EQUAL_UINT8(96, frameCh(manual, CH_MODE));  // the look stays, now by hand
  const Context debugging = must(playing, "dbg start", ADMIN, 10);
  TEST_ASSERT_FALSE(debugging.playback.isPlaying);
  TEST_ASSERT_TRUE(isAllZero(frameFor(debugging)));
}

void test_show_stop_keeps_the_look_and_delete_stops_only_that_show() {
  const Context playing = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  const Context stopped = stopShow(playing).ctx;
  TEST_ASSERT_FALSE(stopped.playback.isPlaying);
  TEST_ASSERT_EQUAL_UINT8(10, frameCh(stopped, CH_PATTERN));
  TEST_ASSERT_TRUE(showDeleted(playing, 4).ctx.playback.isPlaying);
  TEST_ASSERT_FALSE(showDeleted(playing, 3).ctx.playback.isPlaying);
}

void test_loading_a_preset_stops_a_show() {
  const Context playing = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  const Context loaded = loadPreset(playing, 7, presetLook(30, 80)).ctx;
  TEST_ASSERT_FALSE(loaded.playback.isPlaying);
  TEST_ASSERT_EQUAL_UINT8(7, loaded.loadedPreset);
}

void test_empty_show_is_refused() {
  TEST_ASSERT_EQUAL(ErrorCode::EmptySlot, startShow(calibrated(30, 100), 1, ActiveShow{}, 0).error);
}

void test_show_steps_keep_their_schedule_when_a_frame_is_late() {
  Context ctx = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  ctx = tick(ctx, false, 1030).ctx;  // step 1 ends at 1000, seen 30 ms late
  TEST_ASSERT_EQUAL_UINT8(1, ctx.playback.step);
  TEST_ASSERT_EQUAL_UINT32(1970, holdLeftMs(ctx, 1030));
  ctx = tick(ctx, false, 3000).ctx;  // step 2 still ends at 1000 + 2000
  TEST_ASSERT_EQUAL_UINT8(0, ctx.playback.step);
  ctx = tick(ctx, false, 9000).ctx;  // far behind (a long stall): restart from now
  TEST_ASSERT_EQUAL_UINT8(1, ctx.playback.step);
  TEST_ASSERT_EQUAL_UINT32(2000, holdLeftMs(ctx, 9000));
}

void test_saving_a_preset_a_playing_show_uses_stops_the_show() {
  const Context playing = startShow(calibrated(30, 100), 3, twoStepShow(), 0).ctx;
  const Channels current = playing.state.channels;
  const Context other = presetSaved(playing, 9, current).ctx;
  TEST_ASSERT_TRUE(other.playback.isPlaying);  // the show doesn't use slot 9
  TEST_ASSERT_EQUAL_UINT8(1, other.loadedPreset);
  const Context used = presetSaved(playing, 2, current).ctx;
  TEST_ASSERT_FALSE(used.playback.isPlaying);  // its copy of preset 2 is stale
  TEST_ASSERT_EQUAL_UINT8(2, used.loadedPreset);  // what is showing was just saved as 2
  TEST_ASSERT_EQUAL_UINT8(10, frameCh(used, CH_PATTERN));  // the look stays
}

void test_saved_or_deleted_preset_updates_the_loaded_mark() {
  const Context manual = must(calibrated(30, 100), "1 96", GUEST);
  TEST_ASSERT_EQUAL_UINT8(0, manual.loadedPreset);
  TEST_ASSERT_EQUAL_UINT8(5, presetSaved(manual, 5, manual.state.channels).ctx.loadedPreset);
  const Context loaded = loadPreset(calibrated(30, 100), 4, presetLook(10, 60)).ctx;
  TEST_ASSERT_EQUAL_UINT8(0, presetSaved(loaded, 4, presetLook(11, 60)).ctx.loadedPreset);  // 4 changed meanwhile
  TEST_ASSERT_EQUAL_UINT8(4, presetDeleted(loaded, 6).ctx.loadedPreset);
  TEST_ASSERT_EQUAL_UINT8(0, presetDeleted(loaded, 4).ctx.loadedPreset);
}

void test_preset_saved_after_blackout_is_marked_loaded() {
  const Context dark = must(calibrated(30, 100), "0", GUEST);  // the state holds CH7 = 0, below the limit
  const Channels stored = sanitizeLook(dark.state.channels, dark.settings.limits);  // what the library stores
  TEST_ASSERT_EQUAL_UINT8(30, stored[CH_VERTICAL_POSITION - 1]);
  TEST_ASSERT_EQUAL_UINT8(5, presetSaved(dark, 5, stored).ctx.loadedPreset);
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_is_dark);
  RUN_TEST(test_uncalibrated_laser_cannot_switch_on);
  RUN_TEST(test_auto_and_sound_are_never_allowed);
  RUN_TEST(test_saved_limit_clamps_every_frame);
  RUN_TEST(test_inverted_calibration_keeps_beam_above_the_low_mark);
  RUN_TEST(test_settings_continue_after_disconnect_and_time);
  RUN_TEST(test_guest_off_ends_someone_elses_calibration);
  RUN_TEST(test_zero_ends_calibration_and_blacks_out);
  RUN_TEST(test_off_keeps_other_channels_outside_calibration);
  RUN_TEST(test_stop_button_blacks_out_holds_and_never_resumes);
  RUN_TEST(test_stop_button_ends_calibration);
  RUN_TEST(test_button_held_from_boot_is_ignored_until_released);
  RUN_TEST(test_guest_cannot_run_admin_commands);
  RUN_TEST(test_auth_grants_and_counts_failures);
  RUN_TEST(test_change_pin_needs_old_pin);
  RUN_TEST(test_pin_reset_only_from_serial);
  RUN_TEST(test_calibration_belongs_to_its_owner);
  RUN_TEST(test_calibration_pins_every_channel_but_positions);
  RUN_TEST(test_save_needs_both_marks);
  RUN_TEST(test_marks_refused_in_the_flat_square_look);
  RUN_TEST(test_save_without_check_and_mid_check);
  RUN_TEST(test_cal_stop_discards_marks_and_keeps_old_limits);
  RUN_TEST(test_redo_after_failed_check_does_not_save);
  RUN_TEST(test_saving_ends_calibration_dark);
  RUN_TEST(test_calibration_times_out_without_heartbeat);
  RUN_TEST(test_owner_heartbeat_keeps_calibration_alive);
  RUN_TEST(test_owner_leaving_or_logging_out_ends_calibration);
  RUN_TEST(test_calibration_never_resumes_previous_look);
  RUN_TEST(test_address_change_uncalibrates_and_blacks_out);
  RUN_TEST(test_settings_cannot_change_during_calibration);
  RUN_TEST(test_serial_is_admin_and_can_calibrate);
  RUN_TEST(test_bad_command_reports_error);
  RUN_TEST(test_error_and_stop_names_are_stable);
  RUN_TEST(test_expired_pin_lock_never_returns_after_millis_wrap);
  RUN_TEST(test_loop_stall_pauses_only_the_heartbeat_rule);
  RUN_TEST(test_debug_is_admin_only_and_starts_dark);
  RUN_TEST(test_debug_sends_raw_values_even_uncalibrated);
  RUN_TEST(test_debug_belongs_to_its_owner);
  RUN_TEST(test_debug_ends_dark_on_stop_off_and_owner_leaving);
  RUN_TEST(test_debug_and_calibration_exclude_each_other);
  RUN_TEST(test_stop_button_ends_debug);
  RUN_TEST(test_session_survives_long_idle_with_heartbeats);
  RUN_TEST(test_load_preset_is_explicit_and_clamped);
  RUN_TEST(test_library_looks_follow_rule_8);
  RUN_TEST(test_library_refused_during_sessions_and_stop);
  RUN_TEST(test_show_steps_loop_without_a_phone);
  RUN_TEST(test_off_and_stop_button_end_a_show_for_good);
  RUN_TEST(test_manual_change_and_sessions_take_over_from_a_show);
  RUN_TEST(test_show_stop_keeps_the_look_and_delete_stops_only_that_show);
  RUN_TEST(test_loading_a_preset_stops_a_show);
  RUN_TEST(test_empty_show_is_refused);
  RUN_TEST(test_show_steps_keep_their_schedule_when_a_frame_is_late);
  RUN_TEST(test_saving_a_preset_a_playing_show_uses_stops_the_show);
  RUN_TEST(test_saved_or_deleted_preset_updates_the_loaded_mark);
  RUN_TEST(test_preset_saved_after_blackout_is_marked_loaded);
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
