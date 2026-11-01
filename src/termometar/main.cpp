/*
 * BEŽIČNI SENZOR TEMPERATURE SA OLED DISPLEJOM (v1.2 + WiFiManager + OTA)
 * ESP8266 + DS18B20 (D4) + OLED 0.96" I2C (SDA=D2, SCL=D1)
 *
 * GLAVNE PREDNOSTI:
 * 1. NEMA ŠIFARA U KODU: Uređaj pokreće AP "Termometar-Setup" (šifra: 12345678, IP: 192.168.4.1).
 *    Povežete se telefonom i izaberete WiFi mrežu, unesete lozinku i URL kontrolera.
 * 2. URL Kontrolera (npr. http://192.168.1.75/update) se čuva u EEPROM-u.
 * 3. Reset WiFi podešavanja:
 *    - Fizički: Držite FLASH taster na ploči (D3/GPIO0) duže od 5 sekundi.
 *    - Bežično: Otvorite http://<IP_TERMOMETRA>/reset-wifi u pregledaču.
 * 4. OTA ažuriranje (ArduinoOTA + Web OTA na /firmware):
 *    Uvek aktivno, a na žuto-plavom OLED-u se prikazuje procenat i grafički progress bar.
 * 5. Neblokirajući rad (millis tajmer na 30s) sa automatskim WiFi reconnect-om.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <EEPROM.h>

// EEPROM konfiguracija
#define EEPROM_SIZE 256
#define EEPROM_MAGIC 0xC123

// FLASH taster na ploči (D3 / GPIO0) za WiFi Reset
#define FLASH_BUTTON_PIN 0

// DS18B20 na pinu D4 (GPIO2)
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// OLED displej 128x64 (SDA=GPIO4/D2, SCL=GPIO5/D1)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Mrežni klijenti i serveri
WiFiClient wifiClient;
WiFiManager wifiManager;
ESP8266WebServer otaWebServer(80);
ESP8266HTTPUpdateServer httpUpdater;

// Parametri kontrolera (čuvaju se u EEPROM-u)
char controllerIP[80] = "http://192.168.1.75/update";
bool shouldSaveConfig = false;

// Interval slanja temperature (30 sekundi, neblokirajući)
const unsigned long SEND_INTERVAL_MS = 30000UL;

// Promenljive stanja
float currentTemp = 0.0;
float outdoorTemp = -99.0;
bool wifiConnected = false;
int sendCounter = 0;
unsigned long lastSendTime = 0;
unsigned long lastWiFiCheck = 0;

// Deklaracije funkcija
void setupWiFiManager();
void checkWiFiReset();
void saveConfig();
void loadConfig();
void setupOTA();
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

  pinMode(FLASH_BUTTON_PIN, INPUT_PULLUP);

  Serial.println("\n\n=== BEŽIČNI SENZOR SA OLED (v1.2 + WiFiManager + OTA) ===");

  // Inicijalizuj OLED displej: SDA=GPIO4(D2), SCL=GPIO5(D1)
  Wire.begin(4, 5);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("GREŠKA: OLED displej nije pronađen!");
  } else {
    Serial.println("OLED displej inicijalizovan");
    display.clearDisplay();
    displayStartupScreen();
  }

  // Pokreni DS18B20 senzor
  sensors.begin();
  Serial.println("DS18B20 senzor pokrenut");

  // Povezivanje preko WiFiManager-a
  setupWiFiManager();

  // Pokreni OTA servere
  setupOTA();

  // Odmah izvrši prvo merenje i slanje
  measureAndSend();
  lastSendTime = millis();
}

void loop() {
  // Proveri fizičko držanje FLASH dugmeta (>5s za WiFi reset)
  checkWiFiReset();

  // Obrada OTA i Web zahteva (bez blokiranja)
  ArduinoOTA.handle();
  otaWebServer.handleClient();

  unsigned long now = millis();

  // Provera WiFi konekcije svakih 10s
  if (now - lastWiFiCheck >= 10000UL) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      WiFi.reconnect();
    } else {
      wifiConnected = true;
    }
  }

  // Očitaj i pošalji temperaturu svakih 30s
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    measureAndSend();
  }

  delay(10);
}

// ------------------------------------------------------------
// WiFiManager konfiguracija
// ------------------------------------------------------------
void setupWiFiManager() {
  loadConfig();

  WiFiManagerParameter custom_controller("controller", "URL Kontrolera (npr. http://192.168.1.75/update)", controllerIP, 79);
  wifiManager.addParameter(&custom_controller);

  wifiManager.setSaveConfigCallback([]() {
    shouldSaveConfig = true;
  });

  wifiManager.setConfigPortalTimeout(180); // 3 minuta timeout za portal

  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("WiFi Konfiguracija"));
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
    display.setCursor(0, 14);
    display.println(F("Povezi se na WiFi AP:"));
    display.setCursor(0, 26);
    display.println(F("Termometar-Setup"));
    display.setCursor(0, 38);
    display.println(F("Lozinka: 12345678"));
    display.setCursor(0, 52);
    display.println(F("IP: 192.168.4.1"));
    display.display();
  });

  displayMessage("Povezivanje", "WiFi...", "");

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  WiFi.setAutoReconnect(true);

  if (!wifiManager.autoConnect("Termometar-Setup", "12345678")) {
    Serial.println("❌ WiFi timeout - restartujem uređaj!");
    displayMessage("WiFi timeout", "Restart...", "");
    delay(2000);
    ESP.restart();
  }

  if (shouldSaveConfig) {
    strncpy(controllerIP, custom_controller.getValue(), sizeof(controllerIP) - 1);
    controllerIP[sizeof(controllerIP) - 1] = '\0';
    saveConfig();
  }

  wifiConnected = true;
  Serial.println("✅ WiFi povezan! IP: " + WiFi.localIP().toString());
  Serial.printf("📡 URL Kontrolera: %s\n", controllerIP);
  displayMessage("WiFi OK!", WiFi.localIP().toString(), "OTA aktivan");
  delay(1500);
}

// Proveri da li je FLASH taster (D3) držan duže od 5s za reset WiFi-ja
void checkWiFiReset() {
  static unsigned long resetPressStart = 0;
  static bool resetting = false;

  if (digitalRead(FLASH_BUTTON_PIN) == LOW) {
    if (resetPressStart == 0) resetPressStart = millis();
    unsigned long holdTime = millis() - resetPressStart;

    if (holdTime > 5000 && !resetting) {
      resetting = true;
      display.clearDisplay();
      display.setTextSize(2);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(10, 15);
      display.println(F("WIFI"));
      display.setCursor(10, 35);
      display.println(F("RESET..."));
      display.display();
      delay(1000);
      wifiManager.resetSettings();
      ESP.restart();
    }
  } else {
    resetPressStart = 0;
    resetting = false;
  }
}

void saveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic = EEPROM_MAGIC;
  EEPROM.put(0, magic);
  for (int i = 0; i < 80; i++) {
    EEPROM.write(10 + i, controllerIP[i]);
  }
  EEPROM.commit();
  EEPROM.end();
  Serial.printf("💾 Sačuvan URL kontrolera u EEPROM: %s\n", controllerIP);
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == EEPROM_MAGIC) {
    char temp[80];
    for (int i = 0; i < 80; i++) {
      temp[i] = EEPROM.read(10 + i);
    }
    temp[79] = '\0';
    if (strlen(temp) > 5) {
      strncpy(controllerIP, temp, sizeof(controllerIP) - 1);
      controllerIP[sizeof(controllerIP) - 1] = '\0';
      Serial.printf("📥 Učitan URL kontrolera iz EEPROM-a: %s\n", controllerIP);
    }
  } else {
    Serial.printf("⚠️ Koristim podrazumevani URL kontrolera: %s\n", controllerIP);
  }
  EEPROM.end();
}

// ------------------------------------------------------------
// OTA Ažuriranje
// ------------------------------------------------------------
void setupOTA() {
  ArduinoOTA.setHostname("termometar-senzor");

  ArduinoOTA.onStart([]() {
    Serial.println("\n📦 OTA ažuriranje započeto...");
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
    // Žuti deo (gore)
    display.setTextSize(1);
    display.setCursor(22, 4);
    display.print(F("OTA UPDATE..."));
    display.drawLine(0, 18, 127, 18, SSD1306_WHITE);
    // Plavi deo (dole)
    display.setTextSize(2);
    display.setCursor(38, 24);
    display.print(pct);
    display.print(F("%"));
    display.drawRect(4, 48, 120, 12, SSD1306_WHITE);
    display.fillRect(6, 50, (pct * 116) / 100, 8, SSD1306_WHITE);
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n✅ OTA uspešno završen! Restartujem...");
    displayMessage("OTA USPESAN!", "Restartovanje...", "");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("\n❌ OTA Greška [%u]\n", error);
    displayError("OTA Greska!");
  });

  ArduinoOTA.begin();

  // Web OTA na /firmware
  httpUpdater.setup(&otaWebServer, "/firmware");
  otaWebServer.on("/", HTTP_GET, []() {
    String info = "Termometar v1.2 (WiFiManager + OTA)\nTemp: " + String(currentTemp, 1) + " C\nKontroler: " + String(controllerIP) + "\nZa Web OTA idite na /firmware\nZa WiFi Reset idite na /reset-wifi";
    otaWebServer.send(200, "text/plain; charset=utf-8", info);
  });
  otaWebServer.on("/reset-wifi", HTTP_POST, []() {
    otaWebServer.send(200, "text/plain; charset=utf-8", "WiFi resetovan! Uredjaj se restartuje u AP mod 'Termometar-Setup'...");
    delay(500);
    wifiManager.resetSettings();
    ESP.restart();
  });
  otaWebServer.on("/reset-wifi", HTTP_GET, []() {
    otaWebServer.send(200, "text/html; charset=utf-8",
      "<form method='POST'><p>Da li zelite da obrisete WiFi podesavanja termometra?</p><button type='submit'>Resetuj WiFi</button></form>");
  });
  otaWebServer.begin();

  Serial.println("✅ OTA spreman (Hostname: termometar-senzor, Web: /firmware)");
}

// ------------------------------------------------------------
// Merenje i slanje
// ------------------------------------------------------------
void measureAndSend() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);

  if (temp == DEVICE_DISCONNECTED_C || temp == 85.0 || temp < -50 || temp > 100) {
    Serial.println("GREŠKA: Senzor nije povezan ili nevalidno očitavanje!");
    displayError("Senzor greška!");
    return;
  }

  currentTemp = temp;
  Serial.printf("Temperatura: %.1f °C\n", currentTemp);

  bool sendSuccess = false;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    sendSuccess = sendTemperature(currentTemp);
  } else {
    wifiConnected = false;
    Serial.println("WiFi nije povezan!");
  }

  displayTemperature(currentTemp, outdoorTemp, wifiConnected, sendSuccess);
}

bool sendTemperature(float temp) {
  HTTPClient http;
  http.setTimeout(2500);

  String url = String(controllerIP);
  if (!url.startsWith("http://")) {
    url = "http://" + url;
  }
  if (!url.endsWith("/update") && url.indexOf("/update") == -1) {
    url += "/update";
  }
  url += "?temp=" + String(temp, 2);

  http.begin(wifiClient, url);
  int httpCode = http.GET();
  bool success = false;

  if (httpCode > 0) {
    Serial.printf("HTTP odgovor: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      StaticJsonDocument<128> doc;
      if (!deserializeJson(doc, payload) && doc.containsKey("outdoorTemp")) {
        outdoorTemp = doc["outdoorTemp"];
        Serial.printf("🌤️ Spoljna temp: %.1f °C\n", outdoorTemp);
      }
      sendCounter++;
      success = true;
    }
  } else {
    Serial.printf("HTTP greška: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return success;
}

// ------------------------------------------------------------
// Displej funkcije
// ------------------------------------------------------------
void displayStartupScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(F("TERMOSTAT"));
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.println(F("Bezicni senzor v1.2"));
  display.setCursor(0, 50);
  display.println(F("Startovanje..."));
  display.display();
  delay(1500);
}

void displayTemperature(float temp, float outdoor, bool wifi, bool sent) {
  display.clearDisplay();

  // DONJI DEO (PLAVI) - Unutrašnja temperatura (velike cifre)
  display.setTextSize(4);
  display.setCursor(0, 36);

  if (temp >= 0 && temp < 10) {
    display.print(F(" "));
  }
  display.print(temp, 1);

  display.setCursor(98, 42);
  display.setTextSize(2);
  display.print(F("o"));
  display.setCursor(110, 46);
  display.print(F("C"));

  // Linija razdvaja žuti i plavi deo
  display.drawLine(0, 20, 127, 20, SSD1306_WHITE);

  // GORNJI DEO (ŽUTI) - Spoljna temperatura i WiFi ikonica
  display.setTextSize(2);
  display.setCursor(5, 0);

  if (outdoor > -99) {
    if (outdoor >= 0 && outdoor < 10) {
      display.print(F(" "));
    }
    display.print(outdoor, 1);
    display.print(F("o"));
  } else {
    display.print(F("--.-"));
  }

  if (wifi) {
    int rssi = WiFi.RSSI();
    drawWiFiIcon(105, 0, rssi);
  } else {
    display.drawLine(105, 0, 115, 10, SSD1306_WHITE);
    display.drawLine(115, 0, 105, 10, SSD1306_WHITE);
  }

  display.display();
}

void drawWiFiIcon(int x, int y, int rssi) {
  int bars = (rssi > -50) ? 4 : (rssi > -60) ? 3 : (rssi > -70) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int barHeight = 3 + (i * 3);
    if (i < bars) {
      display.fillRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
    } else {
      display.drawRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
    }
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
