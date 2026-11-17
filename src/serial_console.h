#pragma once

#include "shared_state.h"

// USB serial console. Same commands as the WebSocket (see docs/PROTOCOL.md); USB means
// physical access, so it is always admin (no PIN) and can run "pin reset".
void serialConsoleBegin();
void serialConsolePoll(SharedLaserState& state);
void printStatus(SharedLaserState& state);

// One line per patched fixture: "f2 head @11-29".
void printPatch(const laser::Patch& patch);
