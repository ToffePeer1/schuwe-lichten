#include "serial_console.h"

#include <Arduino.h>

#include "command_service.h"
#include "config.h"
#include "dmx_output.h"
#include "library_service.h"
#include "web_ui.h"

namespace {

constexpr size_t LINE_BUFFER_SIZE = laser::MAX_LINE_LENGTH + 1;
const laser::Source SERIAL_SOURCE{laser::SourceKind::Serial, 0, true};

char lineBuffer[LINE_BUFFER_SIZE];
size_t lineLength = 0;
bool isDiscardingLine = false;

void printHelp() {
  Serial.println("commands: '<ch> <value>' (e.g. '7 40'), '0' = all zero, 'off' = all off, '?' = status");
  Serial.println("  'f<1-4>' in front picks a fixture: 'f2 7 255', 'f2 off' (no prefix: f1, or all for 0/off)");
  Serial.println("admin (no PIN on USB): cal start|stop, cal look square|diamond|spin,");
  Serial.println("  lim mark low7|high7|left6|right6, lim h on|off, lim save, lim check|redo (optional check),");
  Serial.println("  dbg start|stop (raw laser channels, NO limits), pin <old> <new>, pin reset,");
  Serial.println("  patch <1-4> none|laser <addr>|head <addr> (which light sits at which DMX address)");
  Serial.println("library: preset save <1-32> <name>, preset load|delete <slot>, lib (list),");
  Serial.println("  show save <1-16> <p:t,...> <name> (t in 0.1 s), show play <slot>, show stop, show delete <slot>");
}

void printLibrary() {
  const laser::Library& lib = library_service::library();
  Serial.println("presets:");
  for (size_t i = 0; i < laser::PRESET_SLOTS; ++i) {
    const laser::Preset& p = lib.presets[i];
    if (p.isUsed) {
      Serial.printf("  %2u %s\n", static_cast<unsigned>(i + 1), p.name);
    }
  }
  Serial.println("shows:");
  for (size_t i = 0; i < laser::SHOW_SLOTS; ++i) {
    const laser::Show& s = lib.shows[i];
    if (!s.isUsed) {
      continue;
    }
    Serial.printf("  %2u %s:", static_cast<unsigned>(i + 1), s.name);
    for (uint8_t k = 0; k < s.stepCount; ++k) {
      Serial.printf(" %u(%u.%us)", s.steps[k].preset, s.steps[k].holdTenths / 10, s.steps[k].holdTenths % 10);
    }
    Serial.println();
  }
}

void printMark(const char* name, int16_t value) {
  if (value < 0) {
    Serial.printf(" %s=-", name);
  } else {
    Serial.printf(" %s=%d", name, value);
  }
}

void printCalibration(const laser::CalSession& cal) {
  if (!laser::isActive(cal)) {
    Serial.println("session: off");
    return;
  }
  if (cal.phase == laser::CalPhase::Debug) {
    Serial.print("DEBUG session (no limits):");
    for (size_t i = 0; i < laser::CHANNEL_COUNT; ++i) {
      Serial.printf(" %u=%u", static_cast<unsigned>(i + 1), cal.raw[i]);
    }
    Serial.println();
    return;
  }
  if (cal.phase == laser::CalPhase::Adjust) {
    Serial.printf("calibration: adjust ch7=%u ch6=%u look=%s h=%s", cal.ch7, cal.ch6, laser::lookName(cal.look),
                  cal.isHorizontalOn ? "on" : "off");
    printMark("low7", cal.marks.low7);
    printMark("high7", cal.marks.high7);
    printMark("left6", cal.marks.left6);
    printMark("right6", cal.marks.right6);
    Serial.println();
    return;
  }
  const laser::CheckStep& step = laser::CHECK_STEPS[cal.checkStep % laser::CHECK_STEP_COUNT];
  Serial.printf("calibration: check step %u/%u %s ('lim save' to keep, 'lim redo' to adjust)\n",
                cal.checkStep + 1, static_cast<unsigned>(laser::CHECK_STEP_COUNT), step.name);
}

void reportResult(SharedLaserState& state, const laser::Command& command, const command_service::Result& result) {
  const laser::Outcome& o = result.outcome;
  if (o.error != laser::ErrorCode::None) {
    Serial.printf("error: %s\n", laser::errorCodeName(o.error));
  }
  if (result.isSaveFailed) {
    Serial.println("error: save-failed (nothing changed)");
  }
  if (result.isSaved) {
    Serial.println("saved");
    if (o.commitKind == laser::CommitKind::Limits) {
      webUiOnSettingsSaved();  // the "settings were reset" warning means "calibrate again"
    }
  }
  if (result.stop != laser::StopReason::None) {
    Serial.printf("stopped: %s\n", laser::stopReasonName(result.stop));
    webUiNotifyStop(result.stop);
  }
  if (result.isLibraryChanged) {
    Serial.println("library saved");
  }
  if (result.isLibraryRequested) {
    printLibrary();
  }
  if (command.type == laser::CommandType::Status || result.isSaved) {
    printStatus(state);  // fresh snapshot: a save may already have ended calibration
  } else if (o.error == laser::ErrorCode::None && result.stop == laser::StopReason::None &&
             laser::isActive(o.ctx.cal)) {
    printCalibration(o.ctx.cal);
  } else if (o.error == laser::ErrorCode::None && command.type == laser::CommandType::SetChannel) {
    const uint8_t fixture = command.fixture == 0 ? 1 : command.fixture;
    Serial.printf("ok f%u CH%u=%u\n", fixture, command.channel,
                  o.ctx.fixtures[fixture - 1].channels[command.channel - 1]);
  }
}

void handleLine(SharedLaserState& state, const char* line) {
  if (line[0] == '\0') {
    return;
  }
  const laser::Command command = laser::parseCommand(line);
  if (command.type == laser::CommandType::Invalid) {
    Serial.printf("error: invalid command '%s'\n", line);
    printHelp();
    return;
  }
  reportResult(state, command, command_service::execute(state, command, SERIAL_SOURCE));
}

}  // namespace

