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
  const uint8_t fixture = command.fixture == 0 ? 0 : static_cast<uint8_t>(command.fixture - 1);
  const laser::FixtureType type = ctx.settings.patch.slots[fixture].type;
  if (type == laser::FixtureType::None) {
    return fail(laser::ErrorCode::NoFixture);
  }
  const laser::FixtureChannels look = laser::sanitizeLook(type, ctx.fixtures[fixture].channels, ctx.settings.limits);
  scratch = live;
  laser::storePreset(scratch, command.slot, command.name, type, look);
  Result result = persist(library_store::savePresets, &isPresetsReset);
  if (result.isChanged) {
    result.outcome = state.presetSaved(command.slot, fixture, look);  // a playing show may use this preset
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
    result.outcome = state.showChanged(command.slot);  // a playing copy would be stale
  }
  return result;
}

Result deleteShow(SharedLaserState& state, const laser::Command& command) {
  const laser::LibraryStatus status = laser::checkShowDelete(live, command.slot);
  if (status != laser::LibraryStatus::Ok) {
    return fail(laser::libraryError(status));
  }
  scratch = live;
  laser::eraseShow(scratch, command.slot);
  Result result = persist(library_store::saveShows, &isShowsReset);
  if (result.isChanged) {
    result.outcome = state.showChanged(command.slot);
  }
  return result;
}

// Without a prefix a preset goes to the first patched fixture of its type.
uint8_t firstFixtureOfType(SharedLaserState& state, laser::FixtureType type) {
  const laser::Patch patch = state.snapshot(nullptr).settings.patch;
  for (uint8_t i = 0; i < laser::MAX_FIXTURES; ++i) {
    if (patch.slots[i].type == type) {
      return i;
    }
  }
  return laser::NO_FIXTURE;
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
      const laser::Preset& preset = live.presets[command.slot - 1];
      const uint8_t fixture =
          command.fixture != 0 ? static_cast<uint8_t>(command.fixture - 1) : firstFixtureOfType(state, preset.type);
      return ok(state.loadPreset(fixture, command.slot, preset));
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
