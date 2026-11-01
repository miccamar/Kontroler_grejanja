/*
 * KONTROLER GREJANJA v2.3 SA OTA PODRŠKOM (PlatformIO + LittleFS)
 * ESP8266 + Relej + LittleFS Web Server + ArduinoOTA + Web OTA (/firmware)
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoOTA.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <LittleFS.h>
#include <WiFiManager.h>

const char* ntpServer = "pool.ntp.org";
const char* tzInfo = "CET-1CEST,M3.5.0,M10.5.0/3";

const char* weatherApiKey = "Tvoj_OpenWeather_API_Ključ";
const char* weatherCity = "Tvoja_lokacija";

#define RELAY_PIN 5          // D1 (GPIO5) - Aktivno na LOW
#define MANUAL_BUTTON 12     // D6 (GPIO12)
#define AUTO_LED 14          // D5 (GPIO14) - Zelena LED
#define MANUAL_LED 0         // D3 (GPIO0)  - Crvena LED
#define LOW_BATTERY_LED 13   // D7 (GPIO13)

const unsigned long SENSOR_TIMEOUT_MS = 15UL * 60UL * 1000UL;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiManager wifiManager;

unsigned long bootTime = 0;

float currentTemp = 0.0;
bool sensorValid = false;
float outdoorTemp = -99.0;
String weatherDescription = "";
bool heatingOn = false;
bool manualMode = false;
bool manualForceOff = false;
float manualTargetTemp = 26.0;
float activeTargetTemp = 17.0;
unsigned long lastTempUpdate = 0;
unsigned long lastWeatherUpdate = 0;

bool lowBattery = false;
float batteryVoltage = 0.0;
int batteryPercent = 0;
unsigned long lastBatteryBlink = 0;

float hysteresisOn = 2.0;
float hysteresisOff = 0.5;

struct TimePeriod {
  bool enabled;
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  float targetTemp;
};

struct DaySchedule {
  TimePeriod period1;
  TimePeriod period2;
  TimePeriod period3;
  float nightTemp;
};

DaySchedule weekSchedule[7];

#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xAB12

struct EEPROMData {
  uint16_t magic;
  DaySchedule schedule[7];
  bool manualMode;
  float hysteresisOn;
  float hysteresisOff;
  float manualTargetTemp;
  bool manualForceOff;
};

void setupWiFiManager();
void setupOTA();
void checkWiFiReset();
void updateLEDs();
void toggleHeating();
void switchToAutoMode();
void updateOutdoorTemperature();
void controlHeating();
void saveSettings();
void loadSettings();
void setDefaultSchedule();
String getDayName(int day);
void printCurrentTime();
int calculateBatteryPercent(float voltage);
String getUptime();

void handleTempUpdate();
void handleStatus();
void handleManual();
void handleGetSchedule();
void handleSetSchedule();
void handleGetTime();
void handleSetHysteresis();
void handleSystemInfo();
void handleResetWiFi();
void handleRestart();

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  Serial.println("\n\n=== KONTROLER GREJANJA V2.3 (OTA + LittleFS) ===");

  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS mount failed!");
  } else {
    Serial.println("✅ LittleFS uspešno montiran");
  }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  pinMode(MANUAL_BUTTON, INPUT_PULLUP);
  pinMode(AUTO_LED, OUTPUT);
  pinMode(MANUAL_LED, OUTPUT);
  pinMode(LOW_BATTERY_LED, OUTPUT);
  digitalWrite(LOW_BATTERY_LED, LOW);

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  updateLEDs();

  setupWiFiManager();
  bootTime = millis();

  configTime(tzInfo, ntpServer, "time.nist.gov");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 24 * 3600 && retries < 20) {
    delay(500);
    now = time(nullptr);
    retries++;
  }
  printCurrentTime();

  // Inicijalizuj ArduinoOTA + Web OTA (/firmware)
  setupOTA();

  // Ruta "/" služi index.html, a ako stigne "/?temp=..." (iz termometar_v1.4-nova), čita temperaturu!
  server.on("/", HTTP_GET, []() {
    if (server.hasArg("temp")) {
      handleTempUpdate();
      return;
    }
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
      server.send(404, "text/plain", "index.html not found (uradite Upload Filesystem Image u PlatformIO ili preko /firmware)");
      return;
    }
    server.streamFile(file, "text/html");
    file.close();
  });

  server.on("/update", HTTP_GET, handleTempUpdate);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/manual", HTTP_GET, handleManual);
  server.on("/api/schedule", HTTP_GET, handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handleSetSchedule);
  server.on("/api/time", HTTP_GET, handleGetTime);
  server.on("/api/hysteresis", HTTP_POST, handleSetHysteresis);
  server.on("/api/system", HTTP_GET, handleSystemInfo);
  server.on("/api/reset-wifi", HTTP_POST, handleResetWiFi);
  server.on("/api/restart", HTTP_POST, handleRestart);

  server.onNotFound([]() {
    String path = server.uri();
    String contentType = "text/plain";
    if (path.endsWith(".html")) contentType = "text/html";
    else if (path.endsWith(".css")) contentType = "text/css";
    else if (path.endsWith(".js")) contentType = "application/javascript";
    else if (path.endsWith(".json")) contentType = "application/json";

    File file = LittleFS.open(path, "r");
    if (!file) {
      server.send(404, "text/plain", "404: Not Found");
      return;
    }
    server.streamFile(file, contentType);
    file.close();
  });

  server.begin();
  Serial.println("🌐 Web server pokrenut: http://" + WiFi.localIP().toString());
  Serial.println("📦 Web OTA dostupan na: http://" + WiFi.localIP().toString() + "/firmware");

  updateOutdoorTemperature();
  lastWeatherUpdate = millis();
}

void setupOTA() {
  // 1. Web OTA preko pregledača (/firmware)
  httpUpdater.setup(&server, "/firmware");

  // 2. ArduinoOTA za direktan upload iz PlatformIO
  ArduinoOTA.setHostname("kontroler-grejanja");
  ArduinoOTA.onStart([]() {
    // Bezbedno isključi relej grejanja dok traje flešovanje
    digitalWrite(RELAY_PIN, HIGH);
    LittleFS.end();
    Serial.println("📦 ArduinoOTA upis započet...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n✅ ArduinoOTA završen!");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("❌ ArduinoOTA greška [%u]\n", error);
  });
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  checkWiFiReset();

  static bool lastButtonState = HIGH;
  static unsigned long buttonPressTime = 0;
  bool currentButtonState = digitalRead(MANUAL_BUTTON);

  if (lastButtonState == HIGH && currentButtonState == LOW) {
    buttonPressTime = millis();
  }

  if (lastButtonState == LOW && currentButtonState == HIGH) {
    unsigned long pressDuration = millis() - buttonPressTime;
    if (pressDuration > 50 && pressDuration < 3000) {
      toggleHeating();
    } else if (pressDuration >= 3000 && pressDuration < 10000) {
      switchToAutoMode();
    }
  }
  lastButtonState = currentButtonState;

  if (lowBattery) {
    if (millis() - lastBatteryBlink > 500) {
      digitalWrite(LOW_BATTERY_LED, !digitalRead(LOW_BATTERY_LED));
      lastBatteryBlink = millis();
    }
  } else {
    digitalWrite(LOW_BATTERY_LED, LOW);
  }

  if (millis() - lastWeatherUpdate > 600000UL) {
    updateOutdoorTemperature();
    lastWeatherUpdate = millis();
  }

  controlHeating();
  delay(10);
}

void controlHeating() {
  bool sensorStale = !sensorValid || (millis() - lastTempUpdate > SENSOR_TIMEOUT_MS);
  if (sensorStale) {
    if (heatingOn) {
      digitalWrite(RELAY_PIN, HIGH);
      heatingOn = false;
      Serial.println("⚠️ FAIL-SAFE: Grejanje ISKLJUČENO jer nema svežih podataka sa senzora!");
    }
    return;
  }

  float targetTemp;

  if (manualMode) {
    if (manualForceOff) {
      activeTargetTemp = 0.0;
      if (heatingOn) {
        digitalWrite(RELAY_PIN, HIGH);
        heatingOn = false;
      }
      return;
    }
    targetTemp = manualTargetTemp;
  } else {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);

    int currentDay = timeinfo->tm_wday;
    int currentTimeInMin = timeinfo->tm_hour * 60 + timeinfo->tm_min;

    DaySchedule& today = weekSchedule[currentDay];
    targetTemp = today.nightTemp;
    bool inSchedule = false;

    const TimePeriod* periods[3] = { &today.period1, &today.period2, &today.period3 };
    for (int i = 0; i < 3; i++) {
      if (periods[i]->enabled && !inSchedule) {
        int startMin = periods[i]->startHour * 60 + periods[i]->startMinute;
        int endMin = periods[i]->endHour * 60 + periods[i]->endMinute;
        if (currentTimeInMin >= startMin && currentTimeInMin < endMin) {
          targetTemp = periods[i]->targetTemp;
          inSchedule = true;
        }
      }
    }
  }

  activeTargetTemp = targetTemp;

  bool shouldHeat = heatingOn
    ? (currentTemp < (targetTemp + hysteresisOff))
    : (currentTemp < (targetTemp - hysteresisOn));

  if (shouldHeat && !heatingOn) {
    digitalWrite(RELAY_PIN, LOW);
    heatingOn = true;
  } else if (!shouldHeat && heatingOn) {
    digitalWrite(RELAY_PIN, HIGH);
    heatingOn = false;
  }
}

void handleTempUpdate() {
  if (server.hasArg("temp")) {
    float incomingTemp = server.arg("temp").toFloat();
    if (incomingTemp > -40.0 && incomingTemp < 85.0) {
      currentTemp = incomingTemp;
      sensorValid = true;
      lastTempUpdate = millis();
    }

    if (server.hasArg("voltage")) {
      batteryVoltage = server.arg("voltage").toFloat();
      batteryPercent = calculateBatteryPercent(batteryVoltage);
      lowBattery = (batteryVoltage > 0.5 && batteryVoltage < 3.4);
    } else {
      batteryVoltage = 0.0;
      batteryPercent = 0;
      lowBattery = false;
    }

    StaticJsonDocument<128> doc;
    doc["status"] = "OK";
    doc["outdoorTemp"] = outdoorTemp;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  } else {
    server.send(400, "text/plain", "Missing temp");
  }
}

int calculateBatteryPercent(float voltage) {
  if (voltage <= 0.5) return 0;
  if (voltage >= 4.15) return 100;
  if (voltage >= 4.00) return 90;
  if (voltage >= 3.90) return 80;
  if (voltage >= 3.80) return 60;
  if (voltage >= 3.70) return 40;
  if (voltage >= 3.55) return 20;
  if (voltage >= 3.40) return 10;
  return 5;
}

void handleGetTime() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  StaticJsonDocument<256> doc;
  doc["hour"] = timeinfo->tm_hour;
  doc["minute"] = timeinfo->tm_min;
  doc["day"] = timeinfo->tm_wday;
  doc["dayName"] = getDayName(timeinfo->tm_wday);

  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
  doc["timeStr"] = timeStr;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void toggleHeating() {
  if (!manualMode) {
    manualMode = true;
    updateLEDs();
  }
  manualForceOff = !manualForceOff;
  if (manualForceOff) {
    digitalWrite(RELAY_PIN, HIGH);
    heatingOn = false;
  }
  saveSettings();
}

void switchToAutoMode() {
  manualMode = false;
  manualForceOff = false;
  updateLEDs();
  saveSettings();
}

void updateLEDs() {
  digitalWrite(AUTO_LED, manualMode ? LOW : HIGH);
  digitalWrite(MANUAL_LED, manualMode ? HIGH : LOW);
}

void handleStatus() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  StaticJsonDocument<384> doc;
  doc["currentTemp"] = currentTemp;
  doc["outdoorTemp"] = outdoorTemp;
  doc["heatingOn"] = heatingOn;
  doc["manualMode"] = manualMode;
  doc["manualForceOff"] = manualForceOff;
  doc["targetTemp"] = activeTargetTemp;
  doc["lastUpdate"] = sensorValid ? ((millis() - lastTempUpdate) / 1000) : 9999;
  doc["currentDay"] = timeinfo->tm_wday;
  doc["currentHour"] = timeinfo->tm_hour;
  doc["currentMinute"] = timeinfo->tm_min;
  doc["hysteresisOn"] = hysteresisOn;
  doc["hysteresisOff"] = hysteresisOff;
  doc["manualTargetTemp"] = manualTargetTemp;
  doc["lowBattery"] = lowBattery;
  doc["batteryVoltage"] = batteryVoltage;
  doc["batteryPercent"] = batteryPercent;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleManual() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "auto") {
      manualMode = false;
      manualForceOff = false;
    } else if (mode == "on") {
      manualMode = true;
      manualForceOff = false;
    } else if (mode == "off") {
      manualMode = true;
      manualForceOff = true;
      digitalWrite(RELAY_PIN, HIGH);
      heatingOn = false;
    }
    updateLEDs();
    saveSettings();
    server.send(200, "text/plain", "OK");
  } else if (server.hasArg("temp")) {
    manualTargetTemp = constrain(server.arg("temp").toFloat(), 10.0f, 35.0f);
    saveSettings();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing mode");
  }
}

void handleGetSchedule() {
  if (!server.hasArg("day")) {
    server.send(400, "text/plain", "Missing day");
    return;
  }
  int day = server.arg("day").toInt();
  if (day < 0 || day > 6) {
    server.send(400, "text/plain", "Invalid day");
    return;
  }

  StaticJsonDocument<768> doc;
  DaySchedule& sched = weekSchedule[day];

  doc["p1"]["enabled"] = sched.period1.enabled;
  doc["p1"]["startHour"] = sched.period1.startHour;
  doc["p1"]["startMinute"] = sched.period1.startMinute;
  doc["p1"]["endHour"] = sched.period1.endHour;
  doc["p1"]["endMinute"] = sched.period1.endMinute;
  doc["p1"]["targetTemp"] = sched.period1.targetTemp;

  doc["p2"]["enabled"] = sched.period2.enabled;
  doc["p2"]["startHour"] = sched.period2.startHour;
  doc["p2"]["startMinute"] = sched.period2.startMinute;
  doc["p2"]["endHour"] = sched.period2.endHour;
  doc["p2"]["endMinute"] = sched.period2.endMinute;
  doc["p2"]["targetTemp"] = sched.period2.targetTemp;

  doc["p3"]["enabled"] = sched.period3.enabled;
  doc["p3"]["startHour"] = sched.period3.startHour;
  doc["p3"]["startMinute"] = sched.period3.startMinute;
  doc["p3"]["endHour"] = sched.period3.endHour;
  doc["p3"]["endMinute"] = sched.period3.endMinute;
  doc["p3"]["targetTemp"] = sched.period3.targetTemp;

  doc["nightTemp"] = sched.nightTemp;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSetSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing data");
    return;
  }
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "JSON parse error");
    return;
  }

  int day = doc["day"];
  if (day < 0 || day > 6) {
    server.send(400, "text/plain", "Invalid day");
    return;
  }

  DaySchedule& sched = weekSchedule[day];
  sched.period1 = { doc["p1"]["enabled"], doc["p1"]["startHour"], doc["p1"]["startMinute"], doc["p1"]["endHour"], doc["p1"]["endMinute"], doc["p1"]["targetTemp"] };
  sched.period2 = { doc["p2"]["enabled"], doc["p2"]["startHour"], doc["p2"]["startMinute"], doc["p2"]["endHour"], doc["p2"]["endMinute"], doc["p2"]["targetTemp"] };
  sched.period3 = { doc["p3"]["enabled"], doc["p3"]["startHour"], doc["p3"]["startMinute"], doc["p3"]["endHour"], doc["p3"]["endMinute"], doc["p3"]["targetTemp"] };
  sched.nightTemp = doc["nightTemp"];

  saveSettings();
  server.send(200, "text/plain", "Schedule updated");
}

void handleSetHysteresis() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing data");
    return;
  }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "JSON parse error");
    return;
  }
  hysteresisOn = constrain((float)doc["on"], 0.2f, 5.0f);
  hysteresisOff = constrain((float)doc["off"], 0.0f, 3.0f);
  saveSettings();
  server.send(200, "text/plain", "Hysteresis updated");
}

void updateOutdoorTemperature() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2500);

  String url = String("http://api.openweathermap.org/data/2.5/weather?q=") +
               weatherCity + "&appid=" + weatherApiKey + "&units=metric";

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      StaticJsonDocument<768> doc;
      if (!deserializeJson(doc, http.getString())) {
        if (doc.containsKey("main") && doc["main"].containsKey("temp")) {
          outdoorTemp = doc["main"]["temp"];
        }
      }
    }
    http.end();
  }
}

void saveSettings() {
  EEPROMData data;
  data.magic = EEPROM_MAGIC;
  for (int i = 0; i < 7; i++) {
    data.schedule[i] = weekSchedule[i];
  }
  data.manualMode = manualMode;
  data.hysteresisOn = hysteresisOn;
  data.hysteresisOff = hysteresisOff;
  data.manualTargetTemp = manualTargetTemp;
  data.manualForceOff = manualForceOff;

  EEPROM.put(0, data);
  EEPROM.commit();
}

void loadSettings() {
  EEPROMData data;
  EEPROM.get(0, data);

  if (data.magic == EEPROM_MAGIC) {
    for (int i = 0; i < 7; i++) {
      weekSchedule[i] = data.schedule[i];
    }
    manualMode = data.manualMode;
    hysteresisOn = constrain(data.hysteresisOn, 0.2f, 5.0f);
    hysteresisOff = constrain(data.hysteresisOff, 0.0f, 3.0f);
    manualTargetTemp = constrain(data.manualTargetTemp, 10.0f, 35.0f);
    manualForceOff = data.manualForceOff;
  } else {
    setDefaultSchedule();
  }
}

void setDefaultSchedule() {
  for (int i = 0; i < 7; i++) {
    weekSchedule[i].period1 = { true, (i == 0 || i == 6) ? 8 : 6, 0, 10, 0, 24.0 };
    weekSchedule[i].period2 = { true, 14, 0, 16, 0, 24.0 };
    weekSchedule[i].period3 = { true, 17, 0, 23, 0, 24.0 };
    weekSchedule[i].nightTemp = 17.0;
  }
  manualMode = false;
  manualForceOff = false;
}

void setupWiFiManager() {
  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect("Termostat-Setup", "12345678")) {
    delay(3000);
    ESP.restart();
  }
}

void checkWiFiReset() {
  static unsigned long resetStartTime = 0;
  static bool resetting = false;
  static unsigned long lastBlink = 0;
  static int blinkState = 0;

  if (digitalRead(MANUAL_BUTTON) == LOW) {
    if (resetStartTime == 0) resetStartTime = millis();
    unsigned long holdTime = millis() - resetStartTime;

    if (holdTime > 10000 && !resetting) {
      resetting = true;
    }

    if (resetting) {
      if (millis() - lastBlink > 150) {
        lastBlink = millis();
        blinkState = !blinkState;
        digitalWrite(AUTO_LED, blinkState);
        digitalWrite(MANUAL_LED, blinkState);
        digitalWrite(LOW_BATTERY_LED, blinkState);
      }
      if (holdTime > 13000) {
        wifiManager.resetSettings();
        delay(200);
        ESP.restart();
      }
    }
  } else {
    if (resetting) {
      updateLEDs();
      digitalWrite(LOW_BATTERY_LED, LOW);
    }
    resetStartTime = 0;
    resetting = false;
  }
}

void handleSystemInfo() {
  StaticJsonDocument<256> doc;
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = getUptime();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["chipId"] = ESP.getChipId();

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleResetWiFi() {
  server.send(200, "text/plain", "WiFi reset - restartujem...");
  delay(500);
  wifiManager.resetSettings();
  ESP.restart();
}

void handleRestart() {
  server.send(200, "text/plain", "Restartovanje...");
  delay(500);
  ESP.restart();
}

String getUptime() {
  unsigned long uptime = (millis() - bootTime) / 1000;
  unsigned long d = uptime / 86400;
  uptime %= 86400;
  unsigned long h = uptime / 3600;
  uptime %= 3600;
  unsigned long m = uptime / 60;

  String res = "";
  if (d > 0) res += String(d) + "d ";
  if (h > 0 || d > 0) res += String(h) + "h ";
  res += String(m) + "m";
  return res;
}

String getDayName(int day) {
  const char* names[] = {"Nedelja", "Ponedeljak", "Utorak", "Sreda", "Četvrtak", "Petak", "Subota"};
  return (day >= 0 && day <= 6) ? String(names[day]) : "Nepoznato";
}

void printCurrentTime() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  Serial.printf("⏰ Vreme: %s, %02d:%02d:%02d\n", getDayName(t->tm_wday).c_str(), t->tm_hour, t->tm_min, t->tm_sec);
}
