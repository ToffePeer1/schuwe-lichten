#pragma once

// IO0 stop button: debounced, level-held and armed only after it has been
// seen released once after boot (the pin is also the BOOT strapping pin).

#include <cstdint>

namespace laser {

constexpr uint8_t STOP_DEBOUNCE_SAMPLES = 2;  // consecutive low reads (one per DMX frame)

struct StopButton {
  bool isArmed = false;
  bool isHeld = false;
  uint8_t lowSamples = 0;
};

enum class ButtonEvent : uint8_t { None, Pressed, Released };

struct ButtonUpdate {
  StopButton button;
  ButtonEvent event;
};

ButtonUpdate updateStopButton(const StopButton& button, bool isLevelLow);

}  // namespace laser
