#include "controller.h"

#include <cstring>

namespace laser {

namespace {

Outcome result(const Context& ctx) {
  Outcome outcome;
  outcome.ctx = ctx;
  return outcome;
}

Outcome withError(const Context& ctx, ErrorCode error) {
  Outcome outcome = result(ctx);
  outcome.error = error;
  return outcome;
}

Outcome withStop(const Context& ctx, StopReason reason) {
  Outcome outcome = result(ctx);
  outcome.stop = reason;
  return outcome;
}

Outcome toSave(const Context& ctx, const Settings& settings, CommitKind kind) {
  Outcome outcome = result(ctx);
  outcome.hasSettingsToSave = true;
  outcome.settingsToSave = settings;
  outcome.commitKind = kind;
  return outcome;
}

FixtureType typeAt(const Context& ctx, uint8_t index) { return ctx.settings.patch.slots[index].type; }

uint8_t laserAt(const Context& ctx) { return laserIndex(ctx.settings.patch); }

State laserStateAt(const Context& ctx, uint8_t index) { return State{toLaserChannels(ctx.fixtures[index].channels)}; }

// A laser look as the laser fixture stores it: inside the current limits.
FixtureChannels clampedLook(const Channels& look, const Limits& limits) {
  return toFixtureChannels(reclamp(State{look}, limits).channels);
}

// The show drives the laser (shows only hold laser looks for now).
bool isShowDriving(const Context& ctx, uint8_t index) { return ctx.playback.isPlaying && index == laserAt(ctx); }

// The type's "off": the laser's CH1 to 0, the head's light channels and macro to 0.
FixtureChannels offChannels(FixtureType type, const FixtureChannels& channels) {
  switch (type) {
    case FixtureType::Laser:
      return toFixtureChannels(laserOff(State{toLaserChannels(channels)}).channels);
    case FixtureType::Head:
      return headOff(channels);
    case FixtureType::None:
      break;
  }
  return FixtureChannels{};
}

// Every fixture to 0 with no show running: boot, sessions, patch changes.
Context darkenAll(const Context& ctx) {
  Context next = ctx;
  for (FixtureState& fixture : next.fixtures) {
    fixture = FixtureState{};
  }
  next.playback = Playback{};
  return next;
}

// Every way out of a session lands on all channels 0, with no show running.
Context endCalibration(const Context& ctx) {
  Context next = darkenAll(ctx);
  next.cal = CalSession{};
  return next;
}

uint32_t stepHoldMs(const Playback& playback) {
  return static_cast<uint32_t>(playback.show.holdTenths[playback.step]) * 100u;
}

// Rule 8 for library looks: lighting the laser needs a saved limit.
bool isLookAllowed(const Channels& look, const Limits& limits) {
  return isModeAllowed(look[CH_MODE - 1], limits);
}

ErrorCode lookError(const Channels& look) {
  return look[CH_MODE - 1] >= MODE_AUTO_MIN ? ErrorCode::AutoBlocked : ErrorCode::NotCalibrated;
}

ErrorCode fromApply(ApplyStatus status) {
  switch (status) {
    case ApplyStatus::Ok:
      return ErrorCode::None;
    case ApplyStatus::AutoBlocked:
      return ErrorCode::AutoBlocked;
    case ApplyStatus::NotCalibrated:
      return ErrorCode::NotCalibrated;
    case ApplyStatus::BadChannel:
      return ErrorCode::BadCommand;
  }
  return ErrorCode::BadCommand;
}

ErrorCode fromCal(CalStatus status) {
  switch (status) {
    case CalStatus::Ok:
      return ErrorCode::None;
    case CalStatus::WrongPhase:
      return ErrorCode::WrongPhase;
    case CalStatus::NeedWorstCaseLook:
      return ErrorCode::NeedWorstCaseLook;
    case CalStatus::NeedBothMarks:
      return ErrorCode::NeedBothMarks;
    case CalStatus::NeedHorizontalMarks:
      return ErrorCode::NeedHorizontalMarks;
    case CalStatus::EqualEnds:
      return ErrorCode::EqualEnds;
  }
  return ErrorCode::WrongPhase;
}

Outcome fromCalResult(const Context& ctx, const CalResult& calResult) {
  if (calResult.status != CalStatus::Ok) {
    return withError(ctx, fromCal(calResult.status));
  }
  Context next = ctx;
  next.cal = calResult.session;
  return result(next);
}

// "off" and "0": accepted from anyone, always, and they end a session. Without
// a fixture prefix they act on every fixture.
Outcome handleStop(const Context& ctx, const Command& command) {
  const bool isBlackout = command.type == CommandType::Blackout;
  const StopReason reason = isBlackout ? StopReason::Blackout : StopReason::Off;
  if (isActive(ctx.cal)) {
    return withStop(endCalibration(ctx), reason);
  }
  Context next = ctx;
  for (uint8_t i = 0; i < MAX_FIXTURES; ++i) {
    if (command.fixture != 0 && i != command.fixture - 1) {
      continue;
    }
    if (isShowDriving(next, i)) {
      next.playback = Playback{};  // nothing resumes it
    }
    FixtureState& fixture = next.fixtures[i];
    fixture.channels = isBlackout ? FixtureChannels{} : offChannels(typeAt(next, i), fixture.channels);
    fixture.loadedPreset = 0;
  }
  // Only a stop of every fixture is announced to all phones; "f2 off" is a normal change.
  return command.fixture == 0 ? withStop(next, reason) : result(next);
}

// During a session only its owner may move the laser, within what the session allows.
Outcome handleSessionChannel(const Context& ctx, const Command& command, const Source& source, uint8_t index,
                             uint32_t nowMs) {
  if (!isOwner(ctx.cal, source) || index != laserAt(ctx)) {
    return withError(ctx, ErrorCode::CalibrationBusy);
  }
  const uint8_t value = static_cast<uint8_t>(command.value);
  const CalResult moved = ctx.cal.phase == CalPhase::Debug
                              ? calSetRaw(ctx.cal, command.channel, value, nowMs)  // rule 9: raw
                              : calSetPosition(ctx.cal, command.channel, value, nowMs);
  if (moved.status != CalStatus::Ok) {
    return withError(ctx, ErrorCode::CalibrationBusy);  // calibration pins every other channel
  }
  Context next = ctx;
  next.cal = moved.session;
  return result(next);
}

Outcome handleSetChannel(const Context& ctx, const Command& command, const Source& source, uint32_t nowMs) {
  if (ctx.button.isHeld) {
    return withError(ctx, ErrorCode::StopHeld);
  }
  // Without a prefix a command is for fixture 1, except during a session: that is about the laser.
  const uint8_t index = command.fixture != 0 ? static_cast<uint8_t>(command.fixture - 1)
                        : isActive(ctx.cal)  ? laserAt(ctx)
                                             : 0;
  if (index >= MAX_FIXTURES || typeAt(ctx, index) == FixtureType::None) {
    return withError(ctx, ErrorCode::NoFixture);
  }
  const FixtureType type = typeAt(ctx, index);
  if (command.channel < 1 || command.channel > channelCount(type)) {
    return withError(ctx, ErrorCode::BadCommand);
  }
  if (isActive(ctx.cal)) {
    return handleSessionChannel(ctx, command, source, index, nowMs);
  }
  const uint8_t value = static_cast<uint8_t>(command.value);
  Context next = ctx;
  if (type == FixtureType::Laser) {
    const ApplyResult applied = setChannel(laserStateAt(ctx, index), command.channel, value, ctx.settings.limits);
    if (applied.status != ApplyStatus::Ok) {
      return withError(ctx, fromApply(applied.status));
    }
    next.fixtures[index].channels = toFixtureChannels(applied.state.channels);
  } else {
    next.fixtures[index].channels[command.channel - 1] = value;  // the head has no limits (user decision)
  }
  if (isShowDriving(ctx, index)) {
    next.playback = Playback{};  // a manual change takes over from a show
  }
  next.fixtures[index].loadedPreset = 0;
  return result(next);
}

Outcome withAuthResult(const Context& ctx, const AuthResult& auth) {
  Context next = ctx;
  next.guard = auth.guard;
  Outcome outcome = result(next);
  outcome.lockedMs = auth.lockedForMs;
  outcome.attemptsLeft = auth.attemptsLeft;
  switch (auth.outcome) {
    case AuthOutcome::Accepted:
      outcome.auth = AuthEvent::Granted;
      break;
    case AuthOutcome::Rejected:
      outcome.auth = AuthEvent::Rejected;
      outcome.error = ErrorCode::BadPin;
      break;
    case AuthOutcome::Locked:
      outcome.auth = AuthEvent::Locked;
      outcome.error = ErrorCode::Locked;
      break;
  }
  return outcome;
}

Outcome handleChangePin(const Context& ctx, const Command& command, uint32_t nowMs) {
  const AuthResult auth = checkPin(ctx.guard, command.pin, ctx.settings.pin, nowMs);
  if (auth.outcome != AuthOutcome::Accepted) {
    return withAuthResult(ctx, auth);
  }
  Context next = ctx;
  next.guard = auth.guard;
  Settings settings = ctx.settings;
  std::memset(settings.pin, 0, sizeof(settings.pin));
  std::strncpy(settings.pin, command.newPin, PIN_MAX_DIGITS);
  return toSave(next, settings, CommitKind::Pin);
}

// The laser's limit is only valid for the slots it was calibrated on.
bool isLaserMoved(const Patch& before, const Patch& after) {
  const uint8_t was = laserIndex(before);
  const uint8_t is = laserIndex(after);
  return was != is || (is != NO_FIXTURE && before.slots[was].address != after.slots[is].address);
}

Outcome handleSetPatch(const Context& ctx, const Command& command) {
  if (isActive(ctx.cal)) {
    return withError(ctx, ErrorCode::CalibrationBusy);
  }
  Settings settings = ctx.settings;
  const bool isEmpty = command.fixtureType == FixtureType::None;
  settings.patch.slots[command.fixture - 1] = PatchSlot{command.fixtureType, isEmpty ? uint16_t{1} : command.address};
  if (checkPatch(settings.patch) != PatchStatus::Ok) {
    return withError(ctx, ErrorCode::BadPatch);
  }
  if (settings.patch == ctx.settings.patch) {
    return result(ctx);  // nothing moved: no flash write, no blackout
  }
  if (isLaserMoved(ctx.settings.patch, settings.patch)) {
    settings.limits.isCalibrated = false;  // the clamp must be re-verified on the new slots
  }
  return toSave(ctx, settings, CommitKind::Patch);
}

// Admin commands that need a running calibration owned by this source.
Outcome handleCalibrationCommand(const Context& ctx, const Command& command, const Source& source, uint32_t nowMs) {
  if (!isActive(ctx.cal)) {
    return withError(ctx, ErrorCode::NotCalibrating);
  }
  if (!isOwner(ctx.cal, source)) {
    return withError(ctx, ErrorCode::NotOwner);
  }
  const CalSession& cal = ctx.cal;
  switch (command.type) {
    case CommandType::CalStop:
      return withStop(endCalibration(ctx),
                      cal.phase == CalPhase::Debug ? StopReason::DebugStopped : StopReason::CalibrationStopped);
    case CommandType::DbgStop:
      if (cal.phase != CalPhase::Debug) {
        return withError(ctx, ErrorCode::WrongPhase);
      }
      return withStop(endCalibration(ctx), StopReason::DebugStopped);
    case CommandType::CalAlive: {
      Context next = ctx;
      next.cal = calTouch(cal, nowMs);
      return result(next);
    }
    case CommandType::CalLookSquare:
      return fromCalResult(ctx, calSetLook(cal, CalLook::Square, nowMs));
    case CommandType::CalLookDiamond:
      return fromCalResult(ctx, calSetLook(cal, CalLook::Diamond, nowMs));
    case CommandType::CalLookSpin:
      return fromCalResult(ctx, calSetLook(cal, CalLook::Spin, nowMs));
    case CommandType::MarkLow7:
      return fromCalResult(ctx, calMark(cal, MarkKind::Low7, nowMs));
    case CommandType::MarkHigh7:
      return fromCalResult(ctx, calMark(cal, MarkKind::High7, nowMs));
    case CommandType::MarkLeft6:
      return fromCalResult(ctx, calMark(cal, MarkKind::Left6, nowMs));
    case CommandType::MarkRight6:
      return fromCalResult(ctx, calMark(cal, MarkKind::Right6, nowMs));
    case CommandType::HorizontalOn:
      return fromCalResult(ctx, calSetHorizontal(cal, true, nowMs));
    case CommandType::HorizontalOff:
      return fromCalResult(ctx, calSetHorizontal(cal, false, nowMs));
    case CommandType::LimCheck:
      return fromCalResult(ctx, calCheck(cal, ctx.settings.limits, nowMs));
    case CommandType::LimRedo:
      return fromCalResult(ctx, calRedo(cal, nowMs));
    case CommandType::LimSave: {
      // The Controle round is optional (user decision): save straight from
      // adjusting once the marks are valid, or at any point of a check.
      CandidateResult candidate{cal.candidate, CalStatus::Ok};
      if (cal.phase == CalPhase::Adjust) {
        candidate = makeCandidate(cal, ctx.settings.limits);
      } else if (cal.phase != CalPhase::Check) {
        return withError(ctx, ErrorCode::WrongPhase);
      }
      if (candidate.status != CalStatus::Ok) {
        return withError(ctx, fromCal(candidate.status));
      }
      Context next = ctx;
      next.cal = calTouch(cal, nowMs);
      Settings settings = ctx.settings;
      settings.limits = candidate.limits;
      return toSave(next, settings, CommitKind::Limits);
    }
    default:
      return withError(ctx, ErrorCode::BadCommand);
  }
}

Outcome handleAdmin(const Context& ctx, const Command& command, const Source& source, uint32_t nowMs) {
  if (!source.isAdmin) {
    return withError(ctx, ErrorCode::NotAdmin);
  }
  switch (command.type) {
    case CommandType::PinReset: {
      if (source.kind != SourceKind::Serial) {
        return withError(ctx, ErrorCode::SerialOnly);
      }
      Settings settings = ctx.settings;
      std::memcpy(settings.pin, ctx.factoryPin, sizeof(settings.pin));
      return toSave(ctx, settings, CommitKind::Pin);
    }
    case CommandType::ChangePin:
      return handleChangePin(ctx, command, nowMs);
    case CommandType::SetPatch:
      return handleSetPatch(ctx, command);
    case CommandType::CalStart:
    case CommandType::DbgStart: {
      if (ctx.button.isHeld) {
        return withError(ctx, ErrorCode::StopHeld);
      }
      if (isActive(ctx.cal)) {
        return withError(ctx, ErrorCode::CalibrationBusy);
      }
      if (laserAt(ctx) == NO_FIXTURE) {
        return withError(ctx, ErrorCode::NoFixture);
      }
      Context next = darkenAll(ctx);  // nothing resumes after a session ends
      next.cal = command.type == CommandType::CalStart ? startCalibration(ctx.settings.limits, source, nowMs)
                                                       : startDebug(source, nowMs);
      return result(next);
    }
    default:
      return handleCalibrationCommand(ctx, command, source, nowMs);
  }
}

}  // namespace

const char* errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::BadCommand: return "bad-command";
    case ErrorCode::NotAdmin: return "not-admin";
    case ErrorCode::SerialOnly: return "serial-only";
    case ErrorCode::CalibrationBusy: return "calibration-busy";
    case ErrorCode::NotOwner: return "not-owner";
    case ErrorCode::StopHeld: return "stop-held";
    case ErrorCode::AutoBlocked: return "auto-blocked";
    case ErrorCode::NotCalibrated: return "not-calibrated";
    case ErrorCode::WrongPhase: return "wrong-phase";
    case ErrorCode::NeedWorstCaseLook: return "need-worst-case-look";
    case ErrorCode::NeedBothMarks: return "need-both-marks";
    case ErrorCode::NeedHorizontalMarks: return "need-horizontal-marks";
    case ErrorCode::EqualEnds: return "equal-ends";
    case ErrorCode::BadPin: return "bad-pin";
    case ErrorCode::Locked: return "locked";
    case ErrorCode::NotCalibrating: return "not-calibrating";
    case ErrorCode::SaveFailed: return "save-failed";
    case ErrorCode::BadSlot: return "bad-slot";
    case ErrorCode::EmptySlot: return "empty-slot";
    case ErrorCode::BadName: return "bad-name";
    case ErrorCode::BadSteps: return "bad-steps";
    case ErrorCode::PresetInUse: return "preset-in-use";
    case ErrorCode::NoFixture: return "no-fixture";
    case ErrorCode::BadPatch: return "bad-patch";
  }
  return "unknown";
}

