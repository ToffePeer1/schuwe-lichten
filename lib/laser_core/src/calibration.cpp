#include "calibration.h"

namespace laser {

namespace {

constexpr uint8_t SIZE_MAX_FIXED = 0;    // CH8 0 = largest fixed size
constexpr uint8_t SIZE_GROW = 100;       // CH8 64-127: zoom small -> large
constexpr uint8_t SIZE_PULSE = 220;      // CH8 192-255: zoom speed / pulse
constexpr uint8_t VERTICAL_FLIPPING = 200;  // CH5 128-255: keeps flipping

uint8_t capPosition(uint8_t value) { return value > POSITION_MAX ? POSITION_MAX : value; }

CalResult fail(const CalSession& session, CalStatus status) { return {session, status}; }

CalResult ok(const CalSession& session, uint32_t nowMs) { return {calTouch(session, nowMs), CalStatus::Ok}; }

bool isCalibrating(const CalSession& session) {
  return session.phase == CalPhase::Adjust || session.phase == CalPhase::Check;
}

uint8_t rotationFor(CalLook look) {
  switch (look) {
    case CalLook::Square:
      return 0;
    case CalLook::Diamond:
      return CAL_DIAMOND_ROTATION;
    case CalLook::Spin:
      return CAL_SPIN_VALUE;
  }
  return 0;
}

// Relaxed limits for the Adjust phase: the full position range, sweeps still
// blocked, and "calibrated" so the pinned CH1 passes the rule-8 gate.
Limits adjustLimits() {
  Limits limits;
  limits.ch7Low = 0;
  limits.ch7High = POSITION_MAX;
  limits.ch6Enabled = true;
  limits.ch6Left = 0;
  limits.ch6Right = POSITION_MAX;
  limits.isCalibrated = true;
  return limits;
}

Channels pinnedLook(uint8_t pattern, uint8_t rotation, uint8_t verticalFlip, uint8_t ch6, uint8_t ch7,
                    uint8_t size) {
  Channels channels{};
  channels[CH_MODE - 1] = CAL_MODE_VALUE;
  channels[CH_PATTERN - 1] = patternValue(pattern);
  channels[CH_ROTATION - 1] = rotation;
  channels[CH_HORIZONTAL_FLIP - 1] = 0;
  channels[CH_VERTICAL_FLIP - 1] = verticalFlip;
  channels[CH_HORIZONTAL_POSITION - 1] = ch6;
  channels[CH_VERTICAL_POSITION - 1] = ch7;
  channels[CH_SIZE - 1] = size;
  channels[CH_COLOR - 1] = CAL_COLOR;
  channels[CH_DOTS - 1] = 0;
  return channels;
}

}  // namespace

const CheckStep CHECK_STEPS[CHECK_STEP_COUNT] = {
    {"Vierkant", 1, SIZE_MAX_FIXED, 0},
    {"Propeller, 4 bladen", 50, SIZE_MAX_FIXED, 0},
    {"Molentje (5 streepjes)", 35, SIZE_MAX_FIXED, 0},
    {"Omlijnde pijlpunt", 46, SIZE_MAX_FIXED, 0},
    {"Klavertje vier", 39, SIZE_MAX_FIXED, 0},
    {"Knoop", 45, SIZE_MAX_FIXED, 0},
    {"Cijfer 3", 19, SIZE_MAX_FIXED, 0},
    {"Muzieknoten", 16, SIZE_MAX_FIXED, 0},
    {"Hart", 25, SIZE_MAX_FIXED, 0},
    {"Zigzag", 49, SIZE_MAX_FIXED, 0},
    {"Zoomcirkel (groeit)", 17, SIZE_GROW, 0},
    {"Twee zoomcirkels (groeit)", 29, SIZE_GROW, 0},
    {"Zoomcirkel in cirkel (groeit)", 30, SIZE_GROW, 0},
    {"Vierkant, verticaal spiegelen", 1, SIZE_MAX_FIXED, VERTICAL_FLIPPING},
    {"Vierkant, pulseren", 1, SIZE_PULSE, 0},
};

uint8_t patternValue(uint8_t patternNumber) {
  const unsigned n = patternNumber < 1 ? 1 : (patternNumber > PATTERN_COUNT ? PATTERN_COUNT : patternNumber);
  return static_cast<uint8_t>((2 * n - 1) * 128 / PATTERN_COUNT);
}

const char* lookName(CalLook look) {
  switch (look) {
    case CalLook::Square:
      return "square";
    case CalLook::Diamond:
      return "diamond";
    case CalLook::Spin:
      return "spin";
  }
  return "square";
}

bool isActive(const CalSession& session) { return session.phase != CalPhase::Off; }

bool isOwner(const CalSession& session, const Source& source) {
  if (!isActive(session) || source.kind != session.ownerKind) {
    return false;
  }
  return source.kind == SourceKind::Serial || source.slot == session.ownerSlot;
}

CalSession startCalibration(const Limits& saved, const Source& owner, uint32_t nowMs) {
  CalSession session;
  session.phase = CalPhase::Adjust;
  session.ownerKind = owner.kind;
  session.ownerSlot = owner.slot;
  session.isHorizontalOn = saved.ch6Enabled;
  session.marks.left6 = capPosition(saved.ch6Left);  // full width unless narrowed before
  session.marks.right6 = capPosition(saved.ch6Right);
  session.lastHeartbeatMs = nowMs;
  session.lastActionMs = nowMs;
  return session;
}

CalSession startDebug(const Source& owner, uint32_t nowMs) {
  CalSession session;
  session.phase = CalPhase::Debug;
  session.ownerKind = owner.kind;
  session.ownerSlot = owner.slot;
  session.lastHeartbeatMs = nowMs;
  session.lastActionMs = nowMs;
  return session;
}

CalSession calTouch(const CalSession& session, uint32_t nowMs) {
  CalSession next = session;
  next.lastActionMs = nowMs;
  next.lastHeartbeatMs = nowMs;
  return next;
}

CalSession calHeartbeat(const CalSession& session, uint32_t nowMs) {
  CalSession next = session;
  next.lastHeartbeatMs = nowMs;
  return next;
}

CalResult calSetPosition(const CalSession& session, uint8_t channel, uint8_t value, uint32_t nowMs) {
  if (session.phase != CalPhase::Adjust) {
    return fail(session, CalStatus::WrongPhase);
  }
  CalSession next = session;
  if (channel == CH_VERTICAL_POSITION) {
    next.ch7 = capPosition(value);
  } else if (channel == CH_HORIZONTAL_POSITION) {
    next.ch6 = capPosition(value);
  } else {
    return fail(session, CalStatus::WrongPhase);
  }
  return ok(next, nowMs);
}

CalResult calSetLook(const CalSession& session, CalLook look, uint32_t nowMs) {
  if (session.phase != CalPhase::Adjust) {
    return fail(session, CalStatus::WrongPhase);
  }
  CalSession next = session;
  next.look = look;
  return ok(next, nowMs);
}

CalResult calSetHorizontal(const CalSession& session, bool isOn, uint32_t nowMs) {
  if (session.phase != CalPhase::Adjust) {
    return fail(session, CalStatus::WrongPhase);
  }
  CalSession next = session;
  next.isHorizontalOn = isOn;
  return ok(next, nowMs);
}

CalResult calMark(const CalSession& session, MarkKind kind, uint32_t nowMs) {
  if (session.phase != CalPhase::Adjust) {
    return fail(session, CalStatus::WrongPhase);
  }
  if (session.look == CalLook::Square) {
    return fail(session, CalStatus::NeedWorstCaseLook);  // rotated patterns reach lower than the flat square
  }
  CalSession next = session;
  switch (kind) {
    case MarkKind::Low7:
      next.marks.low7 = session.ch7;
      break;
    case MarkKind::High7:
      next.marks.high7 = session.ch7;
      break;
    case MarkKind::Left6:
      next.marks.left6 = session.ch6;
      break;
    case MarkKind::Right6:
      next.marks.right6 = session.ch6;
      break;
  }
  return ok(next, nowMs);
}

CalResult calSetRaw(const CalSession& session, uint8_t channel, uint8_t value, uint32_t nowMs) {
  if (session.phase != CalPhase::Debug || channel < 1 || channel > CHANNEL_COUNT) {
    return fail(session, CalStatus::WrongPhase);
  }
  CalSession next = session;
  next.raw[channel - 1] = value;
  return ok(next, nowMs);
}

CandidateResult makeCandidate(const CalSession& session, const Limits& saved) {
  const Marks& m = session.marks;
  if (m.low7 < 0 || m.high7 < 0) {
    return {saved, CalStatus::NeedBothMarks};
  }
  if (m.low7 == m.high7) {
    return {saved, CalStatus::EqualEnds};
  }
  if (session.isHorizontalOn && (m.left6 < 0 || m.right6 < 0)) {
    return {saved, CalStatus::NeedHorizontalMarks};
  }
  if (session.isHorizontalOn && m.left6 == m.right6) {
    return {saved, CalStatus::EqualEnds};
  }
  Limits candidate = saved;
  candidate.ch7Low = static_cast<uint8_t>(m.low7);
  candidate.ch7High = static_cast<uint8_t>(m.high7);
  candidate.ch6Enabled = session.isHorizontalOn;
  if (session.isHorizontalOn) {
    candidate.ch6Left = static_cast<uint8_t>(m.left6);
    candidate.ch6Right = static_cast<uint8_t>(m.right6);
  }
  candidate.isCalibrated = true;
  return {candidate, CalStatus::Ok};
}

CalResult calCheck(const CalSession& session, const Limits& saved, uint32_t nowMs) {
  if (session.phase != CalPhase::Adjust) {
    return fail(session, CalStatus::WrongPhase);
  }
  const CandidateResult candidate = makeCandidate(session, saved);
  if (candidate.status != CalStatus::Ok) {
    return fail(session, candidate.status);
  }
  CalSession next = session;
  next.candidate = candidate.limits;
  next.phase = CalPhase::Check;
  next.isCheckComplete = false;
  next.checkStep = 0;
  next.checkStepStartMs = nowMs;
  return ok(next, nowMs);
}

CalResult calRedo(const CalSession& session, uint32_t nowMs) {
  if (session.phase != CalPhase::Check) {
    return fail(session, CalStatus::WrongPhase);
  }
  CalSession next = session;
  next.phase = CalPhase::Adjust;
  return ok(next, nowMs);
}

bool isCalExpired(const CalSession& session, uint32_t nowMs, bool isLoopStalled) {
  return isActive(session) && !isLoopStalled && session.ownerKind == SourceKind::WebSocket &&
         nowMs - session.lastHeartbeatMs >= CAL_HEARTBEAT_TIMEOUT_MS;
}

CalSession advanceCheck(const CalSession& session, uint32_t nowMs) {
  if (session.phase != CalPhase::Check || nowMs - session.checkStepStartMs < CAL_CHECK_STEP_MS) {
    return session;
  }
  CalSession next = session;
  next.checkStep = static_cast<uint8_t>((session.checkStep + 1) % CHECK_STEP_COUNT);
  next.checkStepStartMs = nowMs;
  if (next.checkStep == 0) {
    next.isCheckComplete = true;  // the last step has had its full time on the wall
  }
  return next;
}

Channels calibrationFrame(const CalSession& session) {
  if (session.phase == CalPhase::Debug) {
    return session.raw;  // rule 9: no limits in a debug session
  }
  if (session.phase == CalPhase::Adjust) {
    const Channels look =
        pinnedLook(CAL_PATTERN_NUMBER, rotationFor(session.look), 0, session.ch6, session.ch7, SIZE_MAX_FIXED);
    return toDmxFrame(State{look}, adjustLimits());
  }
  if (isCalibrating(session)) {
    const Limits& c = session.candidate;
    const CheckStep& step = CHECK_STEPS[session.checkStep % CHECK_STEP_COUNT];
    const uint8_t ch6 = clampCh6(CAL_START_CH6, c);  // no offset, unless the limit excludes it
    const Channels look = pinnedLook(step.pattern, CAL_SPIN_VALUE, step.verticalFlip, ch6, c.ch7Low, step.size);
    return toDmxFrame(State{look}, c);
  }
  return Channels{};
}

}  // namespace laser
