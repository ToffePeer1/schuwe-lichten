#include "auth.h"

#include <cstring>

#include "protocol.h"

namespace laser {

namespace {

constexpr uint8_t MAX_LOCKOUT_DOUBLINGS = 6;  // 60 s << 6 > 1 h, so the cap applies from here on

uint32_t lockDuration(uint8_t lockouts) {
  const uint8_t doublings = lockouts > MAX_LOCKOUT_DOUBLINGS ? MAX_LOCKOUT_DOUBLINGS : lockouts;
  const uint32_t duration = PIN_BASE_LOCK_MS << doublings;
  return duration > PIN_MAX_LOCK_MS ? PIN_MAX_LOCK_MS : duration;
}

struct PaddedPin {
  char digits[PIN_MAX_DIGITS + 1];
};

PaddedPin padded(const char* pin) {
  PaddedPin out{};
  if (pin != nullptr) {
    std::strncpy(out.digits, pin, PIN_MAX_DIGITS);
  }
  return out;
}

// Every byte of the padded copies is compared, so timing doesn't reveal how
// many leading digits were right.
bool pinsEqual(const char* a, const char* b) {
  const PaddedPin pa = padded(a);
  const PaddedPin pb = padded(b);
  uint8_t difference = 0;
  for (size_t i = 0; i <= PIN_MAX_DIGITS; ++i) {
    difference |= static_cast<uint8_t>(pa.digits[i] ^ pb.digits[i]);
  }
  return difference == 0;
}

}  // namespace

uint32_t lockRemainingMs(const PinGuard& guard, uint32_t nowMs) {
  if (!guard.isLocked) {
    return 0;
  }
  const int32_t left = static_cast<int32_t>(guard.lockedUntilMs - nowMs);
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

PinGuard expireLock(const PinGuard& guard, uint32_t nowMs) {
  if (!guard.isLocked || lockRemainingMs(guard, nowMs) > 0) {
    return guard;
  }
  PinGuard next = guard;
  next.isLocked = false;
  return next;
}

AuthResult checkPin(const PinGuard& guard, const char* attempt, const char* stored, uint32_t nowMs) {
  const uint32_t remaining = lockRemainingMs(guard, nowMs);
  if (remaining > 0) {
    return {guard, AuthOutcome::Locked, remaining, 0};
  }
  PinGuard next = guard;
  next.isLocked = false;
  if (isValidPin(attempt) && isValidPin(stored) && pinsEqual(attempt, stored)) {
    return {PinGuard{}, AuthOutcome::Accepted, 0, PIN_MAX_FAILURES};
  }
  next.failures = static_cast<uint8_t>(next.failures + 1);
  if (next.failures < PIN_MAX_FAILURES) {
    return {next, AuthOutcome::Rejected, 0, static_cast<uint8_t>(PIN_MAX_FAILURES - next.failures)};
  }
  const uint32_t duration = lockDuration(next.lockouts);
  next.failures = 0;
  next.lockouts = static_cast<uint8_t>(next.lockouts < 255 ? next.lockouts + 1 : 255);
  next.isLocked = true;
  next.lockedUntilMs = nowMs + duration;
  return {next, AuthOutcome::Locked, duration, 0};
}

}  // namespace laser