const char* stopReasonName(StopReason reason) {
  switch (reason) {
    case StopReason::None: return "none";
    case StopReason::Button: return "button";
    case StopReason::Off: return "off";
    case StopReason::Blackout: return "blackout";
    case StopReason::CalibrationStopped: return "calibration-stopped";
    case StopReason::CalibrationTimeout: return "calibration-timeout";
    case StopReason::CalibrationOwnerLeft: return "calibration-owner-left";
    case StopReason::PatchChanged: return "patch-changed";
    case StopReason::CalibrationSaved: return "calibration-saved";
    case StopReason::DebugStopped: return "debug-stopped";
  }
  return "unknown";
}

Context makeContext(const Settings& settings, const char* factoryPin) {
  Context ctx;
  ctx.settings = settings;
  if (factoryPin != nullptr) {
    std::strncpy(ctx.factoryPin, factoryPin, PIN_MAX_DIGITS);
  }
  return ctx;
}

Outcome handleCommand(const Context& ctx, const Command& command, const Source& source, uint32_t nowMs) {
  switch (command.type) {
    case CommandType::Invalid:
      return withError(ctx, ErrorCode::BadCommand);
    case CommandType::Status:
      return result(ctx);
    case CommandType::Heartbeat: {
      if (!isOwner(ctx.cal, source)) {
        return result(ctx);
      }
      Context next = ctx;
      next.cal = calHeartbeat(ctx.cal, nowMs);
      return result(next);
    }
    case CommandType::Blackout:
    case CommandType::Off:
      return handleStop(ctx, command);
    case CommandType::SetChannel:
      return handleSetChannel(ctx, command, source, nowMs);
    case CommandType::Auth:
      return withAuthResult(ctx, checkPin(ctx.guard, command.pin, ctx.settings.pin, nowMs));
    case CommandType::Logout: {
      const bool ownsCalibration = isOwner(ctx.cal, source);
      Outcome outcome = ownsCalibration ? withStop(endCalibration(ctx), StopReason::CalibrationOwnerLeft) : result(ctx);
      outcome.isLogout = true;
      return outcome;
    }
    default:
      return handleAdmin(ctx, command, source, nowMs);
  }
}

