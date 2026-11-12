#pragma once

#include "library.h"
#include "shared_state.h"

// Presets and shows (stage 3 step 2). Loop task only: the library is touched
// by nobody else; the DMX task only ever sees the copied-in ActiveShow.
namespace library_service {

struct Result {
  laser::Outcome outcome;  // controller result (errors, stops) for load/play/stop
  laser::ErrorCode error;  // library or controller error, None on success
  bool isChanged;          // the library was edited and persisted
  bool isSaveFailed;       // persisting failed: nothing changed
};

void begin(const laser::Library& loaded, bool wasPresetsReset, bool wasShowsReset);
const laser::Library& library();
uint32_t revision();  // bumps on every persisted change
bool wasReset();      // a file loaded as empty (corrupt) and was not saved since

bool isLibraryCommand(laser::CommandType type);
Result execute(SharedLaserState& state, const laser::Command& command);

}  // namespace library_service
