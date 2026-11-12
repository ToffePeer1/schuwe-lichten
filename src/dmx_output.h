#pragma once

#include <stdint.h>

#include "shared_state.h"

// Installs the esp_dmx driver (TX only, no RTS/RX), configures the IO0 stop
// button, and starts a task that sends a full DMX frame every
// DMX_FRAME_PERIOD_MS, whether or not anything changed. The task also polls
// the stop button and is watched by the task watchdog.
// Returns false if the driver could not be installed.
bool dmxOutputBegin(SharedLaserState* state);

uint32_t dmxFramesSent();

// Frames the driver did not accept or did not finish in time.
uint32_t dmxSendFailures();

// The DMX task's smallest unused stack so far, in bytes (0 before it started).
uint32_t dmxStackFreeBytes();
