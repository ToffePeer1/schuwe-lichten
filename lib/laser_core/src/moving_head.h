#pragma once

// The 19-channel "bee eye" moving head, after its manual (not yet checked on
// the real fixture). It gets no limits (user decision): its lasers are weak,
// and its own random-walk and sound modes are allowed. "Off" only needs to
// know which channels make light.

#include <cstdint>

#include "fixtures.h"

namespace laser {

constexpr uint8_t HEAD_CHANNEL_COUNT = 19;
constexpr uint8_t HEAD_CH_PAN = 1;          // 0-255 = 0-540 degrees
constexpr uint8_t HEAD_CH_PAN_SPEED = 2;    // fast to slow
constexpr uint8_t HEAD_CH_EYE_MOTOR = 3;    // 0-127 angle, 128-191 turns back, 192-255 turns forward
constexpr uint8_t HEAD_CH_MOTOR_1 = 4;      // 4-6: 0-180 degrees
constexpr uint8_t HEAD_CH_MOTOR_3 = 6;
constexpr uint8_t HEAD_CH_DIMMER = 7;       // master dimmer
constexpr uint8_t HEAD_CH_STROBE = 8;       // 0-9 none, 10-255 = 1-25 Hz
constexpr uint8_t HEAD_CH_EYE_RED = 9;      // 9-12 bee eye red, green, blue, white
constexpr uint8_t HEAD_CH_BEAM_RED = 13;    // 13-16 wash + beam red, green, blue, white
constexpr uint8_t HEAD_CH_LASER_RED = 17;
constexpr uint8_t HEAD_CH_LASER_GREEN = 18;
constexpr uint8_t HEAD_CH_MACRO = 19;       // 0-50 DMX, 51-150 random walk, 151-250 sound, 251-255 reset

static_assert(HEAD_CHANNEL_COUNT <= MAX_FIXTURE_CHANNELS, "the head must fit a fixture's channel array");

// Dark but in place: every light channel to 0, and the macro to 0 so the head
// follows DMX again instead of its own program. Pan, tilt motors and speeds stay.
FixtureChannels headOff(const FixtureChannels& channels);

}  // namespace laser
