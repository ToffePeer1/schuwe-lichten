#include "dmx_output.h"

#include <Arduino.h>
#include <esp_dmx.h>
#include <esp_rom_gpio.h>
#include <esp_task_wdt.h>
#include <soc/gpio_pins.h>
#include <soc/uart_periph.h>

#include <atomic>
#include <cstring>

#include "config.h"

namespace {

constexpr dmx_port_t DMX_PORT = static_cast<dmx_port_t>(DMX_UART_NUM);
constexpr uint32_t DMX_TASK_STACK_BYTES = 6144;  // the tick copies the Context (incl. a running show)
constexpr UBaseType_t DMX_TASK_PRIORITY = 3;  // above the Arduino loop task (1)
constexpr BaseType_t DMX_TASK_CORE = 1;       // same core as the driver ISR; WiFi runs on core 0

static_assert(laser::DMX_ADDRESS_MAX + laser::CHANNEL_COUNT - 1 < DMX_PACKET_SIZE,
              "the laser footprint must fit in one DMX universe at every address");

std::atomic<uint32_t> framesSent{0};
std::atomic<uint32_t> sendFailures{0};
TaskHandle_t dmxTaskHandle = nullptr;

void dmxTask(void* arg) {
  auto* state = static_cast<SharedLaserState*>(arg);
  esp_task_wdt_add(nullptr);  // a hung DMX task reboots into the boot blackout
  static uint8_t packet[DMX_PACKET_SIZE];
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const bool isStopLevelLow = digitalRead(PIN_STOP_BUTTON) == LOW;
    const FrameOutput frame = state->frameTick(isStopLevelLow);
    // Rebuilt from zeros every frame: slot 0 = start code, every other slot dark.
    std::memset(packet, 0, sizeof(packet));
    if (frame.dmxAddress >= laser::DMX_ADDRESS_MIN && frame.dmxAddress <= laser::DMX_ADDRESS_MAX) {
      std::memcpy(&packet[frame.dmxAddress], frame.channels.data(), frame.channels.size());
    }
    dmx_write(DMX_PORT, packet, DMX_PACKET_SIZE);
    const bool isSent = dmx_send_num(DMX_PORT, DMX_PACKET_SIZE) == DMX_PACKET_SIZE &&
                        dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);
    (isSent ? framesSent : sendFailures).fetch_add(1, std::memory_order_relaxed);
    esp_task_wdt_reset();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DMX_FRAME_PERIOD_MS));
  }
}

// TX-only hardware: no RX pin is assigned, but esp_dmx still enables receive
// interrupts between frames. Tie the UART's RX input to a constant idle-high
// level inside the GPIO matrix so a floating input can never cause an
// interrupt storm of breaks or framing errors.
void tieRxInputIdle() {
  esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                 UART_PERIPH_SIGNAL(DMX_UART_NUM, SOC_UART_RX_PIN_IDX), false);
}

}  // namespace

bool dmxOutputBegin(SharedLaserState* state) {
  pinMode(PIN_STOP_BUTTON, INPUT_PULLUP);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  if (!dmx_driver_install(DMX_PORT, &config, nullptr, 0)) {
    return false;
  }
  if (!dmx_set_pin(DMX_PORT, PIN_DMX_TX, DMX_PIN_NO_CHANGE, DMX_PIN_NO_CHANGE)) {
    return false;
  }
  tieRxInputIdle();
  return xTaskCreatePinnedToCore(dmxTask, "dmx_tx", DMX_TASK_STACK_BYTES, state,
                                 DMX_TASK_PRIORITY, &dmxTaskHandle, DMX_TASK_CORE) == pdPASS;
}

uint32_t dmxFramesSent() { return framesSent.load(std::memory_order_relaxed); }

uint32_t dmxSendFailures() { return sendFailures.load(std::memory_order_relaxed); }

uint32_t dmxStackFreeBytes() {
  return dmxTaskHandle == nullptr ? 0 : uxTaskGetStackHighWaterMark(dmxTaskHandle);  // bytes on ESP-IDF
}
