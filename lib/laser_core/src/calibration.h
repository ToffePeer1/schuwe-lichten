#pragma once

// Admin sessions: calibration (find the venue limits, optionally check them
// with the widest patterns) and debug (raw channels, no limits, rule 9).

#include <cstddef>
#include <cstdint>

#include "laser_core.h"

namespace laser {

constexpr uint8_t PATTERN_COUNT = 51;
constexpr uint8_t CAL_MODE_VALUE = 96;      // manual
constexpr uint8_t CAL_PATTERN_NUMBER = 1;   // #1 Vierkant (user's choice)
constexpr uint8_t CAL_DIAMOND_ROTATION = 64;  // #1 at CH3 = 64 = "Ruit" (user observation)
constexpr uint8_t CAL_SPIN_VALUE = 140;     // slow forward spin
constexpr uint8_t CAL_COLOR = 0;            // CH9 can't select a fixed colour; keep it simple
constexpr uint8_t CAL_START_CH7 = 64;       // "up high" on the user's laser
constexpr uint8_t CAL_START_CH6 = 0;        // no horizontal offset
constexpr uint32_t CAL_HEARTBEAT_TIMEOUT_MS = 3000;  // the owner's page sends hb every second
constexpr uint32_t CAL_CHECK_STEP_MS = 3000;

enum class CalPhase : uint8_t { Off, Adjust, Check, Debug };
enum class CalLook : uint8_t { Square, Diamond, Spin };
enum class SourceKind : uint8_t { WebSocket, Serial };

struct Source {
  SourceKind kind;
  uint8_t slot;  // WebSocket client number; 0 for serial
  bool isAdmin;
};

// -1 = not marked in this session.
struct Marks {
  int16_t low7 = -1;
  int16_t high7 = -1;
  int16_t left6 = -1;
  int16_t right6 = -1;
};

struct CalSession {
  CalPhase phase = CalPhase::Off;
  SourceKind ownerKind = SourceKind::WebSocket;
  uint8_t ownerSlot = 0;
  CalLook look = CalLook::Diamond;
  uint8_t ch7 = CAL_START_CH7;
  uint8_t ch6 = CAL_START_CH6;
  bool isHorizontalOn = true;
  Marks marks;
  Limits candidate;              // the limits under test in the Check phase
  bool isCheckComplete = false;  // every stress step has been shown since the last "lim check"
  uint8_t checkStep = 0;
  uint32_t checkStepStartMs = 0;
  Channels raw{};                // Debug phase: the channels exactly as sent
  uint32_t lastHeartbeatMs = 0;
  uint32_t lastActionMs = 0;
};

struct CheckStep {
  const char* name;  // Dutch, shown on the admin page
  uint8_t pattern;   // 1-based pattern number
  uint8_t size;      // CH8
  uint8_t verticalFlip;  // CH5
};

constexpr size_t CHECK_STEP_COUNT = 15;
extern const CheckStep CHECK_STEPS[CHECK_STEP_COUNT];

enum class CalStatus : uint8_t {
  Ok,
  WrongPhase,
  NeedWorstCaseLook,
  NeedBothMarks,
  NeedHorizontalMarks,
  EqualEnds,
};

struct CalResult {
  CalSession session;
  CalStatus status;
};

struct CandidateResult {
  Limits limits;
  CalStatus status;
};

enum class MarkKind : uint8_t { Low7, High7, Left6, Right6 };

// CH2 value in the middle of pattern n's band (same formula as the pages).
uint8_t patternValue(uint8_t patternNumber);
const char* lookName(CalLook look);

bool isActive(const CalSession& session);
bool isOwner(const CalSession& session, const Source& source);

// Calibration starts at CH7 64 / CH6 0 in the Ruit look. The horizontal limit
// is taken over from the saved limits (default: on, full width) and pre-marked.
CalSession startCalibration(const Limits& saved, const Source& owner, uint32_t nowMs);

// Debug: every channel raw, starting dark.
CalSession startDebug(const Source& owner, uint32_t nowMs);

CalResult calSetPosition(const CalSession& session, uint8_t channel, uint8_t value, uint32_t nowMs);
CalResult calSetLook(const CalSession& session, CalLook look, uint32_t nowMs);
CalResult calSetHorizontal(const CalSession& session, bool isOn, uint32_t nowMs);
// Marks are only taken in a worst-case look (Ruit or spinning).
CalResult calMark(const CalSession& session, MarkKind kind, uint32_t nowMs);
CalResult calSetRaw(const CalSession& session, uint8_t channel, uint8_t value, uint32_t nowMs);

// The limits the current marks describe: both vertical marks (and horizontal
// ones when on) set this session, ends not equal.
CandidateResult makeCandidate(const CalSession& session, const Limits& saved);

// Optional: enters the Check phase with the candidate limits.
CalResult calCheck(const CalSession& session, const Limits& saved, uint32_t nowMs);
CalResult calRedo(const CalSession& session, uint32_t nowMs);

CalSession calTouch(const CalSession& session, uint32_t nowMs);      // operator action
CalSession calHeartbeat(const CalSession& session, uint32_t nowMs);  // "hb"
// A session only ends by itself when a WebSocket owner's heartbeat is lost
// (page closed, hidden or gone). There is no idle limit (user decision
// 2026-10-23). isLoopStalled: the firmware loop that reads "hb" hasn't run for
// a while, so a missing heartbeat says nothing about the owner.
bool isCalExpired(const CalSession& session, uint32_t nowMs, bool isLoopStalled = false);
CalSession advanceCheck(const CalSession& session, uint32_t nowMs);

// What goes on the wire during a session: the pinned calibration look clamped
// by the relaxed (Adjust) or candidate (Check) limits, or the raw debug channels.
Channels calibrationFrame(const CalSession& session);

}  // namespace laser
