#include "web_ui.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include <cstring>

#include "config.h"
#include "wifi_secrets.h"

// The page is embedded at build time (board_build.embed_txtfiles, NUL-terminated).
extern const char INDEX_HTML_START[] asm("_binary_src_web_index_html_start");
extern const char INDEX_HTML_END[] asm("_binary_src_web_index_html_end");

namespace {

constexpr size_t MAX_MESSAGE_LENGTH = 32;
constexpr size_t STATE_JSON_SIZE = 256;
constexpr uint32_t WS_PING_INTERVAL_MS = 2000;
constexpr uint32_t WS_PONG_TIMEOUT_MS = 1000;
constexpr uint8_t WS_MISSED_PONGS_BEFORE_DISCONNECT = 2;

static_assert(sizeof(AP_PASSWORD) - 1 >= 8 && sizeof(AP_PASSWORD) - 1 <= 63,
              "WPA2 password must be 8-63 characters");

SharedLaserState* sharedState = nullptr;
WebServer httpServer(HTTP_PORT);
WebSocketsServer webSocket(WEBSOCKET_PORT);
uint32_t lastBroadcastRevision = 0;
uint32_t lastBroadcastMs = 0;
// Only clients that completed the WebSocket handshake trip the fail-safe on
// disconnect (a plain HTTP probe on the WebSocket port should not).
bool isClientConnected[WEBSOCKETS_SERVER_CLIENT_MAX] = {};

void buildStateJson(char* out, size_t size) {
  const laser::State state = sharedState->snapshot();
  const laser::Limits& limits = sharedState->limits();
  int written = snprintf(out, size, "{\"type\":\"state\",\"ch\":[");
  for (size_t i = 0; i < laser::CHANNEL_COUNT && written > 0 && static_cast<size_t>(written) < size; ++i) {
    written += snprintf(out + written, size - written, i == 0 ? "%u" : ",%u", state.channels[i]);
  }
  if (written > 0 && static_cast<size_t>(written) < size) {
    snprintf(out + written, size - written,
             "],\"ch7Min\":%u,\"ch7Max\":%u,\"clamp\":%s,\"measured\":%s,\"timeoutMs\":%lu}",
             limits.ch7Min, limits.ch7Max, limits.isCh7ClampEnabled ? "true" : "false",
             CH7_LIMITS_MEASURED ? "true" : "false",
             static_cast<unsigned long>(FAILSAFE_TIMEOUT_MS));
  }
}

void sendState(int8_t clientNum) {
  char json[STATE_JSON_SIZE];
  buildStateJson(json, sizeof(json));
  if (clientNum < 0) {
    webSocket.broadcastTXT(json);
  } else {
    webSocket.sendTXT(static_cast<uint8_t>(clientNum), json);
  }
}

void handleText(uint8_t clientNum, const uint8_t* payload, size_t length) {
  if (length > MAX_MESSAGE_LENGTH) {
    webSocket.sendTXT(clientNum, "{\"type\":\"error\",\"msg\":\"message too long\"}");
    return;
  }
  char line[MAX_MESSAGE_LENGTH + 1];
  std::memcpy(line, payload, length);
  line[length] = '\0';
  const laser::Command command = laser::parseCommand(line);
  if (command.type == laser::CommandType::Invalid) {
    webSocket.sendTXT(clientNum, "{\"type\":\"error\",\"msg\":\"invalid command\"}");
    return;
  }
  if (command.type == laser::CommandType::Status) {
    sendState(clientNum);
    return;
  }
  sharedState->apply(command);  // broadcast happens in webUiLoop when the revision changes
}

void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("ws: client %u connected\n", clientNum);
      if (clientNum < WEBSOCKETS_SERVER_CLIENT_MAX) {
        isClientConnected[clientNum] = true;
      }
      sendState(clientNum);
      break;
    case WStype_DISCONNECTED:
      if (clientNum < WEBSOCKETS_SERVER_CLIENT_MAX && isClientConnected[clientNum]) {
        isClientConnected[clientNum] = false;
        Serial.printf("ws: client %u disconnected\n", clientNum);
        sharedState->triggerFailsafe(FailsafeReason::ClientDisconnected);
      }
      break;
    case WStype_TEXT:
      handleText(clientNum, payload, length);
      break;
    default:
      break;
  }
}

void onStationDisconnected(arduino_event_id_t, arduino_event_info_t) {
  sharedState->triggerFailsafe(FailsafeReason::WifiDisconnected);
}

void handleRoot() {
  const size_t length = static_cast<size_t>(INDEX_HTML_END - INDEX_HTML_START) - 1;  // drop NUL
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send_P(200, "text/html", INDEX_HTML_START, length);
}

}  // namespace

bool webUiBegin(SharedLaserState* state) {
  sharedState = state;
  WiFi.onEvent(onStationDisconnected, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    return false;
  }
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.onNotFound([]() { httpServer.send(404, "text/plain", "not found"); });
  httpServer.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(WS_PING_INTERVAL_MS, WS_PONG_TIMEOUT_MS,
                            WS_MISSED_PONGS_BEFORE_DISCONNECT);

  Serial.printf("wifi: AP '%s' up, open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  return true;
}

void webUiLoop() {
  httpServer.handleClient();
  webSocket.loop();

  const uint32_t revision = sharedState->revision();
  const uint32_t nowMs = millis();
  if (revision != lastBroadcastRevision &&
      nowMs - lastBroadcastMs >= STATE_BROADCAST_MIN_INTERVAL_MS) {
    lastBroadcastRevision = revision;
    lastBroadcastMs = nowMs;
    sendState(-1);
  }
}

void webUiNotifyFailsafe(FailsafeReason reason) {
  char json[64];
  snprintf(json, sizeof(json), "{\"type\":\"failsafe\",\"reason\":\"%s\"}",
           failsafeReasonName(reason));
  webSocket.broadcastTXT(json);
}
