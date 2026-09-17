/*
 * BikeGPS_Mirror
 * ------------------------------------------------------------
 * Riceve via BLE (peripheral) la telemetria dal telefono e la
 * mostra su un OLED SSD1306 128x64 I2C bicolore
 * (banda GIALLA = prime 16 righe, area BLU = righe 16..63).
 *
 * Mostra UN dato alla volta, grande e centrato nell'area blu.
 * Il pulsante cambia pagina. Di default usa il pulsante BOOT
 * integrato (GPIO0); per un pulsante esterno cambia BUTTON_PIN.
 *
 * Payload ricevuto (binario little-endian, 14 byte):
 *   off 0  u16 velocità       (0.1 km/h)
 *   off 2  u32 distanza       (0.01 km)
 *   off 6  u16 tempo movimento(s)
 *   off 8  i16 quota          (m)
 *   off 10 i8  pendenza       (0.5 %)
 *   off 11 u8  satelliti
 *   off 12 u16 velocità max   (0.1 km/h)
 *
 * In piu' l'ESP32 fa da CENTRAL BLE verso la cintura cardio
 * (Heart Rate Service 0x180D) e ne mostra il battito in locale
 * (pagina BATTITO). Il GPS resta del telefono.
 *
 * Librerie: NimBLE-Arduino (>=2.x), Adafruit SSD1306, Adafruit GFX
 * Scheda:   ESP32 (qualsiasi variante con BLE e I2C)
 *
 * Collegamenti OLED:
 *   VCC -> 3V3   GND -> GND
 *   SDA -> I2C_SDA   SCL -> I2C_SCL   (definiti piu' sotto)
 *   ESP32 classico: SDA=21, SCL=22
 *   ESP32-C3:       SDA=6,  SCL=7
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>

#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C

// Banda gialla: righe 0..15. Area blu: righe 16..63.
#define YELLOW_H 16
#define BLUE_Y   YELLOW_H
#define BLUE_H   (SCREEN_H - YELLOW_H)   // 48

// Pulsante cambio pagina (attivo basso, pull-up interno).
// - ESP32 classico: GPIO0 = pulsante BOOT integrato
// - ESP32-C3:       GPIO9 = pulsante BOOT integrato
// Per un pulsante esterno cambia BUTTON_PIN.

// Pin I2C: sul C3 i default 8/9 sono pin di strapping/BOOT, quindi uso 6/7.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #define BUTTON_PIN 9
  #define I2C_SDA 6
  #define I2C_SCL 7
#else
  #define BUTTON_PIN 0
  #define I2C_SDA 21
  #define I2C_SCL 22
#endif

#define SERIAL_DEBUG 1

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// UUID condivisi con l'app Android
#define SERVICE_UUID   "0000a001-0000-1000-8000-00805f9b34fb"
#define TELEMETRY_UUID "0000a002-0000-1000-8000-00805f9b34fb"

static volatile bool bleConnected = false;

struct Telemetry {
  float    spd = 0;      // km/h
  float    dst = 0;      // km
  uint32_t mov = 0;      // s
  int      alt = 0;      // m
  float    slp = 0;      // %
  int      sat = 0;
  float    mx = 0;       // km/h
  uint32_t lastRx = 0;   // millis dell'ultimo pacchetto

  // cardio letto localmente dall'ESP32 (non arriva dal telefono)
  int      hr = 0;         // bpm
  bool     hrContact = false;
  uint32_t hrLastRx = 0;   // millis dell'ultimo battito
} tel;

enum Page { P_SPEED, P_DIST, P_TIME, P_AVG, P_MAX, P_ALT, P_SLOPE, P_SAT, P_HR, PAGE_COUNT };
static uint8_t page = P_SPEED;

// ---------------------------------------------------------------- parsing
// Pacchetto binario little-endian da 14 byte (vedi app Android).
struct __attribute__((packed)) TelemetryPacket {
  uint16_t spd10;
  uint32_t dist100;
  uint16_t mov;
  int16_t  alt;
  int8_t   slp2;
  uint8_t  sat;
  uint16_t max10;
};

static void parsePayload(const uint8_t *data, size_t len) {
  if (len < sizeof(TelemetryPacket)) return;
  TelemetryPacket p;
  memcpy(&p, data, sizeof(p));

  tel.spd = p.spd10 / 10.0f;
  tel.dst = p.dist100 / 100.0f;
  tel.mov = p.mov;
  tel.alt = p.alt;
  tel.slp = p.slp2 / 2.0f;
  tel.sat = p.sat;
  tel.mx  = p.max10 / 10.0f;
  tel.lastRx = millis();

#if SERIAL_DEBUG
  Serial.printf("rx spd=%.1f dst=%.2f mov=%lu alt=%d slp=%.1f sat=%d mx=%.1f\n",
                tel.spd, tel.dst, (unsigned long)tel.mov, tel.alt, tel.slp, tel.sat, tel.mx);
#endif
}

// ---------------------------------------------------------------- BLE
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
#if SERIAL_DEBUG
    Serial.printf("BLE connesso: %s\n", connInfo.getAddress().toString().c_str());
#endif
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleConnected = false;
    tel.spd = 0;
#if SERIAL_DEBUG
    Serial.printf("BLE disconnesso (reason %d), ri-advertising\n", reason);
#endif
    NimBLEDevice::startAdvertising();
  }
};

class TelemetryCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *ch, NimBLEConnInfo &connInfo) override {
    std::string v = ch->getValue();
    parsePayload(reinterpret_cast<const uint8_t *>(v.data()), v.size());
  }
};

// ---------------------------------------------------------------- cardio (central BLE)
// L'ESP32 legge la cintura cardio DIRETTAMENTE (Heart Rate Service 0x180D)
// mentre resta peripheral verso il telefono. Il battito resta locale.
#define HR_SERVICE_UUID "0000180d-0000-1000-8000-00805f9b34fb"
#define HR_MEAS_UUID    "00002a37-0000-1000-8000-00805f9b34fb"

enum HrState { HR_IDLE, HR_SCANNING, HR_CONNECTING, HR_READY };
static HrState hrState = HR_IDLE;
static NimBLEClient *hrClient = nullptr;
static NimBLEAddress hrAddress;
static volatile bool hrFound = false;
static uint32_t hrNextTry = 0;

static void parseHeartRate(const uint8_t *d, size_t n) {
  if (n < 2) return;
  uint8_t flags = d[0];
  int bpm;
  if (flags & 0x01) {                 // battito su 16 bit
    if (n < 3) return;
    bpm = d[1] | (d[2] << 8);
  } else {
    bpm = d[1];
  }
  bool contactSupported = flags & 0x04;
  tel.hr = bpm;
  tel.hrContact = contactSupported ? (flags & 0x02) : true;
  tel.hrLastRx = millis();
#if SERIAL_DEBUG
  Serial.printf("hr bpm=%d contatto=%d\n", bpm, tel.hrContact);
#endif
}

static void hrNotify(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  parseHeartRate(d, n);
}

class HrScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (dev->isAdvertisingService(NimBLEUUID(HR_SERVICE_UUID))) {
      hrAddress = dev->getAddress();
      hrFound = true;
#if SERIAL_DEBUG
      Serial.printf("cardio trovato: %s [%s]\n",
                    dev->haveName() ? dev->getName().c_str() : "?",
                    dev->getAddress().toString().c_str());
#endif
    }
#if SERIAL_DEBUG
    else if (dev->haveName()) {
      Serial.printf("scan: %s [%s]\n", dev->getName().c_str(),
                    dev->getAddress().toString().c_str());
    }
#endif
  }
};
static HrScanCallbacks hrScanCallbacks;

static bool hrConnect() {
  if (hrClient == nullptr) hrClient = NimBLEDevice::createClient();
  if (hrClient == nullptr) {
#if SERIAL_DEBUG
    Serial.println("hr: createClient null");
#endif
    return false;
  }
#if SERIAL_DEBUG
  Serial.println("hr: connessione...");
#endif
  if (!hrClient->connect(hrAddress)) {
#if SERIAL_DEBUG
    Serial.println("hr: connect fallita");
#endif
    NimBLEDevice::deleteClient(hrClient);
    hrClient = nullptr;
    return false;
  }
#if SERIAL_DEBUG
  Serial.println("hr: connesso");
#endif
  NimBLERemoteService *svc = hrClient->getService(HR_SERVICE_UUID);
  if (svc == nullptr) {
#if SERIAL_DEBUG
    Serial.println("hr: servizio mancante");
#endif
    hrClient->disconnect();
    return false;
  }
  NimBLERemoteCharacteristic *ch = svc->getCharacteristic(HR_MEAS_UUID);
  if (ch == nullptr) {
#if SERIAL_DEBUG
    Serial.println("hr: char mancante");
#endif
    hrClient->disconnect();
    return false;
  }
  if (!ch->subscribe(true, hrNotify)) {
#if SERIAL_DEBUG
    Serial.println("hr: subscribe fallita");
#endif
    hrClient->disconnect();
    return false;
  }
  // Ora che le notifiche sono attive passo all'intervallo lungo (~1 s)
  // che la Geonaute pretende; la discovery resta veloce.
  hrClient->updateConnParams(800, 800, 0, 600);
#if SERIAL_DEBUG
  Serial.println("cardio pronto");
#endif
  return true;
}

static void hrTask() {
  switch (hrState) {
    case HR_IDLE:
      if (millis() >= hrNextTry) {
        hrFound = false;
        hrState = HR_SCANNING;
      }
      break;

    case HR_SCANNING: {
      NimBLEScan *scan = NimBLEDevice::getScan();
      if (!hrFound && !scan->isScanning()) {
        scan->setScanCallbacks(&hrScanCallbacks, false);
        scan->setActiveScan(true);
        bool ok = scan->start(0, true, false);   // continua finche' non trovata
#if SERIAL_DEBUG
        if (!ok) Serial.println("scan cardio non avviato");
#endif
      }
      if (hrFound) {
        scan->stop();
        hrState = HR_CONNECTING;
      }
      break;
    }

    case HR_CONNECTING:
      if (hrConnect()) {
        hrState = HR_READY;
      } else {
        hrNextTry = millis() + 5000;
        hrState = HR_IDLE;
      }
      break;

    case HR_READY:
      if (hrClient == nullptr || !hrClient->isConnected()) {
        if (hrClient != nullptr) {
          NimBLEDevice::deleteClient(hrClient);
          hrClient = nullptr;
        }
        tel.hr = 0;
        hrNextTry = millis() + 2000;
        hrState = HR_IDLE;
      }
      break;
  }
}

// ---------------------------------------------------------------- pagine
static const char *pageLabel(uint8_t p) {
  switch (p) {
    case P_SPEED: return "VELOCITA'  km/h";
    case P_DIST:  return "DISTANZA   km";
    case P_TIME:  return "TEMPO";
    case P_AVG:   return "MEDIA      km/h";
    case P_MAX:   return "MAX        km/h";
    case P_ALT:   return "QUOTA      m";
    case P_SLOPE: return "PENDENZA   %";
    case P_SAT:   return "SATELLITI";
    case P_HR:    return "BATTITO    bpm";
  }
  return "";
}

static void pageValue(uint8_t p, char *buf, size_t n) {
  switch (p) {
    case P_SPEED: snprintf(buf, n, "%.1f", tel.spd); break;
    case P_DIST:  snprintf(buf, n, "%.2f", tel.dst); break;
    case P_TIME: {
      uint32_t t = tel.mov;
      snprintf(buf, n, "%lu:%02lu:%02lu",
               (unsigned long)(t / 3600), (unsigned long)((t % 3600) / 60), (unsigned long)(t % 60));
      break;
    }
    case P_AVG: {
      float avg = (tel.mov > 0) ? tel.dst / (tel.mov / 3600.0f) : 0.0f;
      snprintf(buf, n, "%.1f", avg);
      break;
    }
    case P_MAX:   snprintf(buf, n, "%.1f", tel.mx); break;
    case P_ALT:   snprintf(buf, n, "%d", tel.alt); break;
    case P_SLOPE: snprintf(buf, n, "%+.1f", tel.slp); break;
    case P_SAT:   snprintf(buf, n, "%d", tel.sat); break;
    case P_HR:
      if (tel.hr > 0) snprintf(buf, n, "%d", tel.hr);
      else            snprintf(buf, n, "--");
      break;
    default:      buf[0] = '\0';
  }
}

static int textWidth(const char *s, uint8_t size) {
  return (int)strlen(s) * 6 * size;   // font di default: 5px + 1 di spaziatura
}

// ---------------------------------------------------------------- display
static void drawScreen() {
  bool live;
  if (page == P_HR) live = tel.hrLastRx != 0 && (millis() - tel.hrLastRx <= 5000);
  else              live = bleConnected && tel.lastRx != 0 && (millis() - tel.lastRx <= 5000);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- banda gialla: etichetta + indicatore pagina ---
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(pageLabel(page));

  char pg[8];
  snprintf(pg, sizeof(pg), "%d/%d", page + 1, (int)PAGE_COUNT);
  display.setCursor(SCREEN_W - textWidth(pg, 1), 0);
  display.print(pg);

  // --- area blu: valore grande, centrato ---
  char val[24];
  if (live) pageValue(page, val, sizeof(val));
  else      strcpy(val, "---");

  uint8_t size = 6;
  while (size > 1 && textWidth(val, size) > SCREEN_W - 8) size--;

  int w = textWidth(val, size) - size;    // togli l'ultima spaziatura
  int h = 8 * size;
  display.setTextSize(size);
  display.setCursor((SCREEN_W - w) / 2, BLUE_Y + (BLUE_H - h) / 2);
  display.print(val);

  // --- stato BLE / cardio in basso a destra nell'area blu ---
  if (page == P_HR) {
    if (!live) {
      display.setTextSize(1);
      display.setCursor(SCREEN_W - textWidth("no HR", 1), SCREEN_H - 8);
      display.print("no HR");
    }
  } else if (!bleConnected) {
    display.setTextSize(1);
    display.setCursor(SCREEN_W - textWidth("no BLE", 1), SCREEN_H - 8);
    display.print("no BLE");
  }

  display.display();
}

// ---------------------------------------------------------------- pulsante
static void handleButton() {
  static bool last = HIGH;
  static uint32_t lastChange = 0;
  bool now = digitalRead(BUTTON_PIN);
  if (now != last && (millis() - lastChange) > 40) {
    lastChange = millis();
    last = now;
    if (now == LOW) {                 // fronte di pressione
      page = (page + 1) % PAGE_COUNT;
    }
  }
}

// ---------------------------------------------------------------- setup
void setup() {
#if SERIAL_DEBUG
  Serial.begin(115200);
  delay(200);
#endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
#if SERIAL_DEBUG
    Serial.println("SSD1306 non trovato");
#endif
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("BikeGPS");
  display.println("avvio BLE...");
  display.display();

  NimBLEDevice::init("BikeGPS");
  NimBLEDevice::setMTU(64);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(SERVICE_UUID);
  NimBLECharacteristic *ch = service->createCharacteristic(
      TELEMETRY_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  ch->setCallbacks(new TelemetryCallbacks());

  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName("BikeGPS");
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();

#if SERIAL_DEBUG
  Serial.println("BikeGPS pronto, in attesa del telefono");
#endif
}

// ---------------------------------------------------------------- loop
void loop() {
  handleButton();
  hrTask();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 200) {
    lastDraw = millis();
    drawScreen();
  }
  delay(5);
}
