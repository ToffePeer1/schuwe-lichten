#pragma once

#include <stdint.h>

#include "shared_state.h"

// WiFi access point AP_SSID, the phone page (/) and the admin page
// (/admin) on HTTP_PORT, and a WebSocket on WEBSOCKET_PORT speaking the text
// protocol from docs/PROTOCOL.md. Clients leaving does NOT stop the laser (rule 3);
// it only ends a calibration that the leaving client owned.
bool webUiBegin(SharedLaserState* state, bool wasSettingsReset);
void webUiLoop();
void webUiNotifyStop(laser::StopReason reason);

// New limits were just persisted: drop the "settings were reset, calibrate again" warning.
void webUiOnSettingsSaved();

// Phones (WiFi stations) joined to the access point right now.
uint8_t webUiStationCount();
