#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <time.h>

#include "ST7305_U8g2.h"
#include "config.h"

namespace {

constexpr uint16_t SCREEN_W = 400;
constexpr uint16_t SCREEN_H = 300;
constexpr uint16_t HEADER_H = 58;
constexpr uint16_t BAR_X = 123;
constexpr uint16_t BAR_Y = 12;
constexpr uint16_t BAR_W = 265;
constexpr uint16_t BAR_H = 34;
constexpr uint16_t STEP_TEXT_X = 42;
constexpr uint16_t STEP_TEXT_RIGHT = 388;
constexpr uint32_t SCROLL_HOLD_MS = 3000;
constexpr uint8_t SCROLL_CHARS_PER_STEP = 1;
constexpr uint32_t SCROLL_CHAR_STEP_MS = 125;
constexpr uint32_t DISPLAY_INTERVAL_MS = 20;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t MQTT_RETRY_MS = 5000;

// Broches de l'exemple officiel Waveshare pour l'ecran ST7305.
constexpr int LCD_SCK = 11;
constexpr int LCD_MOSI = 12;
constexpr int LCD_DC = 5;
constexpr int LCD_CS = 40;
constexpr int LCD_RST = 41;

ST7305_U8g2 lcd(LCD_SCK, LCD_MOSI, LCD_DC, LCD_CS, LCD_RST);
U8G2 *display = nullptr;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

struct TimerState {
  uint32_t durationSeconds = 15 * 60;
  uint32_t remainingSeconds = 15 * 60;
  uint32_t lastTickMs = 0;
  bool running = false;
};

struct MqttCommand {
  uint16_t length;
  char payload[512];
};

struct StateSnapshot {
  bool running;
  uint32_t remainingSeconds;
  uint32_t durationSeconds;
  bool stepDone[3];
};

TimerState timer;
String steps[3] = {"Preparer le materiel", "on fait un test avec un phrases super longue pour voir si ca rotate bien", "Verifier le resultat"};
bool stepDone[3] = {false, false, false};
uint32_t stepScrollStartMs[3] = {0, 0, 0};
uint32_t stepScrollEndMs[3] = {0, 0, 0};
uint16_t stepScrollCharOffset[3] = {0, 0, 0};
uint32_t stepScrollLastStepMs[3] = {0, 0, 0};
bool stepScrollAtEnd[3] = {false, false, false};
QueueHandle_t commandQueue = nullptr;
QueueHandle_t stateQueue = nullptr;
volatile bool mqttConnected = false;
volatile bool refreshStateRequested = false;
String commandTopic;
String stateTopic;
uint32_t lastDrawMs = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;

void formatDuration(uint32_t seconds, char *result, size_t resultSize) {
  snprintf(result, resultSize, "%02lu:%02lu", static_cast<unsigned long>(seconds / 60),
           static_cast<unsigned long>(seconds % 60));
}

void drawCenteredInBar(const char *text, int baseline, uint8_t color, int clipRight) {
  const int textWidth = display->getStrWidth(text);
  const int textX = BAR_X + (BAR_W - textWidth) / 2;
  display->setDrawColor(color);
  display->setClipWindow(BAR_X + 2, BAR_Y + 2, clipRight, BAR_Y + BAR_H - 2);
  display->drawStr(textX, baseline, text);
  display->setMaxClipWindow();
}

void drawStepText(uint8_t index, int baseline) {
  const uint8_t *font = (index == 1) ? u8g2_font_helvB18_tf : u8g2_font_helvB14_tf;
  display->setFont(font);

  const int availableWidth = STEP_TEXT_RIGHT - STEP_TEXT_X;
  const int textWidth = display->getUTF8Width(steps[index].c_str());
  String visibleText = steps[index];

  if (textWidth > availableWidth) {
    const uint32_t now = millis();

    if (stepScrollAtEnd[index]) {
      if (now - stepScrollEndMs[index] >= SCROLL_HOLD_MS) {
        stepScrollCharOffset[index] = 0;
        stepScrollAtEnd[index] = false;
        stepScrollStartMs[index] = now;
        stepScrollLastStepMs[index] = now;
      }
    } else if (now - stepScrollStartMs[index] >= SCROLL_HOLD_MS &&
               now - stepScrollLastStepMs[index] >= SCROLL_CHAR_STEP_MS) {
      const size_t textLength = steps[index].length();
      for (uint8_t n = 0; n < SCROLL_CHARS_PER_STEP && stepScrollCharOffset[index] < textLength; ++n) {
        const uint8_t lead = static_cast<uint8_t>(steps[index][stepScrollCharOffset[index]]);
        uint8_t charBytes = 1;
        if ((lead & 0xE0) == 0xC0) charBytes = 2;
        else if ((lead & 0xF0) == 0xE0) charBytes = 3;
        else if ((lead & 0xF8) == 0xF0) charBytes = 4;
        if (stepScrollCharOffset[index] + charBytes > textLength) charBytes = 1;
        stepScrollCharOffset[index] += charBytes;
      }
      stepScrollLastStepMs[index] = now;

      visibleText = steps[index].substring(stepScrollCharOffset[index]);
      if (display->getUTF8Width(visibleText.c_str()) <= availableWidth) {
        stepScrollAtEnd[index] = true;
        stepScrollEndMs[index] = now;
      }
    }
    visibleText = steps[index].substring(stepScrollCharOffset[index]);
  }

  // Clip the line so scrolling text never paints over another step or the screen edge.
  display->setClipWindow(STEP_TEXT_X, baseline - 22, STEP_TEXT_RIGHT, baseline + 6);
  display->drawUTF8(STEP_TEXT_X, baseline, visibleText.c_str());
  display->setMaxClipWindow();
}

void drawDashboard() {
  char clockText[10] = "--:--";
  tm localTime{};
  if (getLocalTime(&localTime, 10)) {
    strftime(clockText, sizeof(clockText), "%H:%M", &localTime);
  }

  char timerText[10];
  formatDuration(timer.remainingSeconds, timerText, sizeof(timerText));
  const uint32_t elapsed = timer.durationSeconds - timer.remainingSeconds;
  const int filledWidth = timer.durationSeconds == 0 ? 0 :
    static_cast<int>((static_cast<uint64_t>(elapsed) * (BAR_W - 4)) / timer.durationSeconds);
  const int fillRight = BAR_X + 2 + filledWidth;

  display->clearBuffer();
  display->setDrawColor(1);

  // Heure compacte, en haut a gauche.
  display->setFont(u8g2_font_6x12_tf);
  display->drawStr(12, 25, "HEURE");
  display->setFont(u8g2_font_logisoso20_tn);
  display->drawStr(12, 50, clockText);

  // Barre : noir = temps ecoule. Le nombre est noir sur fond blanc puis blanc
  // dans la partie deja noire, ce qui produit l'inversion progressive demandee.
  display->drawRFrame(BAR_X, BAR_Y, BAR_W, BAR_H, 5);
  if (filledWidth > 0) {
    display->drawRBox(BAR_X + 2, BAR_Y + 2, filledWidth, BAR_H - 4, 3);
  }
  display->setFont(u8g2_font_logisoso18_tn);
  const int timerBaseline = BAR_Y + 25;
  drawCenteredInBar(timerText, timerBaseline, 1, BAR_X + BAR_W - 2);
  if (filledWidth > 0) {
    drawCenteredInBar(timerText, timerBaseline, 0, fillRight);
  }

  display->drawHLine(12, HEADER_H, SCREEN_W - 24);
  display->setFont(u8g2_font_6x12_tf);
  display->drawStr(12, 78, "ETAPES");
  for (uint8_t i = 0; i < 3; ++i) {
    const int y = 102 + i * 59;
    display->drawFrame(14, y - 13, 18, 18);
    if (stepDone[i]) {
      display->drawLine(17, y - 4, 22, y + 1);
      display->drawLine(22, y + 1, 30, y - 9);
    }
    // Etapes 1 et 3 restent plus discretes ; l'etape 2 est mise en avant.
    drawStepText(i, y);
    if (i < 2) display->drawHLine(14, y + 14, SCREEN_W - 28);
  }

  display->setFont(u8g2_font_5x8_tf);
  display->drawStr(12, 285, mqttConnected ? "MQTT connecte" : "MQTT hors ligne");
  display->sendBuffer();
}

void queueStatePublish() {
  if (!stateQueue) return;
  StateSnapshot snapshot{};
  snapshot.running = timer.running;
  snapshot.remainingSeconds = timer.remainingSeconds;
  snapshot.durationSeconds = timer.durationSeconds;
  for (uint8_t i = 0; i < 3; ++i) snapshot.stepDone[i] = stepDone[i];
  xQueueOverwrite(stateQueue, &snapshot);
}

bool publishState(const StateSnapshot &snapshot) {
  JsonDocument state;
  state["running"] = snapshot.running;
  state["remaining"] = snapshot.remainingSeconds;
  state["duration"] = snapshot.durationSeconds;
  JsonArray done = state["done"].to<JsonArray>();
  for (bool value : snapshot.stepDone) done.add(value);
  char payload[256];
  serializeJson(state, payload, sizeof(payload));
  return mqtt.publish(stateTopic.c_str(), payload, true);
}

void resetTimer(uint32_t seconds) {
  timer.durationSeconds = seconds == 0 ? 1 : seconds;
  timer.remainingSeconds = timer.durationSeconds;
  timer.running = false;
  timer.lastTickMs = millis();
}

void handleCommand(const JsonDocument &doc) {
  const char *action = doc["timer"].is<const char *>() ? doc["timer"].as<const char *>() :
                       (doc["command"] | "");
  const uint32_t seconds = doc["seconds"] | timer.durationSeconds;

  if (strcmp(action, "start") == 0) {
    if (doc["seconds"].is<uint32_t>()) resetTimer(seconds);
    timer.running = true;
    timer.lastTickMs = millis();
  } else if (strcmp(action, "pause") == 0) {
    timer.running = false;
  } else if (strcmp(action, "resume") == 0) {
    timer.running = timer.remainingSeconds > 0;
    timer.lastTickMs = millis();
  } else if (strcmp(action, "reset") == 0) {
    resetTimer(seconds);
  }

  JsonArrayConst newSteps = doc["steps"].as<JsonArrayConst>();
  if (!newSteps.isNull()) {
    for (uint8_t i = 0; i < 3 && i < newSteps.size(); ++i) {
      steps[i] = newSteps[i].as<String>();
      stepDone[i] = false;
      stepScrollStartMs[i] = millis();
      stepScrollEndMs[i] = 0;
      stepScrollCharOffset[i] = 0;
      stepScrollLastStepMs[i] = stepScrollStartMs[i];
      stepScrollAtEnd[i] = false;
    }
  }

  if (doc["step"].is<JsonObject>()) {
    const uint8_t index = doc["step"]["index"] | 0;
    if (index < 3) stepDone[index] = doc["step"]["done"] | false;
  }
  queueStatePublish();
}

void onMqttMessage(char *, byte *payload, unsigned int length) {
  if (!commandQueue || length >= sizeof(MqttCommand::payload)) {
    Serial.println("Commande MQTT ignoree: trop longue ou file indisponible");
    return;
  }
  MqttCommand command{};
  command.length = static_cast<uint16_t>(length);
  memcpy(command.payload, payload, length);
  command.payload[length] = '\0';
  if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
    Serial.println("Commande MQTT ignoree: file pleine");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED || millis() - lastWiFiAttemptMs < WIFI_RETRY_MS) return;
  lastWiFiAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connexion Wi-Fi a %s\n", WIFI_SSID);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected() || millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = millis();
  const String clientId = String(DEVICE_ID) + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  const bool connected = strlen(MQTT_USER) == 0
    ? mqtt.connect(clientId.c_str())
    : mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
  if (!connected) {
    Serial.printf("MQTT erreur=%d\n", mqtt.state());
    return;
  }
  mqtt.subscribe(commandTopic.c_str());
  Serial.printf("MQTT connecte, ecoute %s\n", commandTopic.c_str());
}

