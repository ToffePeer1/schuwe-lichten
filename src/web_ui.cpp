#include "web_ui.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include <cstdarg>
#include <cstring>

#include "command_service.h"
#include "config.h"
#include "library_service.h"
#include "wifi_secrets.h"

// The pages are embedded at build time (board_build.embed_txtfiles, NUL-terminated).
extern const char INDEX_HTML_START[] asm("_binary_src_web_index_html_start");
extern const char INDEX_HTML_END[] asm("_binary_src_web_index_html_end");
extern const char ADMIN_HTML_START[] asm("_binary_src_web_admin_html_start");
extern const char ADMIN_HTML_END[] asm("_binary_src_web_admin_html_end");
extern const char PATTERNS_JSON_START[] asm("_binary_src_web_patterns_json_start");
extern const char PATTERNS_JSON_END[] asm("_binary_src_web_patterns_json_end");

namespace {

constexpr size_t STATE_JSON_SIZE = 2048;  // 4 fixtures of up to 19 channels (~600 B), a session, a show
constexpr size_t LIBRARY_JSON_SIZE = 10240;  // 32 presets + 16 shows of 24 steps, worst case ~8 KB
constexpr size_t EVENT_JSON_SIZE = 128;

constexpr bool isDigitsOnly(const char* s, size_t i = 0) {
  return s[i] == '\0' ? i >= 6 && i <= 8 : s[i] >= '0' && s[i] <= '9' && i < 8 && isDigitsOnly(s, i + 1);
}
constexpr bool isSameText(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || isSameText(a + 1, b + 1));
}
static_assert(sizeof(AP_PASSWORD) - 1 >= 8 && sizeof(AP_PASSWORD) - 1 <= 63, "WPA2 password must be 8-63 characters");
static_assert(!isSameText(AP_PASSWORD, "change-me"), "set your own AP_PASSWORD in include/wifi_secrets.h");
static_assert(isDigitsOnly(ADMIN_PIN), "ADMIN_PIN in include/wifi_secrets.h must be 6-8 digits");

struct ClientSlot {
  bool isConnected;
  bool isAdmin;
  uint32_t lastAdminMs;
};

SharedLaserState* sharedState = nullptr;
bool isSettingsReset = false;
WebServer httpServer(HTTP_PORT);
WebSocketsServer webSocket(WEBSOCKET_PORT);
ClientSlot slots[WEBSOCKETS_SERVER_CLIENT_MAX] = {};
uint32_t lastBroadcastRevision = 0;
uint32_t lastBroadcastMs = 0;
uint32_t lastLibraryRevision = 0;
char libraryJson[LIBRARY_JSON_SIZE];  // loop task only

// Appends with snprintf and remembers if anything didn't fit.
class JsonWriter {
 public:
  JsonWriter(char* buffer, size_t size) : buffer_(buffer), size_(size) { buffer_[0] = '\0'; }
  void add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    if (length_ >= size_) {
      return;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer_ + length_, size_ - length_, format, args);
    va_end(args);
    length_ = written < 0 ? size_ : length_ + static_cast<size_t>(written);
  }
  bool isComplete() const { return length_ < size_; }

 private:
  char* buffer_;
  size_t size_;
  size_t length_ = 0;
};

const char* boolText(bool value) { return value ? "true" : "false"; }

bool isValidSlot(uint8_t num) { return num < WEBSOCKETS_SERVER_CLIENT_MAX; }

void addMark(JsonWriter& json, const char* name, int16_t value, bool isLast) {
  if (value < 0) {
    json.add("\"%s\":null%s", name, isLast ? "" : ",");
  } else {
    json.add("\"%s\":%d%s", name, value, isLast ? "" : ",");
  }
}

const char* phaseName(laser::CalPhase phase) {
  switch (phase) {
    case laser::CalPhase::Check:
      return "check";
    case laser::CalPhase::Debug:
      return "debug";
    default:
      return "adjust";
  }
}

void addCalibration(JsonWriter& json, const laser::Context& ctx, uint8_t num) {
  const laser::CalSession& cal = ctx.cal;
  if (!laser::isActive(cal)) {
    json.add("\"cal\":null");
    return;
  }
  const laser::Source viewer{laser::SourceKind::WebSocket, num, false};
  const laser::CheckStep& step = laser::CHECK_STEPS[cal.checkStep % laser::CHECK_STEP_COUNT];
  json.add("\"cal\":{\"phase\":\"%s\",\"owner\":%s,\"look\":\"%s\",\"ch7\":%u,\"ch6\":%u,\"marks\":{",
           phaseName(cal.phase), boolText(laser::isOwner(cal, viewer)), laser::lookName(cal.look), cal.ch7, cal.ch6);
  addMark(json, "low7", cal.marks.low7, false);
  addMark(json, "high7", cal.marks.high7, false);
  addMark(json, "left6", cal.marks.left6, false);
  addMark(json, "right6", cal.marks.right6, true);
  json.add("},\"hOn\":%s,\"step\":%u,\"steps\":%u,\"stepName\":\"%s\",\"checkDone\":%s,\"raw\":[",
           boolText(cal.isHorizontalOn), cal.checkStep, static_cast<unsigned>(laser::CHECK_STEP_COUNT), step.name,
           boolText(cal.isCheckComplete));
  for (size_t i = 0; i < laser::CHANNEL_COUNT; ++i) {
    json.add(i == 0 ? "%u" : ",%u", cal.raw[i]);
  }
  json.add("]}");
}