Context afterLoopStall(const Context& ctx, uint32_t nowMs) {
  if (!isActive(ctx.cal)) {
    return ctx;
  }
  Context next = ctx;
  next.cal = calHeartbeat(ctx.cal, nowMs);
  return next;
}

Outcome tick(const Context& ctx, bool isStopLevelLow, uint32_t nowMs, bool isLoopStalled) {
  Context next = ctx;
  next.guard = expireLock(ctx.guard, nowMs);
  const ButtonUpdate button = updateStopButton(ctx.button, isStopLevelLow);
  next.button = button.button;
  if (button.event == ButtonEvent::Pressed) {
    return withStop(endCalibration(next), StopReason::Button);
  }
  if (isCalExpired(next.cal, nowMs, isLoopStalled)) {
    return withStop(endCalibration(next), StopReason::CalibrationTimeout);
  }
  next.cal = advanceCheck(next.cal, nowMs);
  Playback& play = next.playback;
  const uint8_t laser = laserAt(next);
  const bool isStepOver = play.isPlaying && play.show.stepCount > 0 && nowMs - play.stepStartMs >= stepHoldMs(play);
  if (isStepOver && laser != NO_FIXTURE) {
    const uint32_t lateMs = nowMs - play.stepStartMs - stepHoldMs(play);
    play.step = static_cast<uint8_t>((play.step + 1) % play.show.stepCount);  // loops
    // Keep the schedule (a late frame must not stretch every step), unless it fell far behind.
    play.stepStartMs = lateMs < stepHoldMs(play) ? nowMs - lateMs : nowMs;
    next.fixtures[laser].channels = clampedLook(play.show.looks[play.step], next.settings.limits);
    next.fixtures[laser].loadedPreset = play.show.presetSlots[play.step];
  }
  return result(next);
}

