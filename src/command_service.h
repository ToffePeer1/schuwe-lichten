#pragma once

#include "shared_state.h"

// Runs one command end to end for the serial console and the WebSocket:
// controller -> (if needed) verified NVS save -> commit. Call from the loop task only.
namespace command_service {

struct Result {
  laser::Outcome outcome;  // from the controller (errors, auth, logout)
  bool isSaved;            // settings were persisted, verified and applied
  bool isSaveFailed;       // persisting failed: nothing was applied
  laser::StopReason stop;  // a stop from the command or from the commit
  bool isLibraryChanged;   // presets/shows were edited and persisted (broadcast the library)
  bool isLibraryRequested; // "lib": send the library to this client
};

Result execute(SharedLaserState& state, const laser::Command& command, const laser::Source& source);

}  // namespace command_service
