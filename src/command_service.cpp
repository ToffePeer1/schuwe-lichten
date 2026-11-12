#include "command_service.h"

#include "library_service.h"
#include "settings_store.h"

namespace command_service {

Result execute(SharedLaserState& state, const laser::Command& command, const laser::Source& source) {
  if (library_service::isLibraryCommand(command.type)) {
    const library_service::Result lib = library_service::execute(state, command);
    laser::Outcome outcome = lib.outcome;
    outcome.error = lib.isSaveFailed ? laser::ErrorCode::None : lib.error;  // isSaveFailed reports it
    return Result{outcome, false, lib.isSaveFailed, outcome.stop, lib.isChanged,
                  command.type == laser::CommandType::LibraryList};
  }
  const laser::Outcome outcome = state.handle(command, source);
  Result result{outcome, false, false, outcome.stop, false, false};
  if (!outcome.hasSettingsToSave) {
    return result;
  }
  // NVS write happens outside the lock; the change is applied only once it is verified.
  if (!settings_store::save(outcome.settingsToSave)) {
    result.isSaveFailed = true;
    return result;
  }
  const laser::Outcome committed = state.commit(outcome.settingsToSave, outcome.commitKind);
  result.isSaved = true;
  if (committed.stop != laser::StopReason::None) {
    result.stop = committed.stop;
  }
  return result;
}

}  // namespace command_service
