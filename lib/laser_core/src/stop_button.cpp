#include "stop_button.h"

namespace laser {

ButtonUpdate updateStopButton(const StopButton& button, bool isLevelLow) {
  StopButton next = button;
  if (!button.isArmed) {
    next.isArmed = !isLevelLow;
    return {next, ButtonEvent::None};
  }
  if (!isLevelLow) {
    next.lowSamples = 0;
    next.isHeld = false;
    return {next, button.isHeld ? ButtonEvent::Released : ButtonEvent::None};
  }
  if (next.lowSamples < STOP_DEBOUNCE_SAMPLES) {
    next.lowSamples = static_cast<uint8_t>(next.lowSamples + 1);
  }
  if (!button.isHeld && next.lowSamples >= STOP_DEBOUNCE_SAMPLES) {
    next.isHeld = true;
    return {next, ButtonEvent::Pressed};
  }
  return {next, ButtonEvent::None};
}

}  // namespace laser
