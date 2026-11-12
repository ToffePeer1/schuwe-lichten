// Unit tests for the admin PIN guard and the IO0 stop button.

#include <unity.h>

#include "auth.h"
#include "stop_button.h"

using namespace laser;

namespace {

constexpr const char* STORED = "246810";

PinGuard failTimes(PinGuard guard, int times, uint32_t nowMs) {
  for (int i = 0; i < times; ++i) {
    guard = checkPin(guard, "111111", STORED, nowMs).guard;
  }
  return guard;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_correct_pin_is_accepted() {
  const AuthResult r = checkPin(PinGuard{}, "246810", STORED, 0);
  TEST_ASSERT_EQUAL(AuthOutcome::Accepted, r.outcome);
}

void test_wrong_pin_counts_down() {
  const AuthResult r = checkPin(PinGuard{}, "111111", STORED, 0);
  TEST_ASSERT_EQUAL(AuthOutcome::Rejected, r.outcome);
  TEST_ASSERT_EQUAL_UINT8(PIN_MAX_FAILURES - 1, r.attemptsLeft);
}

void test_pin_is_a_string_not_a_number() {
  TEST_ASSERT_EQUAL(AuthOutcome::Rejected, checkPin(PinGuard{}, "0246810", STORED, 0).outcome);
  TEST_ASSERT_EQUAL(AuthOutcome::Rejected, checkPin(PinGuard{}, "24681", STORED, 0).outcome);
}

void test_fifth_failure_locks_for_a_minute() {
  PinGuard guard = failTimes(PinGuard{}, PIN_MAX_FAILURES - 1, 0);
  const AuthResult r = checkPin(guard, "111111", STORED, 0);
  TEST_ASSERT_EQUAL(AuthOutcome::Locked, r.outcome);
  TEST_ASSERT_EQUAL_UINT32(PIN_BASE_LOCK_MS, r.lockedForMs);
  // Even the right PIN is refused while locked.
  TEST_ASSERT_EQUAL(AuthOutcome::Locked, checkPin(r.guard, "246810", STORED, 1000).outcome);
}

void test_attempts_during_lockout_do_not_extend_it() {
  PinGuard guard = failTimes(PinGuard{}, PIN_MAX_FAILURES, 0);
  for (int i = 0; i < 50; ++i) {
    guard = checkPin(guard, "111111", STORED, 30000).guard;
  }
  TEST_ASSERT_EQUAL_UINT32(30000, lockRemainingMs(guard, 30000));
  TEST_ASSERT_EQUAL(AuthOutcome::Accepted, checkPin(guard, "246810", STORED, PIN_BASE_LOCK_MS).outcome);
}

void test_lockout_doubles_and_caps_at_one_hour() {
  PinGuard guard{};
  uint32_t now = 0;
  uint32_t expected = PIN_BASE_LOCK_MS;
  for (int round = 0; round < 10; ++round) {
    guard = failTimes(guard, PIN_MAX_FAILURES - 1, now);
    const AuthResult r = checkPin(guard, "111111", STORED, now);
    TEST_ASSERT_EQUAL(AuthOutcome::Locked, r.outcome);
    TEST_ASSERT_EQUAL_UINT32(expected, r.lockedForMs);
    guard = r.guard;
    now += r.lockedForMs;
    expected = expected * 2 > PIN_MAX_LOCK_MS ? PIN_MAX_LOCK_MS : expected * 2;
  }
}

void test_success_resets_the_doubling() {
  PinGuard guard = failTimes(PinGuard{}, PIN_MAX_FAILURES, 0);
  guard = checkPin(guard, "246810", STORED, PIN_BASE_LOCK_MS).guard;
  guard = failTimes(guard, PIN_MAX_FAILURES - 1, PIN_BASE_LOCK_MS);
  TEST_ASSERT_EQUAL_UINT32(PIN_BASE_LOCK_MS, checkPin(guard, "111111", STORED, PIN_BASE_LOCK_MS).lockedForMs);
}

void test_lockout_survives_millis_wraparound() {
  PinGuard guard = failTimes(PinGuard{}, PIN_MAX_FAILURES, 0xFFFFFF00u);
  TEST_ASSERT_TRUE(lockRemainingMs(guard, 0x00000100u) > 0);
  TEST_ASSERT_EQUAL_UINT32(0, lockRemainingMs(guard, 0xFFFFFF00u + PIN_BASE_LOCK_MS));
}

void test_invalid_stored_pin_never_matches() {
  TEST_ASSERT_EQUAL(AuthOutcome::Rejected, checkPin(PinGuard{}, "", "", 0).outcome);
}


void test_expire_lock_clears_only_after_the_lock_time() {
  PinGuard guard = failTimes(PinGuard{}, PIN_MAX_FAILURES, 0);
  TEST_ASSERT_TRUE(expireLock(guard, PIN_BASE_LOCK_MS - 1).isLocked);
  TEST_ASSERT_FALSE(expireLock(guard, PIN_BASE_LOCK_MS).isLocked);
}

// --- stop button ------------------------------------------------------------

void test_button_not_armed_until_seen_released() {
  StopButton b{};
  for (int i = 0; i < 10; ++i) {
    const ButtonUpdate u = updateStopButton(b, true);  // held low from boot
    TEST_ASSERT_EQUAL(ButtonEvent::None, u.event);
    b = u.button;
  }
  TEST_ASSERT_FALSE(b.isArmed);
  b = updateStopButton(b, false).button;
  TEST_ASSERT_TRUE(b.isArmed);
}

void test_button_press_is_debounced() {
  StopButton b = updateStopButton(StopButton{}, false).button;
  ButtonUpdate u = updateStopButton(b, true);
  TEST_ASSERT_EQUAL(ButtonEvent::None, u.event);  // one sample is a glitch
  u = updateStopButton(u.button, false);
  TEST_ASSERT_FALSE(u.button.isHeld);
  u = updateStopButton(u.button, true);
  u = updateStopButton(u.button, true);
  TEST_ASSERT_EQUAL(ButtonEvent::Pressed, u.event);
  TEST_ASSERT_TRUE(u.button.isHeld);
}

void test_button_held_fires_once_then_releases() {
  StopButton b = updateStopButton(StopButton{}, false).button;
  int presses = 0;
  for (int i = 0; i < 20; ++i) {
    const ButtonUpdate u = updateStopButton(b, true);
    presses += u.event == ButtonEvent::Pressed ? 1 : 0;
    b = u.button;
  }
  TEST_ASSERT_EQUAL(1, presses);
  const ButtonUpdate released = updateStopButton(b, false);
  TEST_ASSERT_EQUAL(ButtonEvent::Released, released.event);
  TEST_ASSERT_FALSE(released.button.isHeld);
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_correct_pin_is_accepted);
  RUN_TEST(test_wrong_pin_counts_down);
  RUN_TEST(test_pin_is_a_string_not_a_number);
  RUN_TEST(test_fifth_failure_locks_for_a_minute);
  RUN_TEST(test_attempts_during_lockout_do_not_extend_it);
  RUN_TEST(test_lockout_doubles_and_caps_at_one_hour);
  RUN_TEST(test_success_resets_the_doubling);
  RUN_TEST(test_lockout_survives_millis_wraparound);
  RUN_TEST(test_invalid_stored_pin_never_matches);
  RUN_TEST(test_expire_lock_clears_only_after_the_lock_time);
  RUN_TEST(test_button_not_armed_until_seen_released);
  RUN_TEST(test_button_press_is_debounced);
  RUN_TEST(test_button_held_fires_once_then_releases);
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
