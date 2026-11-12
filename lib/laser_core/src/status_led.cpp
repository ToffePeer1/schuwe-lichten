#include "status_led.h"

namespace laser {

bool isStatusLedOn(bool isWifiUp, uint8_t stationCount, uint32_t nowMs) {
  if (!isWifiUp || stationCount > 0) {
    return false;
  }
  return nowMs % STATUS_LED_PERIOD_MS < STATUS_LED_ON_MS;
}

}  // namespace laser
