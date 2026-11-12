#pragma once

// Onboard blue LED: a short flash once a second while the WiFi is up but no
// phone has joined it, so you can see the firmware is running. Off as soon as
// a phone is on the WiFi (the user doesn't want a light during use). A missing
// DMX cable can't be shown: the VMA432 only transmits.

#include <cstdint>

namespace laser {

constexpr uint32_t STATUS_LED_PERIOD_MS = 1000;
constexpr uint32_t STATUS_LED_ON_MS = 150;

bool isStatusLedOn(bool isWifiUp, uint8_t stationCount, uint32_t nowMs);

}  // namespace laser
