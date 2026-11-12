#pragma once

// The whole command/safety state machine as pure functions, so every rule in
// docs/SAFETY.md can be host-tested. The firmware only adds locking, NVS and I/O.
// "Rule N" in comments throughout the code refers to the numbered rules there.

#include <cstdint>

#include "auth.h"
#include "calibration.h"
#include "laser_core.h"
#include "library.h"
#include "protocol.h"
#include "settings_blob.h"
#include "stop_button.h"

namespace laser {

enum class ErrorCode : uint8_t {
  None,
  BadCommand,
  NotAdmin,
  SerialOnly,
  CalibrationBusy,
  NotOwner,
  StopHeld,
  AutoBlocked,
  NotCalibrated,
  WrongPhase,
  NeedWorstCaseLook,
  NeedBothMarks,
  NeedHorizontalMarks,
  EqualEnds,
  BadPin,
  Locked,
  NotCalibrating,
  SaveFailed,
  BadSlot,
  EmptySlot,
  BadName,
  BadSteps,
  PresetInUse,
};

enum class StopReason : uint8_t {
  None,
  Button,
  Off,
  Blackout,
  CalibrationStopped,
  CalibrationTimeout,
  CalibrationOwnerLeft,
  AddressChanged,
  CalibrationSaved,
  DebugStopped,
};

enum class AuthEvent : uint8_t { None, Granted, Rejected, Locked };
enum class CommitKind : uint8_t { Limits, Pin, Address };

const char* errorCodeName(ErrorCode code);
const char* stopReasonName(StopReason reason);

// A running show. The whole show is copied in when it starts, so the DMX-task
// tick can step through it without touching the library.
struct Playback {
  bool isPlaying = false;
  uint8_t showSlot = 0;
  uint8_t step = 0;
  uint32_t stepStartMs = 0;
  ActiveShow show;
};

struct Context {
  State state;
  Settings settings;
  CalSession cal;
  StopButton button;
  PinGuard guard;
  char factoryPin[PIN_MAX_DIGITS + 1] = {};
  Playback playback;          // never persisted: a reboot is dark (rule 1)
  uint8_t loadedPreset = 0;   // last loaded preset slot, 0 = none / changed by hand
};

struct Outcome {
  Context ctx;
  ErrorCode error = ErrorCode::None;
  StopReason stop = StopReason::None;  // a blackout/stop happened (to broadcast)
  AuthEvent auth = AuthEvent::None;
  uint32_t lockedMs = 0;
  uint8_t attemptsLeft = 0;
  bool isLogout = false;
  // Settings changes are two-phase: persist settingsToSave, verify, then commitSettings().
  bool hasSettingsToSave = false;
  CommitKind commitKind = CommitKind::Limits;
  Settings settingsToSave;
};

// Boot: all channels 0, loaded settings, nothing calibrating.
Context makeContext(const Settings& settings, const char* factoryPin);

Outcome handleCommand(const Context& ctx, const Command& command, const Source& source, uint32_t nowMs);

// Once per DMX frame: stop button, calibration timeouts, check-step advance.
// isLoopStalled pauses only the calibration heartbeat rule (see isCalExpired).
Outcome tick(const Context& ctx, bool isStopLevelLow, uint32_t nowMs, bool isLoopStalled = false);

// The command loop is running again after a stall: give a calibration owner a
// fresh heartbeat window, since their queued "hb" messages are only now read.
Context afterLoopStall(const Context& ctx, uint32_t nowMs);

// A WebSocket client went away: ends calibration if it owned it.
Outcome clientLeft(const Context& ctx, uint8_t slot);

// Applies settings after they were persisted and verified.
Outcome commitSettings(const Context& ctx, const Settings& saved, CommitKind kind);

// The 10 laser channels to put on the wire right now.
Channels frameFor(const Context& ctx);

bool isFactoryPin(const Context& ctx);

ErrorCode libraryError(LibraryStatus status);

// Library actions (explicit user commands, rule 5). Refused during a
// calibration/debug session, while IO0 is held, and (rule 8) if the look would
// light the laser while uncalibrated. Loading stops any running show.
Outcome loadPreset(const Context& ctx, uint16_t slot, const Channels& look);
Outcome startShow(const Context& ctx, uint16_t slot, const ActiveShow& show, uint32_t nowMs);
// Stops advancing; the current look stays.
Outcome stopShow(const Context& ctx);
// The show in this slot was deleted: stop it if it is playing.
Outcome showDeleted(const Context& ctx, uint16_t slot);
// The preset in this slot was saved with this look: a playing show that uses
// it holds a stale copy and stops (the look stays). The slot becomes the
// loaded preset if the current look, as the library would store it, is that look.
Outcome presetSaved(const Context& ctx, uint16_t slot, const Channels& look);
// The preset in this slot was deleted: it is no longer the loaded one.
Outcome presetDeleted(const Context& ctx, uint16_t slot);
uint32_t holdLeftMs(const Context& ctx, uint32_t nowMs);

}  // namespace laser
