#include "shared_state.h"

#include <Arduino.h>

void SharedLaserState::reset(const laser::Context& ctx) {
  portENTER_CRITICAL(&mux_);
  ctx_ = ctx;
  ++revision_;
  portEXIT_CRITICAL(&mux_);
}

laser::Outcome SharedLaserState::storeLocked(const laser::Outcome& outcome, bool isChange) {
  ctx_ = outcome.ctx;
  if (isChange) {
    ++revision_;
  }
  return outcome;
}

laser::Outcome SharedLaserState::handle(const laser::Command& command, const laser::Source& source) {
  const bool isQuiet = command.type == laser::CommandType::Status || command.type == laser::CommandType::Heartbeat;
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::handleCommand(ctx_, command, source, millis()), !isQuiet);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::commit(const laser::Settings& saved, laser::CommitKind kind) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::commitSettings(ctx_, saved, kind), true);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::clientLeft(uint8_t slot) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome left = laser::clientLeft(ctx_, slot);
  const laser::Outcome outcome = storeLocked(left, left.stop != laser::StopReason::None);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::loadPreset(uint16_t slot, const laser::Channels& look) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome loaded = laser::loadPreset(ctx_, slot, look);
  const laser::Outcome outcome = storeLocked(loaded, loaded.error == laser::ErrorCode::None);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::startShow(uint16_t slot, const laser::ActiveShow& show) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome started = laser::startShow(ctx_, slot, show, millis());
  const laser::Outcome outcome = storeLocked(started, started.error == laser::ErrorCode::None);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::stopShow() {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::stopShow(ctx_), true);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::showDeleted(uint16_t slot) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::showDeleted(ctx_, slot), true);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::presetSaved(uint16_t slot, const laser::Channels& look) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::presetSaved(ctx_, slot, look), true);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

laser::Outcome SharedLaserState::presetDeleted(uint16_t slot) {
  portENTER_CRITICAL(&mux_);
  const laser::Outcome outcome = storeLocked(laser::presetDeleted(ctx_, slot), true);
  portEXIT_CRITICAL(&mux_);
  return outcome;
}

FrameOutput SharedLaserState::frameTick(bool isStopLevelLow) {
  portENTER_CRITICAL(&mux_);
  const bool wasHeld = ctx_.button.isHeld;
  const laser::CalPhase oldPhase = ctx_.cal.phase;
  const uint8_t oldStep = ctx_.cal.checkStep;
  const bool wasPlaying = ctx_.playback.isPlaying;
  const uint8_t oldShowStep = ctx_.playback.step;
  const uint32_t nowMs = millis();
  const bool isLoopStalled = nowMs - lastLoopMs_ > LOOP_STALL_MS;
  const laser::Outcome outcome = laser::tick(ctx_, isStopLevelLow, nowMs, isLoopStalled);
  ctx_ = outcome.ctx;
  const bool isStop = outcome.stop != laser::StopReason::None;
  if (isStop && eventCount_ < EVENT_QUEUE_SIZE) {
    events_[eventCount_++] = outcome.stop;
  }
  if (isStop || wasHeld != ctx_.button.isHeld || oldPhase != ctx_.cal.phase || oldStep != ctx_.cal.checkStep ||
      wasPlaying != ctx_.playback.isPlaying || oldShowStep != ctx_.playback.step) {
    ++revision_;
  }
  const FrameOutput frame{laser::frameFor(ctx_), ctx_.settings.dmxAddress};
  portEXIT_CRITICAL(&mux_);
  return frame;
}

laser::Context SharedLaserState::snapshot(uint32_t* nowMs) const {
  portENTER_CRITICAL(&mux_);
  const laser::Context copy = ctx_;
  if (nowMs != nullptr) {
    *nowMs = millis();
  }
  portEXIT_CRITICAL(&mux_);
  return copy;
}

uint32_t SharedLaserState::revision() const {
  portENTER_CRITICAL(&mux_);
  const uint32_t value = revision_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

void SharedLaserState::noteLoopAlive() {
  portENTER_CRITICAL(&mux_);
  const uint32_t nowMs = millis();
  if (nowMs - lastLoopMs_ > LOOP_STALL_MS) {
    ctx_ = laser::afterLoopStall(ctx_, nowMs);
  }
  lastLoopMs_ = nowMs;
  portEXIT_CRITICAL(&mux_);
}

laser::StopReason SharedLaserState::takeStopEvent() {
  portENTER_CRITICAL(&mux_);
  laser::StopReason reason = laser::StopReason::None;
  if (eventCount_ > 0) {
    reason = events_[0];
    for (size_t i = 1; i < eventCount_; ++i) {
      events_[i - 1] = events_[i];
    }
    --eventCount_;
  }
  portEXIT_CRITICAL(&mux_);
  return reason;
}
