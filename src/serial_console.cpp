#include "serial_console.h"

#include <Arduino.h>

#include "config.h"
#include "dmx_output.h"

namespace {

constexpr size_t LINE_BUFFER_SIZE = 48;

char lineBuffer[LINE_BUFFER_SIZE];
size_t lineLength = 0;
bool isDiscardingLine = false;

void printHelp() {
  Serial.println("commands: '<ch> <value>' (e.g. '7 40'), '0' = all zero, 'off' = CH1 0, "
                 "'hb' = heartbeat, '?' = status");
  Serial.printf("note: no valid command for %lu ms -> fail-safe sets everything to 0\n",
                static_cast<unsigned long>(FAILSAFE_TIMEOUT_MS));
}

void reportCommand(const laser::Command& command, const laser::State& state) {
  switch (command.type) {
    case laser::CommandType::SetChannel: {
      const uint8_t applied = state.channels[command.channel - 1];
      Serial.printf("ok CH%u=%u", command.channel, applied);
      if (applied != command.value) {
        Serial.printf(" (clamped from %u)", command.value);
      }
      Serial.println();
      break;
    }
    case laser::CommandType::Blackout:
      Serial.println("ok all channels 0");
      break;
    case laser::CommandType::LaserOff:
      Serial.println("ok CH1=0 (laser off)");
      break;
    case laser::CommandType::Status:
      printLaserState(state);
      break;
    case laser::CommandType::Heartbeat:
    case laser::CommandType::Invalid:
      break;
  }
}

void handleLine(SharedLaserState& state, const char* line) {
  const laser::Command command = laser::parseCommand(line);
  if (command.type == laser::CommandType::Invalid) {
    if (line[0] != '\0') {
      Serial.printf("error: invalid command '%s'\n", line);
      printHelp();
    }
    return;
  }
  const laser::State result = command.type == laser::CommandType::Status
                                  ? state.snapshot()
                                  : state.apply(command);
  reportCommand(command, result);
}

}  // namespace

void serialConsoleBegin() { printHelp(); }

void printLaserState(const laser::State& state) {
  Serial.print("channels:");
  for (size_t i = 0; i < laser::CHANNEL_COUNT; ++i) {
    Serial.printf(" %u=%u", static_cast<unsigned>(i + 1), state.channels[i]);
  }
  Serial.printf("\nDMX frames sent: %lu, send failures: %lu, uptime %lu ms\n",
                static_cast<unsigned long>(dmxFramesSent()),
                static_cast<unsigned long>(dmxSendFailures()),
                static_cast<unsigned long>(millis()));
}

void serialConsolePoll(SharedLaserState& state) {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (isDiscardingLine) {
        Serial.println("error: line too long");
      } else {
        lineBuffer[lineLength] = '\0';
        handleLine(state, lineBuffer);
      }
      lineLength = 0;
      isDiscardingLine = false;
      continue;
    }
    if (lineLength >= LINE_BUFFER_SIZE - 1) {
      isDiscardingLine = true;
      continue;
    }
    lineBuffer[lineLength++] = c;
  }
}