void networkTask(void *) {
  bool wasConnected = false;
  bool pendingState = false;
  StateSnapshot latestState{};

  for (;;) {
    connectWiFi();
    connectMqtt();

    const bool connected = mqtt.connected();
    mqttConnected = connected;
    if (connected) {
      if (!wasConnected) refreshStateRequested = true;
      mqtt.loop();
      wasConnected = mqtt.connected();
      mqttConnected = wasConnected;
    } else {
      wasConnected = false;
    }

    if (xQueueReceive(stateQueue, &latestState, 0) == pdTRUE) {
      pendingState = true;
    }
    if (mqtt.connected() && pendingState && publishState(latestState)) {
      pendingState = false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void updateTimer() {
  if (!timer.running) return;
  const uint32_t now = millis();
  const uint32_t elapsedMs = now - timer.lastTickMs;
  if (elapsedMs < 1000) return;
  const uint32_t elapsedSeconds = elapsedMs / 1000;
  timer.lastTickMs += elapsedSeconds * 1000;
  if (elapsedSeconds >= timer.remainingSeconds) {
    timer.remainingSeconds = 0;
    timer.running = false;
    queueStatePublish();
  } else {
    timer.remainingSeconds -= elapsedSeconds;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  lcd.begin(0, U8G2_R1);  // Paysage logique 400 x 300, connecteurs vers le bas.
  display = lcd.getU8g2();

  commandTopic = String("workshop/") + DEVICE_ID + "/command";
  stateTopic = String("workshop/") + DEVICE_ID + "/state";
  for (uint8_t i = 0; i < 3; ++i) stepScrollStartMs[i] = millis();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  commandQueue = xQueueCreate(4, sizeof(MqttCommand));
  stateQueue = xQueueCreate(1, sizeof(StateSnapshot));
  if (!commandQueue || !stateQueue) {
    Serial.println("Erreur: impossible de creer les files reseau");
  } else {
    queueStatePublish();
    xTaskCreatePinnedToCore(networkTask, "mqtt_network", 6144, nullptr, 1, nullptr, 0);
  }
  drawDashboard();
}

void loop() {
  MqttCommand command{};
  while (commandQueue && xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, command.payload, command.length);
    if (error) {
      Serial.printf("Commande MQTT invalide: %s\n", error.c_str());
    } else {
      handleCommand(doc);
    }
  }
  if (refreshStateRequested) {
    refreshStateRequested = false;
    queueStatePublish();
  }
  updateTimer();
  if (millis() - lastDrawMs >= DISPLAY_INTERVAL_MS) {
    lastDrawMs = millis();
    drawDashboard();
  }
}
