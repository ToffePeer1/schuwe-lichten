#pragma once

#include <stdint.h>

#include "shared_state.h"

// WiFi access point "Laser-Controller", the phone page on HTTP_PORT and a
// WebSocket on WEBSOCKET_PORT. The WebSocket speaks the same text commands as
// the serial console; the server answers with JSON state messages.
// A phone leaving the AP or a WebSocket closing triggers the fail-safe.
bool webUiBegin(SharedLaserState* state);
void webUiLoop();
void webUiNotifyFailsafe(FailsafeReason reason);
