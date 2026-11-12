#pragma once

#include "shared_state.h"

// USB serial console. Same commands as the WebSocket (see CLAUDE.md); USB means
// physical access, so it is always admin (no PIN) and can run "pin reset".
void serialConsoleBegin();
void serialConsolePoll(SharedLaserState& state);
void printLaserStatus(SharedLaserState& state);
