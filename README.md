# 🏠 ESP8266 Pametni Termostat & Kontroler Grejanja

Kompletan sistem za automatizovano upravljanje centralnim grejanjem baziran na dva **ESP8266** mikrokontrolera:
1. **Glavni kontroler grejanja** sa relejem za kotao, naprednim sedmodnevnim rasporedom, Web interfejsom (LittleFS) i prognozom vremena (OpenWeatherMap).
2. **Bežični sobni termometar** sa DS18B20 senzorom i žuto-plavim I2C OLED displejom.

Oba uređaja koriste **WiFiManager** (nema upisanih šifara u kodu!), automatski se povezuju na lokalnu mrežu i podržavaju **OTA (Over-The-Air)** bežično ažuriranje.

---

## 📐 Arhitektura sistema i komunikacija

```text
 ┌──────────────────────────────────────┐                     ┌──────────────────────────────────────┐
 │         BEŽIČNI TERMOMETAR           │                     │          KONTROLER GREJANJA          │
 │              (ESP8266)               │                     │              (ESP8266)               │
 │                                      │     HTTP GET        │                                      │
 │ • DS18B20 senzor (D4 / GPIO2)        │ ──────────────────> │ • Relej kotla (D1 / GPIO5)           │
 │ • I2C OLED 128x64 (D2/SDA, D1/SCL)   │   /update?temp=...  │ • LittleFS Web Server (Port 80)      │
 │ • WiFiManager AP: "Termometar-Setup" │                     │ • WiFiManager AP: "Termostat-Setup"  │
 │ • mDNS: termometar-senzor.local      │ <────────────────── │ • mDNS: kontroler-grejanja.local     │
 │ • Web OTA: /firmware                 │     JSON odgovor    │ • NTP vreme (Automatski CET/CEST)    │
 └──────────────────────────────────────┘   {"outdoorTemp":X} │ • OpenWeatherMap API prognoza        │
                                                              └──────────────────────────────────────┘
```

---

## 📶 Podešavanja mreže (WiFi & Network Configuration)

Zahvaljujući biblioteci **WiFiManager**, u samom kodu se **ne nalaze hardkodovana imena WiFi mreža niti lozinke**. Uređaji su 100% bezbedni za objavljivanje na GitHub-u.

### 1. Prvo povezivanje Kontrolera Grejanja na WiFi
1. Uključite kontroler grejanja u napajanje.
2. Ukoliko u memoriji nema sačuvanu WiFi mrežu, kontroler automatski kreira pristupnu tačku (Access Point):
   * **Naziv mreže (SSID):** `Termostat-Setup`
   * **Lozinka:** `12345678`
   * **IP adresa portala:** `192.168.4.1`
3. Povežite se telefonom ili računarom na mrežu `Termostat-Setup`.
4. Otvoriće se automatski *Captive Portal* (ili ručno u pregledaču otvorite `http://192.168.4.1`):
   * Kliknite na **Configure WiFi**.
   * Izaberite vašu kućnu WiFi mrežu i unesite njenu lozinku.
   * *(Opciono)* Možete uneti željenu statičku IP adresu (npr. `192.168.1.75`), Gateway i Subnet.
   * Kliknite na **Save**. Kontroler se restartuje i povezuje na vaš kućni WiFi.

### 2. Prvo povezivanje Bežičnog Termometra na WiFi
1. Uključite termometar u napajanje.
2. Na žuto-plavom OLED displeju termometra pojaviće se instrukcije za konfiguraciju:
   * **Naziv mreže (SSID):** `Termometar-Setup`
   * **Lozinka:** `12345678`
   * **IP adresa portala:** `192.168.4.1`
3. Povežite se telefonom ili laptopom na mrežu `Termometar-Setup`.
4. U portalu izaberite kućnu WiFi mrežu, unesite njenu lozinku i popunite polje:
   * **URL Kontrolera:** unesite IP adresu ili mDNS URL kontrolera (npr. `http://192.168.1.75/update` ili `http://kontroler-grejanja.local/update`).