Outcome clientLeft(const Context& ctx, uint8_t slot) {
  const Source leaving{SourceKind::WebSocket, slot, false};
  if (!isOwner(ctx.cal, leaving)) {
    return result(ctx);
  }
  return withStop(endCalibration(ctx), StopReason::CalibrationOwnerLeft);
}

Outcome commitSettings(const Context& ctx, const Settings& saved, CommitKind kind) {
  Context next = ctx;
  next.settings = saved;
  StopReason stop = StopReason::None;
  switch (kind) {
    case CommitKind::Limits:
      next = endCalibration(next);
      stop = StopReason::CalibrationSaved;
      break;
    case CommitKind::Patch:
      next = endCalibration(next);  // fixtures moved: start every one dark
      stop = StopReason::PatchChanged;
      break;
    case CommitKind::Pin:
      break;
  }
  const uint8_t laser = laserAt(next);
  if (laser != NO_FIXTURE) {
    next.fixtures[laser].channels = clampedLook(laserChannels(next), saved.limits);
  }
  return withStop(next, stop);
}

FixtureChannels fixtureFrame(const Context& ctx, uint8_t index) {
  const FixtureType type = typeAt(ctx, index);
  if (ctx.button.isHeld || type == FixtureType::None) {
    return FixtureChannels{};
  }
  if (type == FixtureType::Laser) {
    return toFixtureChannels(isActive(ctx.cal) ? calibrationFrame(ctx.cal)
                                               : toDmxFrame(laserStateAt(ctx, index), ctx.settings.limits));
  }
  return ctx.fixtures[index].channels;  // the head has no limits (user decision)
}