// The patched fixtures: slot, type, address, the requested channels and the last loaded preset.
void addFixtures(JsonWriter& json, const laser::Context& ctx) {
  json.add("\"fixtures\":[");
  bool isFirst = true;
  for (uint8_t i = 0; i < laser::MAX_FIXTURES; ++i) {
    const laser::PatchSlot& slot = ctx.settings.patch.slots[i];
    if (slot.type == laser::FixtureType::None) {
      continue;
    }
    json.add("%s{\"slot\":%u,\"type\":\"%s\",\"addr\":%u,\"ch\":[", isFirst ? "" : ",", i + 1,
             laser::fixtureTypeName(slot.type), slot.address);
    for (uint8_t ch = 0; ch < laser::channelCount(slot.type); ++ch) {
      json.add(ch == 0 ? "%u" : ",%u", ctx.fixtures[i].channels[ch]);
    }
    const uint8_t preset = ctx.fixtures[i].loadedPreset;
    if (preset == 0) {
      json.add("],\"preset\":null}");
    } else {
      json.add("],\"preset\":%u}", preset);
    }
    isFirst = false;
  }
  json.add("]");
}

bool buildStateJson(char* out, size_t size, const laser::Context& ctx, uint32_t nowMs, uint8_t num) {
  const laser::Limits& l = ctx.settings.limits;
  const bool isAdmin = isValidSlot(num) && slots[num].isAdmin;
  JsonWriter json(out, size);
  json.add("{\"type\":\"state\",");
  addFixtures(json, ctx);
  json.add(",\"limits\":{\"ch7\":[%u,%u],\"ch6\":{\"on\":%s,\"range\":[%u,%u]},\"calibrated\":%s,\"allowAuto\":false},",
           l.ch7Low, l.ch7High, boolText(l.ch6Enabled), l.ch6Left, l.ch6Right, boolText(l.isCalibrated));
  json.add("\"stopHeld\":%s,\"admin\":%s,", boolText(ctx.button.isHeld), boolText(isAdmin));
  addCalibration(json, ctx, num);
  const laser::Playback& play = ctx.playback;
  if (!play.isPlaying) {
    json.add(",\"play\":null");
  } else {
    json.add(",\"play\":{\"show\":%u,\"step\":%u,\"steps\":%u,\"holdLeftMs\":%lu}", play.showSlot, play.step,
             play.show.stepCount, static_cast<unsigned long>(laser::holdLeftMs(ctx, nowMs)));
  }
  json.add(",\"settingsReset\":%s,\"libraryReset\":%s}", boolText(isSettingsReset),
           boolText(library_service::wasReset()));
  return json.isComplete();
}

void sendState(uint8_t num) {
  uint32_t nowMs = 0;
  const laser::Context ctx = sharedState->snapshot(&nowMs);
  char json[STATE_JSON_SIZE];
  if (buildStateJson(json, sizeof(json), ctx, nowMs, num)) {
    webSocket.sendTXT(num, json);
  }
}

void broadcastState() {
  uint32_t nowMs = 0;
  const laser::Context ctx = sharedState->snapshot(&nowMs);
  char json[STATE_JSON_SIZE];
  for (uint8_t num = 0; num < WEBSOCKETS_SERVER_CLIENT_MAX; ++num) {
    if (slots[num].isConnected && buildStateJson(json, sizeof(json), ctx, nowMs, num)) {
      webSocket.sendTXT(num, json);
    }
  }
}

