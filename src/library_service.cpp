#include "library_service.h"

#include "library_store.h"

namespace library_service {

namespace {

laser::Library live;
laser::Library scratch;  // edits happen here; it replaces `live` only once saved
uint32_t libraryRevision = 0;
// A corrupt file was loaded as empty; cleared once that file is saved again.
bool isPresetsReset = false;
bool isShowsReset = false;

Result ok(const laser::Outcome& outcome) { return {outcome, outcome.error, false, false}; }

Result fail(laser::ErrorCode error) {
  Result result{};
  result.error = error;
  return result;
}

bool isSessionActive(SharedLaserState& state) { return laser::isActive(state.snapshot(nullptr).cal); }

Result persist(bool (*save)(const laser::Library&), bool* isFileReset) {
  if (!save(scratch)) {
    return {laser::Outcome{}, laser::ErrorCode::SaveFailed, false, true};
  }
  live = scratch;
  ++libraryRevision;
  *isFileReset = false;
  return {laser::Outcome{}, laser::ErrorCode::None, true, false};
}

Result savePreset(SharedLaserState& state, const laser::Command& command) {
  const laser::LibraryStatus status = laser::checkPresetSave(command);
  if (status != laser::LibraryStatus::Ok) {
    return fail(laser::libraryError(status));
  }
  const laser::Context ctx = state.snapshot(nullptr);
  if (laser::laserIndex(ctx.settings.patch) == laser::NO_FIXTURE) {
    return fail(laser::ErrorCode::NoFixture);  // presets hold laser looks for now
  }
  const laser::Channels look = laser::sanitizeLook(laser::laserChannels(ctx), ctx.settings.limits);
  scratch = live;
  laser::storePreset(scratch, command.slot, command.name, look);
  Result result = persist(library_store::savePresets, &isPresetsReset);
  if (result.isChanged) {
    result.outcome = state.presetSaved(command.slot, look);  // a playing show may use this preset
  }
  return result;
}

Result deletePreset(SharedLaserState& state, const laser::Command& command) {
  const laser::LibraryStatus status = laser::checkPresetDelete(live, command.slot);
  if (status != laser::LibraryStatus::Ok) {
    return fail(laser::libraryError(status));
  }
  scratch = live;
  laser::erasePreset(scratch, command.slot);
  Result result = persist(library_store::savePresets, &isPresetsReset);
  if (result.isChanged) {
    result.outcome = state.presetDeleted(command.slot);
  }
  return result;
}

Result saveShow(SharedLaserState& state, const laser::Command& command) {
  const laser::LibraryStatus status = laser::checkShowSave(live, command);
  if (status != laser::LibraryStatus::Ok) {
    return fail(laser::libraryError(status));
  }
  scratch = live;
  laser::storeShow(scratch, command);
  Result result = persist(library_store::saveShows, &isShowsReset);
  if (result.isChanged) {
    result.outcome = state.showDeleted(command.slot);  // a playing copy would be stale
  }
  return result;
}

Result deleteShow(SharedLaserState& state, const laser::Command& command) {
  const laser::LibraryStatus status = laser::checkShowSlot(live, command.slot);
  if (status != laser::LibraryStatus::Ok) {
    return fail(laser::libraryError(status));
  }
  scratch = live;
  laser::eraseShow(scratch, command.slot);
  Result result = persist(library_store::saveShows, &isShowsReset);
  if (result.isChanged) {
    result.outcome = state.showDeleted(command.slot);
  }
  return result;
}

}  // namespace

void begin(const laser::Library& loaded, bool wasPresetsReset, bool wasShowsReset) {
  live = loaded;
  isPresetsReset = wasPresetsReset;
  isShowsReset = wasShowsReset;
}

const laser::Library& library() { return live; }
uint32_t revision() { return libraryRevision; }
bool wasReset() { return isPresetsReset || isShowsReset; }

bool isLibraryCommand(laser::CommandType type) {
  switch (type) {
    case laser::CommandType::PresetSave:
    case laser::CommandType::PresetLoad:
    case laser::CommandType::PresetDelete:
    case laser::CommandType::ShowSave:
    case laser::CommandType::ShowPlay:
    case laser::CommandType::ShowStop:
    case laser::CommandType::ShowDelete:
    case laser::CommandType::LibraryList:
      return true;
    default:
      return false;
  }
}

Result execute(SharedLaserState& state, const laser::Command& command) {
  const bool isEdit = command.type == laser::CommandType::PresetSave ||
                      command.type == laser::CommandType::PresetDelete ||
                      command.type == laser::CommandType::ShowSave || command.type == laser::CommandType::ShowDelete;
  if (isEdit && isSessionActive(state)) {
    return fail(laser::ErrorCode::CalibrationBusy);
  }
  switch (command.type) {
    case laser::CommandType::PresetSave:
      return savePreset(state, command);
    case laser::CommandType::PresetDelete:
      return deletePreset(state, command);
    case laser::CommandType::ShowSave:
      return saveShow(state, command);
    case laser::CommandType::ShowDelete:
      return deleteShow(state, command);
    case laser::CommandType::PresetLoad: {
      const laser::LibraryStatus status = laser::checkPresetSlot(live, command.slot);
      if (status != laser::LibraryStatus::Ok) {
        return fail(laser::libraryError(status));
      }
      return ok(state.loadPreset(command.slot, live.presets[command.slot - 1].channels));
    }
    case laser::CommandType::ShowPlay: {
      const laser::LibraryStatus status = laser::checkShowSlot(live, command.slot);
      if (status != laser::LibraryStatus::Ok) {
        return fail(laser::libraryError(status));
      }
      return ok(state.startShow(command.slot, laser::resolveShow(live, command.slot)));
    }
    case laser::CommandType::ShowStop:
      return ok(state.stopShow());
    default:
      return Result{};  // LibraryList: the caller sends the library message
  }
}

}  // namespace library_service