5. Kliknite na **Save**. Termometar čuva URL u EEPROM memoriji, povezuje se na WiFi i počinje slanje temperature na svakih 30 sekundi.

### 3. Preporučena statička IP adresa za Kontroler
Da bi termometar uvek pouzdano znao gde šalje očitavanja, preporučuje se:
* Ili da u ruteru dodelite fiksni DHCP zakup (Static DHCP Lease) za MAC adresu kontrolera na npr. `192.168.1.75`.
* Ili da u polju za URL kontrolera koristite mDNS adresu: `http://kontroler-grejanja.local/update`.

### 4. Kako resetovati WiFi podešavanja (Reset na fabrička podešavanja)

Ako promenite ruter ili WiFi šifru, mrežna podešavanja možete resetovati u svakom trenutku:

| Uređaj | Fizički reset (Taster) | Reset preko mreže / Web-a |
|---|---|---|
| **Kontroler grejanja** | Držite fizički taster na **D6 (GPIO12)** duže od **10 sekundi**. Sve 3 LED lampice će ubrzano treperiti, obrisati WiFi memoriju i pokrenuti AP `Termostat-Setup`. | U Web interfejsu idite na tab **🔧 Sistem** i kliknite **🔄 Resetuj WiFi podešavanja** (ili pošaljite HTTP POST na `/api/reset-wifi`). |
| **Bežični termometar** | Držite taster **FLASH na ploči (D3 / GPIO0)** duže od **5 sekundi**. Na OLED displeju će se ispisati `WIFI RESET...` i ponovo kreirati AP `Termometar-Setup`. | U bilo kom pregledaču otvorite adresu `http://<IP_TERMOMETRA>/reset-wifi`. |

---

## 🌐 Mrežni API Endpoints

### Kontroler Grejanja (Port 80)
| Metoda | Ruta | Opis |
|---|---|---|
| `GET` | `/` | Služi glavni web interfejs iz LittleFS memorije (`index.html`) |
| `GET` | `/update?temp=22.5` | Prijem temperature sa termometra (vraća JSON sa spoljnom temperaturom) |
| `GET` | `/api/status` | Vraća kompletno stanje: trenutna temp, cilj, histerezis, grejanje ON/OFF, uptime, RAM |
| `GET` | `/api/manual?mode=auto\|on\|off` | Promena režima rada (automatski, ručno grejanje ili ručno forsirano gašenje) |
| `GET` | `/api/manual?temp=24.5` | Podešavanje ciljne temperature u ručnom režimu |
| `GET` | `/api/schedule?day=0..6` | Vraća raspored za izabrani dan u nedelji (3 perioda + noćna temp) |
| `POST`| `/api/schedule` | Snimanje rasporeda za određeni dan u EEPROM |
| `POST`| `/api/hysteresis` | Snimanje praga uključivanja i isključivanja kotla (histerezis) |
| `GET` | `/api/system` | Dijagnostika: jačina WiFi signala (RSSI), slobodan Heap RAM, uptime |
| `POST`| `/api/reset-wifi` | Brisanje sačuvane WiFi mreže i restart u AP režim |
| `POST`| `/api/restart` | Meki restart kontrolera |
| `GET/POST` | `/firmware` | Web OTA ažuriranje koda (`firmware.bin`) ili web stranice (`littlefs.bin`) |

### Termometar (Port 80)
| Metoda | Ruta | Opis |
|---|---|---|
| `GET` | `/` | Osnovni info o senzoru i trenutnoj temperaturi |
| `GET/POST` | `/firmware` | Web OTA bežično ažuriranje firmvera termometra |
| `GET/POST` | `/reset-wifi` | Brisanje sačuvanih WiFi podataka i prelazak u AP konfiguraciju |

---

## 🔌 Šema pinova (Pinout)