// Names are validated (no '"', '\\' or control characters, well-formed UTF-8),
// so they can go into the JSON as they are.
bool buildLibraryJson() {
  const laser::Library& lib = library_service::library();
  JsonWriter json(libraryJson, sizeof(libraryJson));
  json.add("{\"type\":\"library\",\"presets\":[");
  bool isFirst = true;
  for (size_t i = 0; i < laser::PRESET_SLOTS; ++i) {
    const laser::Preset& preset = lib.presets[i];
    if (!preset.isUsed) {
      continue;
    }
    json.add("%s{\"slot\":%u,\"name\":\"%s\",\"ch\":[", isFirst ? "" : ",", static_cast<unsigned>(i + 1),
             preset.name);
    for (size_t c = 0; c < laser::CHANNEL_COUNT; ++c) {
      json.add(c == 0 ? "%u" : ",%u", preset.channels[c]);
    }
    json.add("]}");
    isFirst = false;
  }
  json.add("],\"shows\":[");
  isFirst = true;
  for (size_t i = 0; i < laser::SHOW_SLOTS; ++i) {
    const laser::Show& show = lib.shows[i];
    if (!show.isUsed) {
      continue;
    }
    json.add("%s{\"slot\":%u,\"name\":\"%s\",\"steps\":[", isFirst ? "" : ",", static_cast<unsigned>(i + 1),
             show.name);
    for (uint8_t k = 0; k < show.stepCount; ++k) {
      json.add("%s[%u,%u]", k == 0 ? "" : ",", show.steps[k].preset, show.steps[k].holdTenths);
    }
    json.add("]}");
    isFirst = false;
  }
  json.add("]}");
  return json.isComplete();
}

// Worst case (every slot full, longest names) is ~8 KB in one frame, more than
// the TCP send buffer (~5.7 KB), so the write waits for the phone's ACKs. It is
// only sent on connect and after a library change, so that is accepted.
void sendLibrary(int16_t num) {
  if (!buildLibraryJson()) {
    Serial.println("ERROR: library JSON does not fit its buffer; not sent");
    return;
  }
  if (num < 0) {
    webSocket.broadcastTXT(libraryJson);
  } else {
    webSocket.sendTXT(static_cast<uint8_t>(num), libraryJson);
  }
}

void sendError(uint8_t num, laser::ErrorCode code) {
  char json[EVENT_JSON_SIZE];
  snprintf(json, sizeof(json), "{\"type\":\"error\",\"code\":\"%s\"}", laser::errorCodeName(code));
  webSocket.sendTXT(num, json);
}

void sendAuthEvent(uint8_t num, const laser::Outcome& o) {
  char json[EVENT_JSON_SIZE];
  snprintf(json, sizeof(json), "{\"type\":\"auth\",\"ok\":%s,\"lockedSeconds\":%lu,\"attemptsLeft\":%u}",
           boolText(o.auth == laser::AuthEvent::Granted), static_cast<unsigned long>((o.lockedMs + 999) / 1000),
           o.attemptsLeft);
  webSocket.sendTXT(num, json);
}

void trackAdmin(uint8_t num, const laser::Command& command, const laser::Outcome& o, uint32_t nowMs) {
  ClientSlot& slot = slots[num];
  if (o.auth == laser::AuthEvent::Granted) {
    slot.isAdmin = true;
    slot.lastAdminMs = nowMs;
    Serial.printf("admin: client %u logged in from %s\n", num, webSocket.remoteIP(num).toString().c_str());
  } else if (o.auth != laser::AuthEvent::None) {
    Serial.printf("admin: wrong PIN from client %u (%s)%s\n", num, webSocket.remoteIP(num).toString().c_str(),
                  o.auth == laser::AuthEvent::Locked ? ", locked" : "");
  }
  // Working in (or just keeping open) your own calibration/debug session is
  // admin activity, so the 10 min admin expiry never cuts off a session whose
  // owner is still on the page.
  const laser::Source viewer{laser::SourceKind::WebSocket, num, slot.isAdmin};
  const bool isSessionOwner = laser::isOwner(o.ctx.cal, viewer);
  const bool isSessionActivity = isSessionOwner && o.error == laser::ErrorCode::None &&
                                 (command.type == laser::CommandType::SetChannel ||
                                  command.type == laser::CommandType::Heartbeat);
  if (o.isLogout) {
    slot.isAdmin = false;
  } else if (slot.isAdmin && (laser::isAdminCommand(command.type) || isSessionActivity)) {
    slot.lastAdminMs = nowMs;
  }
}

