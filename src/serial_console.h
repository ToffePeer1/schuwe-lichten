#pragma once

#include "shared_state.h"

// Stage 1 serial commands, one per line:
//   <channel> <value>   e.g. "7 40"   set a channel (1-10) to 0-255
//   0                   every channel to 0
//   off                 CH1 to 0 (laser off), other channels kept
//   hb                  heartbeat, keeps the 1 s fail-safe from tripping
//   ?                   print the current state
// Serial commands feed the same 1 s fail-safe as the web UI.
void serialConsoleBegin();
void serialConsolePoll(SharedLaserState& state);
void printLaserState(const laser::State& state);