Universe buildUniverse(const Context& ctx) {
  Universe universe{};  // start code 0, every unpatched slot dark
  if (checkPatch(ctx.settings.patch) != PatchStatus::Ok) {
    return universe;  // never happens (the patch is checked on load and save), but overlap would undo the laser's clamp
  }
  for (uint8_t i = 0; i < MAX_FIXTURES; ++i) {
    const PatchSlot& slot = ctx.settings.patch.slots[i];
    const FixtureChannels frame = fixtureFrame(ctx, i);
    for (uint8_t ch = 0; ch < channelCount(slot.type); ++ch) {
      universe[slot.address + ch] = frame[ch];
    }
  }
  return universe;
}

Channels laserChannels(const Context& ctx) {
  const uint8_t laser = laserAt(ctx);
  return laser == NO_FIXTURE ? Channels{} : toLaserChannels(ctx.fixtures[laser].channels);
}

Channels laserFrame(const Context& ctx) {
  const uint8_t laser = laserAt(ctx);
  return laser == NO_FIXTURE ? Channels{} : toLaserChannels(fixtureFrame(ctx, laser));
}

ErrorCode libraryError(LibraryStatus status) {
  switch (status) {
    case LibraryStatus::Ok: return ErrorCode::None;
    case LibraryStatus::BadSlot: return ErrorCode::BadSlot;
    case LibraryStatus::EmptySlot: return ErrorCode::EmptySlot;
    case LibraryStatus::BadName: return ErrorCode::BadName;
    case LibraryStatus::BadSteps: return ErrorCode::BadSteps;
    case LibraryStatus::PresetInUse: return ErrorCode::PresetInUse;
  }
  return ErrorCode::BadCommand;
}

