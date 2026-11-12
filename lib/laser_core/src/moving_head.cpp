#include "moving_head.h"

namespace laser {

FixtureChannels headOff(const FixtureChannels& channels) {
  FixtureChannels next = channels;
  for (uint8_t ch = HEAD_CH_DIMMER; ch <= HEAD_CH_MACRO; ++ch) {
    next[ch - 1] = 0;
  }
  return next;
}

}  // namespace laser
