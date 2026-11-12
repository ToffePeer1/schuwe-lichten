#pragma once

// Admin PIN check with a global, growing lockout against guessing.

#include <cstdint>

namespace laser {

constexpr uint8_t PIN_MAX_FAILURES = 5;
constexpr uint32_t PIN_BASE_LOCK_MS = 60000;    // first lockout: 60 s
constexpr uint32_t PIN_MAX_LOCK_MS = 3600000;   // doubles each time, up to 1 h

struct PinGuard {
  uint8_t failures = 0;
  uint8_t lockouts = 0;
  bool isLocked = false;
  uint32_t lockedUntilMs = 0;
};

enum class AuthOutcome : uint8_t { Accepted, Rejected, Locked };

struct AuthResult {
  PinGuard guard;
  AuthOutcome outcome;
  uint32_t lockedForMs;  // Locked: time left
  uint8_t attemptsLeft;  // Rejected: tries before the next lockout
};

// A correct PIN resets everything. Attempts during a lockout are refused
// without counting, so a guest can't extend the lockout indefinitely.
AuthResult checkPin(const PinGuard& guard, const char* attempt, const char* stored, uint32_t nowMs);

uint32_t lockRemainingMs(const PinGuard& guard, uint32_t nowMs);

// Clears a lock whose time is up. Called every tick, so a stale absolute
// deadline can never come back after millis() wraps (~24.8 days later).
PinGuard expireLock(const PinGuard& guard, uint32_t nowMs);

}  // namespace laser