Outcome loadPreset(const Context& ctx, uint16_t slot, const Channels& look) {
  if (isActive(ctx.cal)) {
    return withError(ctx, ErrorCode::CalibrationBusy);
  }
  if (ctx.button.isHeld) {
    return withError(ctx, ErrorCode::StopHeld);
  }
  const uint8_t laser = laserAt(ctx);
  if (laser == NO_FIXTURE) {
    return withError(ctx, ErrorCode::NoFixture);
  }
  if (!isLookAllowed(look, ctx.settings.limits)) {
    return withError(ctx, lookError(look));
  }
  Context next = ctx;
  next.playback = Playback{};
  next.fixtures[laser].channels = clampedLook(look, ctx.settings.limits);
  next.fixtures[laser].loadedPreset = static_cast<uint8_t>(slot);
  return result(next);
}

Outcome startShow(const Context& ctx, uint16_t slot, const ActiveShow& show, uint32_t nowMs) {
  if (isActive(ctx.cal)) {
    return withError(ctx, ErrorCode::CalibrationBusy);
  }
  if (ctx.button.isHeld) {
    return withError(ctx, ErrorCode::StopHeld);
  }
  const uint8_t laser = laserAt(ctx);
  if (laser == NO_FIXTURE) {
    return withError(ctx, ErrorCode::NoFixture);
  }
  if (show.stepCount == 0) {
    return withError(ctx, ErrorCode::EmptySlot);
  }
  for (uint8_t i = 0; i < show.stepCount; ++i) {
    if (!isLookAllowed(show.looks[i], ctx.settings.limits)) {
      return withError(ctx, lookError(show.looks[i]));
    }
  }
  Context next = ctx;
  next.playback = Playback{};
  next.playback.isPlaying = true;
  next.playback.showSlot = static_cast<uint8_t>(slot);
  next.playback.stepStartMs = nowMs;
  next.playback.show = show;
  next.fixtures[laser].channels = clampedLook(show.looks[0], ctx.settings.limits);
  next.fixtures[laser].loadedPreset = show.presetSlots[0];
  return result(next);
}