void handleText(uint8_t num, const uint8_t* payload, size_t length) {
  if (length > laser::MAX_LINE_LENGTH) {
    sendError(num, laser::ErrorCode::BadCommand);
    return;
  }
  char line[laser::MAX_LINE_LENGTH + 1];
  std::memcpy(line, payload, length);
  line[length] = '\0';
  const laser::Command command = laser::parseCommand(line);
  if (command.type == laser::CommandType::Invalid) {
    sendError(num, laser::ErrorCode::BadCommand);
    return;
  }
  const laser::Source source{laser::SourceKind::WebSocket, num, slots[num].isAdmin};
  const command_service::Result result = command_service::execute(*sharedState, command, source);
  const laser::Outcome& o = result.outcome;
  trackAdmin(num, command, o, millis());
  if (o.auth != laser::AuthEvent::None) {
    sendAuthEvent(num, o);
  } else if (o.error != laser::ErrorCode::None) {
    sendError(num, o.error);
  }
  if (result.isSaveFailed) {
    sendError(num, laser::ErrorCode::SaveFailed);
  }
  if (result.isSaved) {
    if (o.commitKind == laser::CommitKind::Limits) {
      webUiOnSettingsSaved();  // the "settings were reset" warning means "calibrate again"
    }
    webSocket.sendTXT(num, "{\"type\":\"saved\"}");
  }
  if (result.stop != laser::StopReason::None) {
    webUiNotifyStop(result.stop);
  }
  if (result.isLibraryRequested) {
    sendLibrary(num);
  }
  if (command.type == laser::CommandType::Status || o.auth != laser::AuthEvent::None || o.isLogout) {
    sendState(num);  // admin flag changes are per client, not a revision change
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (!isValidSlot(num)) {
    return;
  }
  switch (type) {
    case WStype_CONNECTED:
      slots[num] = ClientSlot{true, false, 0};
      sendState(num);
      sendLibrary(num);
      break;
    case WStype_DISCONNECTED: {
      slots[num] = ClientSlot{false, false, 0};
      const laser::Outcome left = sharedState->clientLeft(num);
      if (left.stop != laser::StopReason::None) {
        webUiNotifyStop(left.stop);
      }
      break;
    }
    case WStype_TEXT:
      handleText(num, payload, length);
      break;
    default:
      break;
  }
}

void expireIdleAdmins(uint32_t nowMs) {
  for (uint8_t num = 0; num < WEBSOCKETS_SERVER_CLIENT_MAX; ++num) {
    ClientSlot& slot = slots[num];
    if (slot.isAdmin && nowMs - slot.lastAdminMs >= ADMIN_IDLE_TIMEOUT_MS) {
      slot.isAdmin = false;
      // A calibration must not outlive its owner's admin session.
      const laser::Outcome left = sharedState->clientLeft(num);
      if (left.stop != laser::StopReason::None) {
        webUiNotifyStop(left.stop);
      }
      sendState(num);
    }
  }
}

void sendPage(const char* start, const char* end) {
  const size_t length = static_cast<size_t>(end - start) - 1;  // drop the NUL
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send_P(200, "text/html", start, length);
}

}  // namespace

bool webUiBegin(SharedLaserState* state, bool wasSettingsReset) {
  sharedState = state;
  isSettingsReset = wasSettingsReset;
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONNECTIONS)) {
    return false;
  }
  httpServer.on("/", HTTP_GET, []() { sendPage(INDEX_HTML_START, INDEX_HTML_END); });
  httpServer.on("/admin", HTTP_GET, []() { sendPage(ADMIN_HTML_START, ADMIN_HTML_END); });
  httpServer.on("/patterns.json", HTTP_GET, []() {
    const size_t length = static_cast<size_t>(PATTERNS_JSON_END - PATTERNS_JSON_START) - 1;  // drop the NUL
    httpServer.send_P(200, "application/json", PATTERNS_JSON_START, length);
  });
  httpServer.onNotFound([]() { httpServer.send(404, "text/plain", "not found"); });
  httpServer.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  // Library ping/pong: detects phones that vanished without closing the socket.
  webSocket.enableHeartbeat(2000, 1000, 2);

  Serial.printf("wifi: AP '%s' up, open http://%s/ (admin: /admin)\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  return true;
}

uint8_t webUiStationCount() { return WiFi.softAPgetStationNum(); }

void webUiLoop() {
  httpServer.handleClient();
  webSocket.loop();

  const uint32_t nowMs = millis();
  expireIdleAdmins(nowMs);
  if (library_service::revision() != lastLibraryRevision) {
    lastLibraryRevision = library_service::revision();
    sendLibrary(-1);  // presets/shows changed: everyone gets the new library
  }
  const uint32_t revision = sharedState->revision();
  const laser::Context current = sharedState->snapshot(nullptr);
  const bool isTicking = laser::isActive(current.cal) || current.playback.isPlaying;
  const bool isChanged = revision != lastBroadcastRevision;
  const bool isCountdownDue = isTicking && nowMs - lastBroadcastMs >= CALIBRATION_BROADCAST_INTERVAL_MS;
  if ((isChanged && nowMs - lastBroadcastMs >= STATE_BROADCAST_MIN_INTERVAL_MS) || isCountdownDue) {
    lastBroadcastRevision = revision;
    lastBroadcastMs = nowMs;
    broadcastState();
  }
}

void webUiOnSettingsSaved() { isSettingsReset = false; }

void webUiNotifyStop(laser::StopReason reason) {
  char json[EVENT_JSON_SIZE];
  snprintf(json, sizeof(json), "{\"type\":\"stop\",\"reason\":\"%s\"}", laser::stopReasonName(reason));
  webSocket.broadcastTXT(json);
}