void serialConsoleBegin() { printHelp(); }

void printPatch(const laser::Patch& patch) {
  bool isEmpty = true;
  for (uint8_t i = 0; i < laser::MAX_FIXTURES; ++i) {
    const laser::PatchSlot& slot = patch.slots[i];
    if (slot.type != laser::FixtureType::None) {
      Serial.printf("%s f%u %s @%u-%u", isEmpty ? "patch:" : ",", i + 1, laser::fixtureTypeName(slot.type),
                    slot.address, laser::lastAddress(slot));
      isEmpty = false;
    }
  }
  Serial.println(isEmpty ? "patch: empty, nothing is sent" : "");
}

void printStatus(SharedLaserState& state) {
  uint32_t nowMs = 0;
  const laser::Context ctx = state.snapshot(&nowMs);
  const laser::Limits& l = ctx.settings.limits;
  printPatch(ctx.settings.patch);
  for (uint8_t i = 0; i < laser::MAX_FIXTURES; ++i) {
    const uint8_t count = laser::channelCount(ctx.settings.patch.slots[i].type);
    if (count == 0) {
      continue;
    }
    const laser::FixtureChannels frame = laser::fixtureFrame(ctx, i);
    Serial.printf("f%u channels:", i + 1);
    for (uint8_t ch = 0; ch < count; ++ch) {
      Serial.printf(" %u=%u", ch + 1, ctx.fixtures[i].channels[ch]);
    }
    Serial.printf("\nf%u on the wire:", i + 1);
    for (uint8_t ch = 0; ch < count; ++ch) {
      Serial.printf(" %u", frame[ch]);
    }
    Serial.println();
  }
  Serial.printf("laser limits: %s ch7 low=%u high=%u%s, ch6 %s", l.isCalibrated ? "calibrated" : "NOT calibrated",
                l.ch7Low, l.ch7High, l.ch7Low > l.ch7High ? " (inverted)" : "", l.ch6Enabled ? "on" : "off");
  if (l.ch6Enabled) {
    Serial.printf(" %u..%u", l.ch6Left, l.ch6Right);
  }
  Serial.println();
  printCalibration(ctx.cal);
  Serial.printf("stop button (IO0): %s%s\n", ctx.button.isHeld ? "HELD" : "released",
                ctx.button.isArmed ? "" : " (not armed yet)");
  Serial.printf("DMX frames sent: %lu, send failures: %lu, uptime %lu ms\n",
                static_cast<unsigned long>(dmxFramesSent()), static_cast<unsigned long>(dmxSendFailures()),
                static_cast<unsigned long>(nowMs));
  // The serial console runs on the loop task, so nullptr = the loop task.
  Serial.printf("unused stack: loop %lu B, dmx %lu B, free heap %lu B\n",
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned long>(dmxStackFreeBytes()), static_cast<unsigned long>(ESP.getFreeHeap()));
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
