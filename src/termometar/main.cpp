/*
 * BEŽIČNI SENZOR TEMPERATURE SA OLED DISPLEJOM (v1.2 + OTA Optimizovana verzija)
 * ESP8266 + DS18B20 (D4) + OLED 0.96" I2C (SDA=D2, SCL=D1)
 * Šalje temperaturu HTTP zahtevom kontroleru svakih 30s i prikazuje na displeju.
 *
 * ŠTA JE OPTIMIZOVANO I DODATO:
 * 1. Uklonjen blokirajući delay(30000) i zamenjen sa millis() tajmerom — uređaj više
 *    nije "mrtav" 30 sekundi između slanja, već u svakom trenutku prima OTA update!
 * 2. Dodat ArduinoOTA (za bežični upload iz PlatformIO / Arduino IDE) sa grafičkim
 *    prikazom procenta (0-100%) na OLED ekranu.
 * 3. Dodat Web OTA server na http://<IP_TERMOMETRA>/firmware (za update iz pregledača).
 * 4. Dodat automatski WiFi Reconnect ako ruter privremeno izgubi struju i zaštita
 *    od lažnog DS18B20 očitavanja (85.0°C power-on reset vrednost) i kratak HTTP timeout.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "secrets.h"

const char* controllerIP = "http://192.168.1.75/update";
const unsigned long SEND_INTERVAL_MS = 30000UL;

#define ONE_WIRE_BUS 2     // D4 (GPIO2)
#define WIFI_RESET_PIN 0   // D3 (GPIO0) - FLASH dugme (drži 5s za reset WiFi-a)

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient wifiClient;
WiFiManager wifiManager;
ESP8266WebServer otaWebServer(80);
ESP8266HTTPUpdateServer httpUpdater;

float currentTemp = 0.0;
float outdoorTemp = -99.0;
bool wifiConnected = false;
unsigned long lastSendTime = 0;
unsigned long lastWiFiCheck = 0;
unsigned long resetBtnStart = 0;

void setupWiFiManager();
void setupOTA();
void checkWiFiReset();
void measureAndSend();
bool sendTemperature(float temp);
void displayStartupScreen();
void displayTemperature(float temp, float outdoor, bool wifi, bool sent);
void drawWiFiIcon(int x, int y, int rssi);
void displayMessage(String line1, String line2, String line3);
void displayError(String error);

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=== BEŽIČNI SENZOR SA OLED (v1.3 + WiFiManager + OTA) ===");

  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  Wire.begin(4, 5);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("GREŠKA: OLED displej nije pronađen!");
  } else {
    display.clearDisplay();
    displayStartupScreen();
  }

  sensors.begin();
  setupWiFiManager();
  setupOTA();

  measureAndSend();
  lastSendTime = millis();
}

void loop() {
  ArduinoOTA.handle();
  otaWebServer.handleClient();
  checkWiFiReset();

  unsigned long now = millis();

  if (now - lastWiFiCheck >= 10000UL) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      WiFi.reconnect();
    } else {
      wifiConnected = true;
    }
  }

  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    measureAndSend();
  }

  delay(10);
}

void setupWiFiManager() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  WiFi.setAutoReconnect(true);

  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);
  wifiManager.setAPCallback([](WiFiManager*) {
    displayMessage("WiFi Podesavanje", "AP: Termometar-Setup", "IP: 192.168.4.1");
  });

  displayMessage("Povezivanje", "WiFi...", "");
  if (!wifiManager.autoConnect("Termometar-Setup", "12345678")) {
    displayMessage("WiFi greska!", "Restartujem...", "");
    delay(2000);
    ESP.restart();
  }

  wifiConnected = true;
  displayMessage("WiFi OK!", "IP: " + WiFi.localIP().toString(), "OTA: /firmware");
  delay(2000);
}

void checkWiFiReset() {
  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    if (resetBtnStart == 0) resetBtnStart = millis();
    if (millis() - resetBtnStart > 5000) {
      displayMessage("Resetujem WiFi", "Pustite dugme...", "");
      wifiManager.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    resetBtnStart = 0;
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("termometar-senzor");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(18, 4);
    display.println(F("OTA AZURIRANJE"));
    display.drawLine(0, 18, 127, 18, SSD1306_WHITE);
    display.display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = (progress * 100) / total;
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(22, 4);
    display.print(F("OTA UPDATE..."));
    display.drawLine(0, 18, 127, 18, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(38, 24);
    display.print(pct);
    display.print(F("%"));
    display.drawRect(4, 48, 120, 12, SSD1306_WHITE);
    display.fillRect(6, 50, (pct * 116) / 100, 8, SSD1306_WHITE);
    display.display();
  });

  ArduinoOTA.onEnd([]() { displayMessage("OTA USPESAN!", "Restartovanje...", ""); });
  ArduinoOTA.onError([](ota_error_t error) { displayError("OTA Greska!"); });
  ArduinoOTA.begin();

  httpUpdater.setup(&otaWebServer, "/firmware", OTA_USER, OTA_PASS);

  otaWebServer.on("/", HTTP_GET, []() {
    String info = "Termometar v1.3 (OTA) | Temp: " + String(currentTemp, 1) + " C | Za Web OTA idite na /firmware";
    otaWebServer.send(200, "text/plain; charset=utf-8", info);
  });

  otaWebServer.on("/reset-wifi", HTTP_GET, []() {
    if (!otaWebServer.authenticate(OTA_USER, OTA_PASS)) {
      return otaWebServer.requestAuthentication();
    }
    otaWebServer.send(200, "text/plain", "WiFi reset - restartujem...");
    delay(500);
    wifiManager.resetSettings();
    ESP.restart();
  });

  otaWebServer.begin();
}

void measureAndSend() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);

  if (temp == DEVICE_DISCONNECTED_C || temp == 85.0 || temp < -50 || temp > 100) {
    displayError("Senzor greska!");
    return;
  }

  currentTemp = temp;
  bool sendSuccess = false;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    sendSuccess = sendTemperature(currentTemp);
  } else {
    wifiConnected = false;
  }

  displayTemperature(currentTemp, outdoorTemp, wifiConnected, sendSuccess);
}

bool sendTemperature(float temp) {
  HTTPClient http;
  http.setTimeout(2500);

  String url = String(controllerIP) + "?temp=" + String(temp, 2);
  http.begin(wifiClient, url);
  int httpCode = http.GET();

  bool success = false;
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, payload) && doc.containsKey("outdoorTemp")) {
      outdoorTemp = doc["outdoorTemp"];
    }
    success = true;
  }

  http.end();
  return success;
}

void displayStartupScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(F("TERMOSTAT"));
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.println(F("Bezicni senzor v1.3"));
  display.setCursor(0, 50);
  display.println(F("Startovanje..."));
  display.display();
  delay(2000);
}

void displayTemperature(float temp, float outdoor, bool wifi, bool sent) {
  display.clearDisplay();
  display.setTextSize(4);
  display.setCursor(0, 36);
  if (temp >= 0 && temp < 10) display.print(F(" "));
  display.print(temp, 1);

  display.setCursor(98, 42);
  display.setTextSize(2);
  display.print(F("o"));
  display.setCursor(110, 46);
  display.print(F("C"));

  display.drawLine(0, 20, 127, 20, SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(5, 0);
  if (outdoor > -99) {
    if (outdoor >= 0 && outdoor < 10) display.print(F(" "));
    display.print(outdoor, 1);
    display.print(F("o"));
  } else {
    display.print(F("--.-"));
  }

  if (wifi) {
    drawWiFiIcon(105, 0, WiFi.RSSI());
  } else {
    display.drawLine(105, 0, 115, 10, SSD1306_WHITE);
    display.drawLine(115, 0, 105, 10, SSD1306_WHITE);
  }
  display.display();
}

void drawWiFiIcon(int x, int y, int rssi) {
  int bars = 1;
  if (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;

  for (int i = 0; i < 4; i++) {
    int barHeight = 3 + (i * 3);
    if (i < bars) display.fillRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
    else display.drawRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
  }
}

void displayMessage(String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(line1);
  display.setCursor(0, 30);
  display.println(line2);
  if (line3.length() > 0) {
    display.setCursor(0, 50);
    display.println(line3);
  }
  display.display();
}

void displayError(String error) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 10);
  display.println(F("GRESKA!"));
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println(error);
  display.display();
}