Outcome stopShow(const Context& ctx) {
  Context next = ctx;
  next.playback = Playback{};
  return result(next);
}

Outcome showDeleted(const Context& ctx, uint16_t slot) {
  if (!ctx.playback.isPlaying || ctx.playback.showSlot != slot) {
    return result(ctx);
  }
  return stopShow(ctx);
}

Outcome presetSaved(const Context& ctx, uint16_t slot, const Channels& look) {
  const uint8_t laser = laserAt(ctx);
  if (laser == NO_FIXTURE) {
    return result(ctx);
  }
  Context next = ctx;
  const Playback& play = ctx.playback;
  for (uint8_t i = 0; play.isPlaying && i < play.show.stepCount; ++i) {
    if (play.show.presetSlots[i] == slot) {
      next.playback = Playback{};  // its copy of this preset is stale
    }
  }
  if (next.playback.isPlaying) {
    return result(next);  // the show keeps marking its own step
  }
  // Compare as stored: after a blackout the state can hold CH7 below the limit.
  FixtureState& fixture = next.fixtures[laser];
  if (sanitizeLook(toLaserChannels(fixture.channels), next.settings.limits) == look) {
    fixture.loadedPreset = static_cast<uint8_t>(slot);
  } else if (fixture.loadedPreset == slot) {
    fixture.loadedPreset = 0;
  }
  return result(next);
}

Outcome presetDeleted(const Context& ctx, uint16_t slot) {
  Context next = ctx;
  const uint8_t laser = laserAt(ctx);
  if (laser != NO_FIXTURE && next.fixtures[laser].loadedPreset == slot) {
    next.fixtures[laser].loadedPreset = 0;
  }
  return result(next);
}

uint32_t holdLeftMs(const Context& ctx, uint32_t nowMs) {
  const Playback& play = ctx.playback;
  if (!play.isPlaying || play.show.stepCount == 0) {
    return 0;
  }
  const uint32_t elapsed = nowMs - play.stepStartMs;
  const uint32_t hold = stepHoldMs(play);
  return elapsed >= hold ? 0 : hold - elapsed;
}

bool isFactoryPin(const Context& ctx) {
  return std::strncmp(ctx.settings.pin, ctx.factoryPin, PIN_MAX_DIGITS + 1) == 0;
}

}  // namespace laser
