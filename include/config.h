#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
constexpr uint32_t SERIAL_BAUD = 115200;

// ---------------------------------------------------------------------------
// Pin map (see CLAUDE.md)
// ---------------------------------------------------------------------------
// Onboard blue LED. Held off (the user does not want a blinking LED).
constexpr uint8_t PIN_STATUS_LED = 2;

// VMA432 SIG input (UART2 TX pin on the classic ESP32).
constexpr uint8_t PIN_DMX_TX = 17;

// ---------------------------------------------------------------------------
// DMX output
// ---------------------------------------------------------------------------
// UART1, routed to GPIO17 through the GPIO matrix. UART2 is avoided because
// esp_dmx 4.1.0 crashes on DMX_NUM_2 (issue #150, unreleased fix).
constexpr int DMX_UART_NUM = 1;
constexpr uint16_t LASER_DMX_ADDRESS = 1;
constexpr uint32_t DMX_FRAME_PERIOD_MS = 25;  // 40 frames per second

// ---------------------------------------------------------------------------
// Safety
// ---------------------------------------------------------------------------
// Fail-safe: everything to 0 when no valid command arrived for this long.
constexpr uint32_t FAILSAFE_TIMEOUT_MS = 1000;

// CH7 (vertical position) limits, enforced in firmware.
// TODO(stage 1): replace with the values measured in the physical laser test,
// then set CH7_LIMITS_MEASURED to true. Until then, these are placeholders that
// only block the 128-255 "movement speed" range (an automatic vertical sweep).
constexpr uint8_t CH7_MIN = 0;    // TODO(stage 1): measured minimum
constexpr uint8_t CH7_MAX = 127;  // TODO(stage 1): measured maximum
constexpr bool CH7_LIMITS_MEASURED = false;

static_assert(CH7_MIN <= CH7_MAX, "CH7_MIN must not exceed CH7_MAX");
static_assert(CH7_MAX <= 127, "CH7 128-255 is movement speed (vertical sweep), never allow it");

// The clamp can only be disabled by the explicit build flag used in the
// [env:findlimit] build (stage 1 test only).
#if defined(LASER_CH7_CLAMP_DISABLED) && LASER_CH7_CLAMP_DISABLED
constexpr bool CH7_CLAMP_ENABLED = false;
#else
constexpr bool CH7_CLAMP_ENABLED = true;
#endif

// ---------------------------------------------------------------------------
// WiFi access point + web UI
// ---------------------------------------------------------------------------
constexpr char AP_SSID[] = "Laser-Controller";
constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t WEBSOCKET_PORT = 81;
constexpr uint32_t STATE_BROADCAST_MIN_INTERVAL_MS = 50;