### 1. Kontroler Grejanja (NodeMCU v2 / ESP8266)
| Pin na ploči | GPIO | Funkcija | Opis povezivanja |
|---|---|---|---|
| **D1** | GPIO5 | Relej kotla | Upravljački vod relej modula (Aktivno na `LOW`) |
| **D6** | GPIO12 | Fizičko dugme | Taster ka GND (kratko = ON/OFF, 3-10s = AUTO, >10s = WiFi Reset) |
| **D5** | GPIO14 | Zelena LED | Svetli kada je aktivan **AUTO** režim rada |
| **D3** | GPIO0 | Crvena LED | Svetli kada je aktivan **RUČNI** režim rada |
| **D7** | GPIO13 | Upozorenje LED | Treperi ako je baterija slaba ili senzor ne šalje podatke |

### 2. Bežični Termometar (NodeMCU v2 / ESP8266)
| Pin na ploči | GPIO | Funkcija | Opis povezivanja |
|---|---|---|---|
| **D4** | GPIO2 | DS18B20 Data | Data linija senzora temperature (obavezan 4.7kΩ otpornik ka 3.3V) |
| **D2** | GPIO4 | I2C SDA | SDA pin OLED 0.96" displeja |
| **D1** | GPIO5 | I2C SCL | SCL pin OLED 0.96" displeja |
| **D3 / FLASH** | GPIO0 | Fabrički taster | Taster na NodeMCU ploči (drži >5s za WiFi Reset) |

---

## 🚀 PlatformIO Uputstvo za Flešovanje

Projekat je organizovan kao **multi-environment** PlatformIO radni prostor:

```text
termostat_platformio/
├── platformio.ini              # Centralna konfiguracija za oba uređaja
├── README.md                   # Kompletna dokumentacija i mrežna uputstva
├── LICENSE                     # MIT Licenca
├── data/
│   └── index.html              # Jedinstveni Web interfejs (HTML + CSS + JS) za LittleFS
└── src/
    ├── kontroler/
    │   └── main.cpp            # Izvorni kod kontrolera grejanja
    └── termometar/
        └── main.cpp            # Izvorni kod termometra (v1.2 sa WiFiManager-om)
```

### 1. Prvo flešovanje preko USB kabla

Povežite željeni uređaj na računar USB kablom, otvorite folder u **VS Code** i pokrenite:

```bash
# Za Kontroler Grejanja:
pio run -e kontroler -t upload          # 1. Flešovanje firmvera
pio run -e kontroler -t uploadfs        # 2. Upis data/index.html u LittleFS memoriju

# Za Bežični Termometar:
pio run -e termometar -t upload         # Flešovanje firmvera termometra
```

### 2. Bežično ažuriranje (OTA) — nakon prvog USB flešovanja

Kada su uređaji jednom povezani na vašu WiFi mrežu, USB kabl vam više nije potreban:

```bash
# Bežično ažuriranje Kontrolera:
pio run -e kontroler_ota -t upload
pio run -e kontroler_ota -t uploadfs    # Bežično ažuriranje web stranice

# Bežično ažuriranje Termometra:
pio run -e termometar_ota -t upload
```

*(Napomena: Za bežično flešovanje preko PlatformIO proverite da li je u `platformio.ini` parametar `upload_port` postavljen na IP adresu ili mDNS ime vašeg uređaja).*

---

## 🛡️ Sigurnost i Fail-Safe zaštita

* **Sprečavanje pregrevanja:** Ako termometar iz bilo kog razloga prestane da šalje podatke duže od 15 minuta (nestanak struje na senzoru, prekid WiFi veze), kontroler **automatski gasi relej grejanja** kako kotao ne bi ostao uključen u nedogled.
* **Zaštita od lažnih očitavanja:** Ignorišu se nerealne vrednosti i fabrički power-on reset podatak senzora DS18B20 (`85.0°C`).
* **Histerezis:** Podesivi pragovi sprečavaju prečesto paljenje i gašenje gorionika/pumpe, čuvajući opremu od habanja.

---

## 📄 Licenca

Ovaj projekat je izdat pod otvorenim kodom pod uslovima [MIT Licence](LICENSE).
