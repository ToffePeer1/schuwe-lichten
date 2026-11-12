#pragma once

// Copy this file to include/wifi_secrets.h (gitignored) and fill in both values.
// The placeholders below fail the build on purpose, so no device ships with a
// public password or PIN.
// AP_PASSWORD: WPA2 needs 8-63 characters.
// ADMIN_PIN: factory PIN for /admin, 6-8 digits (change it in /admin later).
constexpr char AP_PASSWORD[] = "change-me";
constexpr char ADMIN_PIN[] = "change-me";
