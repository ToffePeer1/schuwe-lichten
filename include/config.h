#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
constexpr uint32_t SERIAL_BAUD = 115200;

// ---------------------------------------------------------------------------
// Pin map (wiring table in README.md)
// ---------------------------------------------------------------------------
// Onboard blue LED (active high). Flashes only while no phone is on the WiFi, see status_led.h.
constexpr uint8_t PIN_STATUS_LED = 2;

// VMA432 SIG input (UART2 TX pin on the classic ESP32).
constexpr uint8_t PIN_DMX_TX = 17;

// IO0 / BOOT button = stop button (active low, board pull-up). Strapping pin:
// it is only read after boot and only armed once it has been seen released.
constexpr uint8_t PIN_STOP_BUTTON = 0;

// ---------------------------------------------------------------------------
// DMX output
// ---------------------------------------------------------------------------
// UART1, routed to GPIO17 through the GPIO matrix. UART2 is avoided because
// esp_dmx 4.1.0 crashes on DMX_NUM_2 (issue #150, unreleased fix).
constexpr int DMX_UART_NUM = 1;
constexpr uint16_t DEFAULT_DMX_ADDRESS = 1;  // runtime setting in /admin
constexpr uint32_t DMX_FRAME_PERIOD_MS = 25;  // 40 frames per second

// ---------------------------------------------------------------------------
// Safety (limits are runtime settings; see lib/laser_core and docs/SAFETY.md)
// ---------------------------------------------------------------------------
constexpr uint32_t STOP_BUTTON_STUCK_WARNING_MS = 1000;
constexpr uint32_t ADMIN_IDLE_TIMEOUT_MS = 10UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// WiFi access point + web UI
// ---------------------------------------------------------------------------
constexpr char AP_SSID[] = "Schuwe Lichtoplossingen";
constexpr int AP_CHANNEL = 1;
constexpr int AP_MAX_CONNECTIONS = 8;
constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t WEBSOCKET_PORT = 81;
constexpr uint32_t STATE_BROADCAST_MIN_INTERVAL_MS = 50;
constexpr uint32_t CALIBRATION_BROADCAST_INTERVAL_MS = 1000;  // countdown refresh
