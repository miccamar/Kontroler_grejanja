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
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// WiFi podešavanja
const char* ssid = "Eeeee";         // Promeni po potrebi
const char* password = "_AndroiD_"; // Promeni po potrebi

// IP adresa kontrolera (ESP #2)
const char* controllerIP = "http://192.168.1.75/update"; // Promeni na IP kontrolera!

// Interval očitavanja i slanja temperature (30 sekundi, neblokirajući)
const unsigned long SEND_INTERVAL_MS = 30000UL;

// DS18B20 na pinu D4 (GPIO2)
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// OLED displej 128x64
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

WiFiClient wifiClient;

// Web server za Web OTA (http://<IP_TERMOMETRA>/firmware)
ESP8266WebServer otaWebServer(80);
ESP8266HTTPUpdateServer httpUpdater;

// Promenljive
float currentTemp = 0.0;
float outdoorTemp = -99.0;  // Spoljna temperatura sa kontrolera
bool wifiConnected = false;
int sendCounter = 0;
unsigned long lastSendTime = 0;
unsigned long lastWiFiCheck = 0;

// Deklaracije funkcija (potrebno za PlatformIO C++ kompajler)
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

  Serial.println("\n\n=== BEŽIČNI SENZOR SA OLED (v1.2 + OTA) ===");

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

  // Poveži se na WiFi (sa automatskim reconnect-om)
  displayMessage("Povezivanje", "WiFi...", "");
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_MODEM_SLEEP); // Manje zagrevanje čipa uz stalnu WiFi vezu
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(ssid, password);
  Serial.print("Povezivanje na WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nPovezano!");
    Serial.print("IP adresa senzora: ");
    Serial.println(WiFi.localIP());

    displayMessage("WiFi OK!", "IP: " + WiFi.localIP().toString(), "OTA: /firmware");
    delay(2000);
  } else {
    wifiConnected = false;
    Serial.println("\nNije uspelo povezivanje!");
    displayMessage("WiFi greška!", "Offline mod", "");
    delay(2000);
  }

  // Pokreni ArduinoOTA i Web OTA
  setupOTA();

  // Odmah izvrši prvo merenje i slanje bez čekanja 30 sekundi
  measureAndSend();
  lastSendTime = millis();
}

void loop() {
  // 1. Uvek obrađuj OTA zahteve (nema više blokirajućeg delay(30000)!)
  ArduinoOTA.handle();
  otaWebServer.handleClient();

  unsigned long now = millis();

  // 2. Proveri WiFi konekciju na svakih 10 sekundi ako je pala
  if (now - lastWiFiCheck >= 10000UL) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      WiFi.reconnect();
    } else {
      wifiConnected = true;
    }
  }

  // 3. Očitaj i pošalji temperaturu svakih 30 sekundi
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    measureAndSend();
  }

  delay(10);
}

// Inicijalizacija ArduinoOTA i Web OTA (/firmware)
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
    // Plavi deo (dole) - procenat i progress bar
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

  // Web OTA dostupan u pregledaču na http://<IP_TERMOMETRA>/firmware
  httpUpdater.setup(&otaWebServer, "/firmware");
  otaWebServer.on("/", HTTP_GET, []() {
    String info = "Termometar v1.2 (OTA) | Temp: " + String(currentTemp, 1) + " C | Za Web OTA idite na /firmware";
    otaWebServer.send(200, "text/plain; charset=utf-8", info);
  });
  otaWebServer.begin();

  Serial.println("✅ OTA spreman (Hostname: termometar-senzor, Web: /firmware)");
}

// Očitaj senzor, pošalji kontroleru i osveži OLED
void measureAndSend() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);

  // Proveri da li je očitavanje validno (uključujući 85.0°C power-on reset grešku)
  if (temp == DEVICE_DISCONNECTED_C || temp == 85.0 || temp < -50 || temp > 100) {
    Serial.println("GREŠKA: Senzor nije povezan ili nevalidno očitavanje!");
    displayError("Senzor greška!");
    return;
  }

  currentTemp = temp;

  Serial.print("Temperatura: ");
  Serial.print(currentTemp, 1);
  Serial.println("°C");

  // Pošalji temperaturu kontroleru ako je WiFi aktivan
  bool sendSuccess = false;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    sendSuccess = sendTemperature(currentTemp);
  } else {
    wifiConnected = false;
    Serial.println("WiFi nije povezan!");
  }

  // Prikaži na displeju
  displayTemperature(currentTemp, outdoorTemp, wifiConnected, sendSuccess);
}

// Pošalji temperaturu kontroleru
bool sendTemperature(float temp) {
  HTTPClient http;
  http.setTimeout(2500); // Kratak timeout da ne blokira OLED i OTA ako je kontroler ugašen

  String url = String(controllerIP) + "?temp=" + String(temp, 2);

  http.begin(wifiClient, url);
  int httpCode = http.GET();

  bool success = false;

  if (httpCode > 0) {
    Serial.printf("HTTP odgovor: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Poslato uspešno");

      // Parsiranje JSON odgovora da dobijemo spoljašnju temperaturu
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error && doc.containsKey("outdoorTemp")) {
        outdoorTemp = doc["outdoorTemp"];
        Serial.print("🌤️  Spoljna temp: ");
        Serial.print(outdoorTemp, 1);
        Serial.println("°C");
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

// Prikaži startnu animaciju
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
  delay(2000);
}

// Prikaži temperaturu na displeju (optimizovano za žuto-plavi displej)
void displayTemperature(float temp, float outdoor, bool wifi, bool sent) {
  display.clearDisplay();

  // DONJI DEO (PLAVI) - Unutrašnja temperatura (velike cifre)
  display.setTextSize(4);
  display.setCursor(0, 36);

  if (temp >= 0 && temp < 10) {
    display.print(F(" "));
  }
  display.print(temp, 1);

  // Stepen znak
  display.setCursor(98, 42);
  display.setTextSize(2);
  display.print(F("o"));
  display.setCursor(110, 46);
  display.print(F("C"));

  // LINIJA razdvaja žuti (gornji) i plavi (donji) deo
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

  // WiFi ikonica sa jačinom signala (desno gore)
  if (wifi) {
    int rssi = WiFi.RSSI();
    drawWiFiIcon(105, 0, rssi);
  } else {
    // X ako nema WiFi
    display.drawLine(105, 0, 115, 10, SSD1306_WHITE);
    display.drawLine(115, 0, 105, 10, SSD1306_WHITE);
  }

  display.display();
}

// Nacrtaj WiFi ikonicu sa signalom
void drawWiFiIcon(int x, int y, int rssi) {
  int bars = 0;
  if (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;
  else bars = 1;

  for (int i = 0; i < 4; i++) {
    int barHeight = 3 + (i * 3);
    if (i < bars) {
      display.fillRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
    } else {
      display.drawRect(x + (i * 4), y + (12 - barHeight), 2, barHeight, SSD1306_WHITE);
    }
  }
}

// Prikaži poruku
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

// Prikaži grešku
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
