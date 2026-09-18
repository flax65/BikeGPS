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

#include <TFT_eSPI.h>
#include <NimBLEDevice.h>

// Display: LILYGO T-Display-S3 (ST7789 170x320, bus parallelo 8 bit).
// Setup in tft_setup.h nella cartella dello sketch.
TFT_eSPI tft = TFT_eSPI();

// Pin della scheda (T-Display-S3).
#define PIN_BTN        14    // pulsante destro: cambia pagina
#define PIN_BTN2        0    // pulsante sinistro (BOOT), usato dall'altro sketch
#define PIN_LCD_POWER  15    // alimentazione pannello: HIGH o resta nero
#define PIN_BL         38    // retroilluminazione
#define PIN_BAT         4    // partitore batteria (1:2)
#define HDR_H          26    // altezza della riga di stato
#define ROTATION        1    // 0/2 verticale, 1/3 orizzontale

#define SERIAL_DEBUG 1

static int W, H;                 // dimensioni reali del pannello
static uint16_t C_BG, C_FG, C_RED;   // colori (Tokyo Night)

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
// Disegno incrementale: lo sfondo si rifa' solo al cambio pagina, poi si
// aggiorna solo cio' che cambia (setTextPadding cancella in una passata).
static uint8_t lastPageDrawn = 255;
static char lastVal[24] = "";
static char lastPg[8]   = "";
static char lastSt[8]   = "";

static void drawScreen() {
  bool live;
  if (page == P_HR) live = tel.hrLastRx != 0 && (millis() - tel.hrLastRx <= 5000);
  else              live = bleConnected && tel.lastRx != 0 && (millis() - tel.lastRx <= 5000);

  // sfondo + etichetta: solo al cambio pagina
  if (page != lastPageDrawn) {
    tft.fillScreen(C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_FG, C_BG);
    tft.setTextPadding(190);
    tft.drawString(pageLabel(page), 4, 5, 2);
    tft.setTextPadding(0);
    lastPageDrawn = page;
    lastVal[0] = lastPg[0] = lastSt[0] = 0;   // forza il ridisegno
  }

  // indicatore pagina (n/N)
  char pg[8];
  snprintf(pg, sizeof(pg), "%d/%d", page + 1, (int)PAGE_COUNT);
  if (strcmp(pg, lastPg) != 0) {
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(C_FG, C_BG);
    tft.setTextPadding(60);
    tft.drawString(pg, W - 4, 5, 2);
    tft.setTextPadding(0);
    strncpy(lastPg, pg, sizeof(lastPg) - 1);
    lastPg[sizeof(lastPg) - 1] = 0;
  }

  // valore grande: font piu' grande che entra, cancellazione in una passata
  char val[24];
  if (live) pageValue(page, val, sizeof(val));
  else      strcpy(val, "---");
  if (strcmp(val, lastVal) != 0) {
    uint8_t f = 7;
    if (tft.textWidth(val, 7) > W - 16) f = 4;
    if (tft.textWidth(val, 4) > W - 16) f = 2;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_FG, C_BG);
    tft.setTextPadding(W - 12);
    tft.drawString(val, W / 2, HDR_H + (H - HDR_H) / 2, f);
    tft.setTextPadding(0);
    strncpy(lastVal, val, sizeof(lastVal) - 1);
    lastVal[sizeof(lastVal) - 1] = 0;
  }

  // stato BLE / cardio (area a destra in basso); stringa vuota = cancella
  char st[8] = "";
  if (page == P_HR) { if (!live) strcpy(st, "no HR"); }
  else if (!bleConnected) strcpy(st, "no BLE");
  if (strcmp(st, lastSt) != 0) {
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(C_RED, C_BG);
    tft.setTextPadding(80);
    tft.drawString(st, W - 4, H - 4, 2);
    tft.setTextPadding(0);
    strncpy(lastSt, st, sizeof(lastSt) - 1);
    lastSt[sizeof(lastSt) - 1] = 0;
  }
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------- pulsante
static void handleButton() {
  static bool last = HIGH;
  static uint32_t lastChange = 0;
  bool now = digitalRead(PIN_BTN);
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
  { uint32_t t = millis(); while (millis() - t < 200) vTaskDelay(1); }
#endif

  pinMode(PIN_BTN, INPUT_PULLUP);

  // pannello: alimentazione, init, orientamento, retroilluminazione
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);
  tft.init();
  tft.setRotation(ROTATION);
  W = tft.width();
  H = tft.height();
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  C_BG  = tft.color565(0x16, 0x17, 0x20);
  C_FG  = tft.color565(0xc0, 0xca, 0xf5);
  C_RED = tft.color565(0xf7, 0x76, 0x8e);

  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_FG, C_BG);
  tft.drawString("BikeGPS", 8, 8, 4);
  tft.drawString("avvio BLE...", 8, 44, 2);

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
  vTaskDelay(1);
}
