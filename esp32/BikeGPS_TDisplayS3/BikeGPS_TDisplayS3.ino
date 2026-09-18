/*
 * BikeGPS_TDisplayS3
 * ------------------------------------------------------------
 * Mirror GPS su LILYGO T-Display-S3 (ESP32-S3 + ST7789 170x320 parallelo).
 *
 * Stessa parte BLE dello sketch SSD1306 (copy-paste, protocollo identico):
 *   - peripheral BLE "BikeGPS" verso l'app Android (telemetria 14 byte)
 *   - central BLE verso la cintura cardio (Heart Rate Service 0x180D)
 * Cambia solo il disegno: 3 pagine a tutto schermo, pulsanti integrati.
 *
 * Layout (landscape 320x170, rotazione 1):
 *   PAGINA 1 "RIDE"   velocita' gigante (font 7-segment 48px) + distanza,
 *                     tempo, media, max
 *   PAGINA 2 "STATS"  griglia 3x2: dist, tempo, media, max, quota, pendenza
 *   PAGINA 3 "SYS"    griglia 3x2: BLE, cardio, satelliti, batteria,
 *                     heap libero, uptime
 *
 * Pulsanti:
 *   GPIO0  (BTN1, BOOT integrato)  -> pagina successiva
 *   GPIO14 (BTN2)                  -> pagina precedente
 *   GPIO0 tenuto premuto (>0,8 s)  -> accende/spegne la retroilluminazione
 *
 * Payload ricevuto (binario little-endian, 14 byte):
 *   off 0  u16 velocita'      (0.1 km/h)
 *   off 2  u32 distanza       (0.01 km)
 *   off 6  u16 tempo movimento(s)
 *   off 8  i16 quota          (m)
 *   off 10 i8  pendenza       (0.5 %)
 *   off 11 u8  satelliti
 *   off 12 u16 velocita' max  (0.1 km/h)
 *
 * Librerie: NimBLE-Arduino (>=2.x), TFT_eSPI (setup in tft_setup.h locale)
 * Scheda:   esp32:esp32:lilygo_t_display_s3
 *
 * ATTENZIONE: compila e carica con lo STESSO FQBN, altrimenti
 * arduino-cli puo' sbagliare i parametri di flash ("Unexpected chip ID").
 */

#include <TFT_eSPI.h>
#include <NimBLEDevice.h>
#include <Preferences.h>   // soglie e target salvati in flash (NVS)

// ---------------------------------------------------------------- pin scheda
#define PIN_BTN1      0     // pulsante integrato ("sinistro"): modifica i valori
#define PIN_BTN2      14    // pulsante integrato ("destro"): cicla le schermate

// Se col display sul manubrio ti risultano invertiti, scambia questi due define.
#define BTN_SET  PIN_BTN1
#define BTN_PAGE PIN_BTN2

#define DEBOUNCE_MS      50    // anti-rimbalzo dei tastini
#define BUTTON_GUARD_MS  2000  // dopo il boot i tasti sono ignorati per questo tempo
#define PIN_LCD_POWER 15    // LCD_POWER_ON: va portato HIGH o il pannello resta nero
#define PIN_BAT       4     // partitore di misura batteria (1:2)

#define SERIAL_DEBUG 1

#define ROTATION 0          // 0/2 = verticale, 1/3 = orizzontale (cambia il verso)
#define HDR_H 26            // altezza della riga di stato in alto

TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------- colori (Tokyo Night)
static uint16_t C_BG, C_PANEL, C_BORDER, C_FG, C_DIM, C_BLUE, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_BATT;
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

// ---------------------------------------------------------------- UUID condivisi con l'app
#define SERVICE_UUID   "0000a001-0000-1000-8000-00805f9b34fb"
#define TELEMETRY_UUID "0000a002-0000-1000-8000-00805f9b34fb"
#define HR_SERVICE_UUID "0000180d-0000-1000-8000-00805f9b34fb"
#define HR_MEAS_UUID    "00002a37-0000-1000-8000-00805f9b34fb"

static volatile bool bleConnected = false;

// ---------------------------------------------------------------- telemetria
struct Telemetry {
  float    spd = 0;      // km/h
  float    dst = 0;      // km
  uint32_t mov = 0;      // s
  int      alt = 0;      // m
  float    slp = 0;      // %
  int      sat = 0;
  float    mx = 0;       // km/h
  uint32_t lastRx = 0;   // millis dell'ultimo pacchetto BLE

  // cardio letto localmente dall'ESP32 (non arriva dal telefono)
  int      hr = 0;       // bpm
  bool     hrContact = false;
  uint32_t hrLastRx = 0;
} tel;

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

// ---------------------------------------------------------------- BLE (peripheral)
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
#if SERIAL_DEBUG
    Serial.printf("BLE connesso: %s\n", connInfo.getAddress().toString().c_str());
#endif
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    bleConnected = false;
    tel.spd = 0;
#if SERIAL_DEBUG
    Serial.printf("BLE disconnesso (reason %d), ri-advertising\n", reason);
#endif
    NimBLEDevice::startAdvertising();
  }
};

class TelemetryCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *ch, NimBLEConnInfo &) override {
    std::string v = ch->getValue();
    parsePayload(reinterpret_cast<const uint8_t *>(v.data()), v.size());
  }
};

// ---------------------------------------------------------------- cardio (central BLE)
enum HrState { HR_IDLE, HR_SCANNING, HR_CONNECTING, HR_READY };
static HrState hrState = HR_IDLE;
static NimBLEClient *hrClient = nullptr;
static NimBLEAddress hrAddress;
static volatile bool hrFound = false;
static uint32_t hrNextTry = 0;
static volatile int8_t hrConnectResult = -1;   // esito dell'ultimo tentativo (<0 mai, 0 fallita, 1 ok)

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
  }
};
static HrScanCallbacks hrScanCallbacks;

// callback del client cardio: registra la connessione e il MOTIVO della disconnessione
class HrClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override {
#if SERIAL_DEBUG
    Serial.println("hr: client onConnect");
#endif
  }
  void onDisconnect(NimBLEClient *, int reason) override {
#if SERIAL_DEBUG
    Serial.printf("hr: client onDisconnect reason=0x%02X\n", reason);
#endif
  }
};
static HrClientCallbacks hrClientCb;

static bool hrConnect() {
  if (hrClient == nullptr) hrClient = NimBLEDevice::createClient();
  if (hrClient == nullptr) return false;
  hrClient->setClientCallbacks(&hrClientCb, false);   // false: non distruggere il client da solo

  bool ok = false;
  do {
    if (!hrClient->connect(hrAddress)) {
#if SERIAL_DEBUG
      Serial.println("hrConnect: connect() fallito");
#endif
      break;
    }
    NimBLERemoteService *svc = hrClient->getService(HR_SERVICE_UUID);
    if (svc == nullptr) {
#if SERIAL_DEBUG
      Serial.println("hrConnect: servizio 0x180D assente");
#endif
      break;
    }
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(HR_MEAS_UUID);
    if (ch == nullptr) {
#if SERIAL_DEBUG
      Serial.println("hrConnect: characteristic 0x2A37 assente");
#endif
      break;
    }
    if (!ch->subscribe(true, hrNotify)) {
#if SERIAL_DEBUG
      Serial.println("hrConnect: subscribe() fallita");
#endif
      break;
    }
    // Intervallo lungo (~1 s): la Geonaute lo pretende, Android non glielo concede.
    hrClient->updateConnParams(800, 800, 0, 600);
    ok = true;
  } while (0);

  if (!ok) {
    // pulizia COMPLETA: se si e' connesso a meta' il client resterebbe sporco e
    // i tentativi successivi fallirebbero per sempre. Sempre client nuovo.
    NimBLEDevice::deleteClient(hrClient);
    hrClient = nullptr;
  }
  return ok;
}

// transizione di stato con log: rende leggibile la storia nel monitor seriale
static void hrStateTo(int s, const char *why) {   // int: i prototipi auto-generati di Arduino
#if SERIAL_DEBUG                                     // stanno prima della definizione di HrState
  Serial.printf("hr: %d -> %d (%s)\n", (int)hrState, s, why);
#endif
  hrState = (HrState)s;
}

static void hrTask() {
  switch (hrState) {
    case HR_IDLE:
      if (millis() >= hrNextTry) {
        hrFound = false;
        hrStateTo(HR_SCANNING, "avvio scansione");
      }
      break;

    case HR_SCANNING: {
      NimBLEScan *scan = NimBLEDevice::getScan();
      if (!hrFound && !scan->isScanning()) {
        scan->setScanCallbacks(&hrScanCallbacks, false);
        scan->setActiveScan(true);
        scan->start(0, true, false);   // continua finche' non trovata
      }
      if (hrFound) {
        scan->stop();
        hrStateTo(HR_CONNECTING, "fascia trovata");
      }
      break;
    }

    case HR_CONNECTING:
      // Connessione NEL LOOP, come nella versione che funzionava.
      // (connect+subscribe sono sincroni: il display si ferma per la durata)
      hrConnectResult = hrConnect() ? 1 : 0;
      if (hrConnectResult > 0) {
        hrStateTo(HR_READY, "connesso");
      } else {
        hrNextTry = millis() + 5000;
        hrStateTo(HR_IDLE, "connessione fallita");
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
        hrStateTo(HR_IDLE, "disconnesso");
      }
      break;
  }
}

// ---------------------------------------------------------------- batteria
static float battFiltered = 0;
static float battVolts() {
  uint32_t mv = analogReadMilliVolts(PIN_BAT);
  return (mv * 2.0f) / 1000.0f;   // partitore 1:2 della scheda
}
static int battPercent() {
  float v = battFiltered;
  if (v <= 0) return 0;
  int p = (int)((v - 3.30f) / (4.20f - 3.30f) * 100.0f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

// ---------------------------------------------------------------- pagine
enum Page { P_RIDE, P_COST_SPEED, P_COST_BPM, P_SETUP, P_DIAG, PAGE_COUNT };
static uint8_t page = P_RIDE;
static bool backlightOn = true;
static bool freezeDraw = false;   // usato dal test 'r'/'v'/'k' per non sovrascrivere il colore
static uint32_t bootTime = 0;     // usato per ignorare i tasti nei primi istanti dopo il boot

// Pagina successiva/precedente. La SETUP (soglie cardio) non fa parte del giro:
// si apre tenendo premuti entrambi i tasti.
static uint8_t advancePage(uint8_t p, int dir) {
  for (int k = 0; k < PAGE_COUNT; k++) {
    p = (uint8_t)((p + dir + PAGE_COUNT) % PAGE_COUNT);
    if (p != P_SETUP) return p;
  }
  return p;
}

// ---------------------------------------------------------------- allenamento (soglie e target)
// Soglie cardiache = limiti Z1|Z2, Z2|Z3, Z3|Z4, Z4|Z5 (bpm).
// Default per FCmax 170: 102 / 119 / 136 / 153 -> 150 bpm cade nella Z4.
#define ZONE_COUNT 5
#define N_ZLIM     4
static int zoneLim[N_ZLIM] = {102, 119, 136, 153};
static const char *const ZONE_NAMES[ZONE_COUNT] = {"Z1", "Z2", "Z3", "Z4", "Z5"};
static uint16_t zoneCol[ZONE_COUNT];       // riempiti in setup()

static float targetSpeed = 25.0f;          // km/h (pagina COST SPEED)
static int   targetHr    = 150;            // bpm (pagina COST BPM)

// target velocita': intervallo selezionabile e passo del tasto sinistro
#define TGT_SPEED_MIN  25.0f
#define TGT_SPEED_MAX  35.0f
#define TGT_SPEED_STEP  1.0f

// target battito
#define TGT_HR_MIN  80
#define TGT_HR_MAX  200

// riporta il target velocita' nell'intervallo (con ritorno al minimo se si supera il massimo)
static void clampTargetSpeed(bool wrap) {
  if (wrap && targetSpeed > TGT_SPEED_MAX) targetSpeed = TGT_SPEED_MIN;
  else if (targetSpeed < TGT_SPEED_MIN)     targetSpeed = TGT_SPEED_MIN;
  else if (targetSpeed > TGT_SPEED_MAX)     targetSpeed = TGT_SPEED_MAX;
}
#define TOLL_SPEED 1.0f                    // tolleranza target velocita' (km/h)
#define TOLL_HR    5                       // tolleranza target battito (bpm)
#define DEV_RANGE_SPEED 5.0f               // fondo scala barra deviazione (km/h)
#define DEV_RANGE_HR    15                 // fondo scala barra deviazione (bpm)

static uint8_t setupField = 0;             // campo selezionato nella pagina SETUP
static uint32_t timeInTarget = 0;          // secondi con scostamento dentro la tolleranza
static Preferences prefs;

// cache dei valori mostrati: si ridisegna solo se il testo cambia (meno flicker, meno lavoro)
static char lastVals[8][24];
static uint16_t lastValsCol[8];
static char lastHeroVal[24];
static bool lastHeroLive = false;
static int  lastHeaderState = -1;  // BLE + GPS + stato colore cuore (-1 = da disegnare)
static int  lastBattPct = -1;      // percentuale batteria disegnata (segmenti)
static bool hrBlinkOn = false;     // lampeggio del cuore in acquisizione
// cache delle pagine di allenamento
static char lastTgt[24], lastHeroC[24], lastDev[24], lastInfo[40], lastTime[24];
static uint16_t lastHeroCCol = 0;   // colore dell'hero (cambia senza che cambi il testo)
static int  lastZoneShown = -9;
// cache della pagina SETUP
static int lastSetupField = -1;
static int lastSetupVal[N_ZLIM] = {-1, -1, -1, -1};

static void invalidateCache() {
  memset(lastVals, 0, sizeof(lastVals));
  memset(lastValsCol, 0, sizeof(lastValsCol));
  lastHeroVal[0] = 0;
  lastHeroLive = false;
  lastHeaderState = -1;
  lastBattPct = -1;
  lastTgt[0] = lastHeroC[0] = lastDev[0] = lastInfo[0] = lastTime[0] = 0;
  lastHeroCCol = 0;
  lastZoneShown = -9;
  lastSetupField = -1;
  for (int i = 0; i < N_ZLIM; i++) lastSetupVal[i] = -1;
}

// zona cardiaca 0..4 (basata sulle soglie), -1 se il battito non c'e'
static int hrZone(int bpm) {
  if (bpm <= 0) return -1;
  int z = 0;
  while (z < N_ZLIM && bpm >= zoneLim[z]) z++;
  return z;
}
static uint16_t zoneColor(int z) { return (z < 0 || z >= ZONE_COUNT) ? C_DIM : zoneCol[z]; }

// ---------------------------------------------------------------- geometria
// Calcolata in setup() in base all'orientamento (ROTATION).
static int W, H;
static bool portrait = false;
static uint8_t pageCount = PAGE_COUNT;

static int heroX, heroY, heroW, heroH;   // pannello della velocita'

// griglia corrente (impostata da setGrid)
static int gCols, gRows, gX, gY, gW, gH;

// GRID_FULL = griglia della pagina intera; GRID_RIDE = griglia sotto l'hero (solo verticale)
#define GRID_FULL 0
#define GRID_RIDE 1

static void setGrid(uint8_t mode, int nItems) {
  const int gap = 4;
  if (portrait) {
    gCols = 2;
    gX = 4;
    gY = (mode == GRID_RIDE) ? heroY + heroH + gap : HDR_H + gap;
    gRows = (nItems + gCols - 1) / gCols;
    gW = (W - gap * 3) / 2;
    gH = ((H - gY - gap) - (gRows - 1) * gap) / gRows;
  } else {
    gRows = 2;
    gCols = (nItems + 1) / 2;
    if (gCols < 2) gCols = 2;
    gX = 4;
    gY = HDR_H + 3;
    gW = (W - gap * (gCols + 1)) / gCols;
    gH = ((H - gY - gap) - (gRows - 1) * gap) / gRows;
  }
}

static void setupGeometry() {
  W = tft.width();
  H = tft.height();
  portrait = (H > W);
  if (portrait) {
    pageCount = PAGE_COUNT;                // RIDE + STATS + SYS (griglia 2x3)
    heroX = 4;
    heroY = HDR_H + 4;
    heroW = W - 8;
    heroH = 124;
  } else {
    pageCount = PAGE_COUNT;
    heroX = 4;
    heroY = HDR_H + 3;
    heroW = 190;
    heroH = 83;
  }
}

static bool gpsLive() { return bleConnected && tel.lastRx != 0 && (millis() - tel.lastRx <= 5000); }
static bool hrLive()  { return tel.hrLastRx != 0 && (millis() - tel.hrLastRx <= 5000); }
// "in acquisizione": la fascia e' stata trovata e ci si sta connettendo.
// In scansione senza averla trovata NON e' acquisizione (nessuna fascia presente).
static bool hrAcquiring() { return hrState == HR_CONNECTING || (hrState == HR_SCANNING && hrFound); }

// Helper: testo con scelta automatica del font in base alla larghezza utile
static void drawFitted(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) {
  uint8_t f = 4;
  if (tft.textWidth(s, 4) > w - 16) f = 2;
  if (tft.textWidth(s, 2) > w - 16) f = 1;
  tft.setTextColor(fg, bg);
  tft.setTextFont(f);
  tft.setTextPadding(w - 12);   // cancella solo l'area del testo (anti-flicker)
  tft.drawString(s, x, y, f);
  tft.setTextPadding(0);
  tft.setTextFont(2);
}

// come drawFitted ma con cache: se testo e colore non cambiano non tocca il bus.
// (il ramo orizzontale della RIDE lo usa per distanza/tempo/media/max)
static void drawFittedCached(int i, int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) {
  if (strcmp(lastVals[i], s) == 0 && lastValsCol[i] == fg) return;
  drawFitted(x, y, w, s, fg, bg);
  strncpy(lastVals[i], s, sizeof(lastVals[i]) - 1);
  lastVals[i][sizeof(lastVals[i]) - 1] = 0;
  lastValsCol[i] = fg;
}

static void drawCell(int x, int y, int w, int h, const char *label, const char *value, uint16_t vc) {
  tft.fillRoundRect(x, y, w, h, 6, C_PANEL);
  tft.drawRoundRect(x, y, w, h, 6, C_BORDER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(label, x + 8, y + 6, 2);
  tft.setTextDatum(ML_DATUM);
  drawFitted(x + 8, y + h - 16, w, value, vc, C_PANEL);
  tft.setTextDatum(TL_DATUM);
}

// intestazione statica (disegnata una volta per pagina)
static const char *pageTitle() {
  switch (page) {
    case P_RIDE:       return "RIDE";
    case P_COST_SPEED: return "COST SPEED";
    case P_COST_BPM:   return "COST BPM";
    case P_SETUP:      return "SETUP";
    case P_DIAG:       return "DIAG";
  }
  return "BikeGPS";
}

static void updateHeader();   // definita sotto: la richiama drawHeaderStatic

// intestazione: niente titolo pagina, solo stato BLE (colore) e batteria (pittogramma)
static void drawHeaderStatic() {
  tft.fillRect(0, 0, W, HDR_H, C_BG);
  lastHeaderState = -1;
  lastBattPct = -1;                 // forza il ridisegno di stato e batteria
  updateHeader();
}

// icona batteria a 4 segmenti: pieni in base alla percentuale, colore a scalare
static void drawBatteryIcon(int x, int y, int pct) {
  const int w = 26, h = 13;
  tft.drawRect(x, y, w, h, C_DIM);
  tft.fillRect(x + w, y + 4, 2, h - 8, C_DIM);       // tappo
  int filled = (pct + 12) / 25;                       // 0..4 segmenti
  if (filled > 4) filled = 4;
  if (filled < 0) filled = 0;
  uint16_t col = (pct < 20) ? C_RED : (pct < 40 ? C_YELLOW : C_BATT);
  for (int i = 0; i < 4; i++)
    tft.fillRect(x + 2 + i * 6, y + 2, 4, h - 4, (i < filled) ? col : C_BG);
}

// icone di stato 12x12: verdi quando il dato c'e', rosse quando manca
static void drawBleIcon(int x, int y, uint16_t col) {         // runa Bluetooth
  const int cx = x + 4;
  tft.drawLine(cx, y, cx, y + 11, col);
  tft.drawLine(cx, y, cx + 4, y + 3, col);
  tft.drawLine(cx + 4, y + 3, cx - 4, y + 9, col);
  tft.drawLine(cx, y + 11, cx + 4, y + 8, col);
  tft.drawLine(cx + 4, y + 8, cx - 4, y + 2, col);
}

static void drawHeartIcon(int x, int y, uint16_t col) {       // cuore
  tft.fillCircle(x + 3, y + 4, 3, col);
  tft.fillCircle(x + 8, y + 4, 3, col);
  tft.fillTriangle(x, y + 5, x + 11, y + 5, x + 6, y + 12, col);
}

// parte variabile dell'intestazione: GPS, BLE, cuore, batteria (1 Hz, solo se cambiano)
static void updateHeader() {
  const int pct    = battPercent();
  const bool gpsOn = gpsLive();
  const bool bleOn = bleConnected;
  const int state  = (bleOn ? 1 : 0) | (gpsOn ? 2 : 0) | (hrLive() ? 4 : 0) | (hrAcquiring() ? 8 : 0);
  if (state == lastHeaderState && pct == lastBattPct) return;

  tft.fillRect(0, 0, 70, HDR_H, C_BG);          // pulizia area GPS/BLE/cuore
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(gpsOn ? C_BLUE : C_DIM, C_BG);
  tft.drawString("GPS", 4, (HDR_H - 16) / 2, 2);
  drawBleIcon(34, (HDR_H - 12) / 2, bleOn ? C_BLUE : C_DIM);
  drawHeartIcon(54, (HDR_H - 12) / 2, hrLive() ? C_GREEN : (hrAcquiring() ? C_RED : C_DIM));
  drawBatteryIcon(W - 36, (HDR_H - 13) / 2, pct);

  lastHeaderState = state;
  lastBattPct = pct;
}

// --- formattazioni condivise
// tempo in movimento: solo ore e minuti (i secondi rendevano il testo piccolo)
static void fmtTime(uint32_t t, char *buf, size_t n) {
  snprintf(buf, n, "%lu:%02lu",
           (unsigned long)(t / 3600), (unsigned long)((t % 3600) / 60));
}
static void fmtAvg(char *buf, size_t n) {
  float avg = (tel.mov > 0) ? tel.dst / (tel.mov / 3600.0f) : 0.0f;
  snprintf(buf, n, "%.1f", avg);
}
static void fmtUptime(char *buf, size_t n) {
  uint32_t s = millis() / 1000;
  snprintf(buf, n, "%lu:%02lu:%02lu",
           (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60), (unsigned long)(s % 60));
}

// etichette delle pagine a griglia (usate anche dalla pagina RIDE in verticale)
static const char *const STATS_LABELS[6] = {"DIST km", "TEMPO", "MEDIA km/h", "MAX km/h", "QUOTA m", "PENDENZA %"};
static const char *const DIAG_LABELS[8]  = {"BLE", "CARDIO bpm", "SATELLITI", "BATTERIA", "QUOTA m", "PENDENZA %", "HEAP", "UPTIME"};

// ---------------------------------------------------------------- pagina RIDE
static void layoutRide() {
  // pannello velocita': tutto il box e' per il numero, nessuna etichetta
  tft.fillRoundRect(heroX, heroY, heroW, heroH, 6, C_PANEL);
  tft.drawRoundRect(heroX, heroY, heroW, heroH, 6, C_BORDER);

  if (portrait) {
    // verticale: sotto l'hero la griglia 2x2 con distanza, tempo, media, max
    setGrid(GRID_RIDE, 4);
    layoutGrid(STATS_LABELS, 4);
    return;
  }

  // orizzontale: distanza/tempo sotto l'hero, media/max a destra
  const int gx = heroX + heroW + 4, gw = W - gx - 4;
  const int cy = heroY + heroH + 4, ch = H - cy - 4;
  const int cw = (heroW - 4) / 2;
  drawCell(heroX, cy, cw, ch, "DIST km", "--", C_CYAN);
  drawCell(heroX + cw + 4, cy, cw, ch, "TEMPO", "--", C_CYAN);
  drawCell(gx, heroY, gw, (heroH - 4) / 2, "MEDIA km/h", "--", C_YELLOW);
  drawCell(gx, heroY + (heroH - 4) / 2 + 4, gw, (heroH - 4) / 2, "MAX km/h", "--", C_YELLOW);
}

static void updateRide() {
  char v[24];
  bool live = gpsLive() || hrLive() || tel.lastRx != 0;

  // velocita' gigante (font 7 = "7 segment" 48 px)
  if (live) snprintf(v, sizeof(v), "%.1f", tel.spd);
  else      snprintf(v, sizeof(v), "-.-");
  if (strcmp(v, lastHeroVal) != 0 || live != lastHeroLive) {
    // velocita' gigante centrata in tutto il box
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(live ? C_FG : C_DIM, C_PANEL);
    tft.setTextPadding(heroW - 12);   // cancella solo l'area del testo (anti-flicker)
    tft.drawString(v, heroX + heroW / 2, heroY + heroH / 2, 7);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    strncpy(lastHeroVal, v, sizeof(lastHeroVal) - 1);
    lastHeroVal[sizeof(lastHeroVal) - 1] = 0;
    lastHeroLive = live;
  }

  const char *vals[6];
  uint16_t cols[6];

  if (portrait) {
    setGrid(GRID_RIDE, 4);
    statsValues(vals, cols);
    updateGrid(vals, cols, 4);
    return;
  }

  // orizzontale: 4 campi, con la stessa cache del resto della pagina
  const int gx = heroX + heroW + 4, gw = W - gx - 4;
  const int cy = heroY + heroH + 4, ch = H - cy - 4;
  const int cw = (heroW - 4) / 2;
  tft.setTextDatum(ML_DATUM);
  if (live) snprintf(v, sizeof(v), "%.2f", tel.dst); else strcpy(v, "--");
  drawFittedCached(0, heroX + 8, cy + ch - 16, cw, v, C_CYAN, C_PANEL);
  if (live) fmtTime(tel.mov, v, sizeof(v)); else strcpy(v, "--");
  drawFittedCached(1, heroX + cw + 12, cy + ch - 16, cw, v, C_CYAN, C_PANEL);
  if (live) fmtAvg(v, sizeof(v)); else strcpy(v, "--");
  drawFittedCached(2, gx + 8, heroY + (heroH - 4) / 2 - 16, gw, v, C_YELLOW, C_PANEL);
  if (live) snprintf(v, sizeof(v), "%.1f", tel.mx); else strcpy(v, "--");
  drawFittedCached(3, gx + 8, heroY + (heroH - 4) / 2 + 4 + (heroH - 4) / 2 - 16, gw, v, C_YELLOW, C_PANEL);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------- pagine a griglia
static void gridCell(int i, int &x, int &y) {
  x = gX + (i % gCols) * (gW + 4);
  y = gY + (i / gCols) * (gH + 4);
}

static void layoutGrid(const char *const *labels, int n) {
  for (int i = 0; i < n; i++) {
    int x, y;
    gridCell(i, x, y);
    drawCell(x, y, gW, gH, labels[i], "--", C_FG);
  }
}

static void updateGrid(const char *const *vals, const uint16_t *cols, int n) {
  tft.setTextDatum(ML_DATUM);
  for (int i = 0; i < n; i++) {
    if (strcmp(lastVals[i], vals[i]) == 0 && lastValsCol[i] == cols[i]) continue;
    int x, y;
    gridCell(i, x, y);
    drawFitted(x + 8, y + gH - 16, gW, vals[i], cols[i], C_PANEL);
    strncpy(lastVals[i], vals[i], sizeof(lastVals[i]) - 1);
    lastVals[i][sizeof(lastVals[i]) - 1] = 0;
    lastValsCol[i] = cols[i];
  }
  tft.setTextDatum(TL_DATUM);
}

// valori per la griglia della pagina RIDE (4 celle: dist, tempo, media, max)
static void statsValues(const char *vals[6], uint16_t cols[6]) {
  static char a[24], b[24], c[24], d[24], e[24], f[24];
  bool live = gpsLive();
  if (live) snprintf(a, sizeof(a), "%.2f", tel.dst);      else snprintf(a, sizeof(a), "--");
  if (live) fmtTime(tel.mov, b, sizeof(b));               else snprintf(b, sizeof(b), "--");
  if (live) fmtAvg(c, sizeof(c));                         else snprintf(c, sizeof(c), "--");
  if (live) snprintf(d, sizeof(d), "%.1f", tel.mx);       else snprintf(d, sizeof(d), "--");
  if (live) snprintf(e, sizeof(e), "%d", tel.alt);        else snprintf(e, sizeof(e), "--");
  if (live) snprintf(f, sizeof(f), "%+.1f", tel.slp);      else snprintf(f, sizeof(f), "--");
  vals[0] = a; vals[1] = b; vals[2] = c; vals[3] = d; vals[4] = e; vals[5] = f;
  for (int i = 0; i < 6; i++) cols[i] = live ? C_FG : C_DIM;
}

// valori della pagina DIAG (8 celle)
static void diagValues(const char *vals[8], uint16_t cols[8]) {
  static char a[24], b[24], c[24], d[24], e[24], f[24], g[24], h[24];
  snprintf(a, sizeof(a), "%s", bleConnected ? "connesso" : "assente");
  if (hrLive()) snprintf(b, sizeof(b), "%d", tel.hr);
  else          snprintf(b, sizeof(b), "--");
  if (gpsLive()) snprintf(c, sizeof(c), "%d", tel.sat);
  else           snprintf(c, sizeof(c), "--");
  snprintf(d, sizeof(d), "%d%% (%.2fV)", battPercent(), battFiltered);
  if (gpsLive()) snprintf(e, sizeof(e), "%d", tel.alt);     else snprintf(e, sizeof(e), "--");
  if (gpsLive()) snprintf(f, sizeof(f), "%+.1f", tel.slp);  else snprintf(f, sizeof(f), "--");
  snprintf(g, sizeof(g), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  fmtUptime(h, sizeof(h));
  vals[0] = a; vals[1] = b; vals[2] = c; vals[3] = d;
  vals[4] = e; vals[5] = f; vals[6] = g; vals[7] = h;
  cols[0] = bleConnected ? C_GREEN : C_RED;
  cols[1] = hrLive() ? C_RED : C_DIM;
  cols[2] = gpsLive() ? C_GREEN : C_DIM;
  cols[3] = (battPercent() < 20) ? C_RED : (battPercent() < 40 ? C_YELLOW : C_GREEN);
  cols[4] = gpsLive() ? C_CYAN : C_DIM;
  cols[5] = gpsLive() ? C_CYAN : C_DIM;
  cols[6] = C_DIM;
  cols[7] = C_DIM;
}

// ---------------------------------------------------------------- pagine allenamento
struct Rect { int x, y, w, h; };
static Rect R_TGT, R_ZBAR, R_HERO, R_DEV, R_DEVBR, R_INFO, R_TIME;

static void costGeometry() {
  if (portrait) {
    int y = HDR_H + 4;
    R_TGT   = {4, y, W - 8, 34};  y += 36;
    R_ZBAR  = {4, y, W - 8, 22};  y += 26;
    R_HERO  = {4, y, W - 8, 100}; y += 102;
    R_DEV   = {4, y, W - 8, 26};  y += 28;
    R_DEVBR = {4, y, W - 8, 20};  y += 24;
    R_INFO  = {4, y, W - 8, 26};  y += 28;
    R_TIME  = {4, y, W - 8, 26};
  } else {
    R_TGT   = {4,   HDR_H + 4,   190, 26};
    R_ZBAR  = {200, HDR_H + 4,   W - 204, 26};
    R_HERO  = {4,   HDR_H + 34,  190, 78};
    R_DEV   = {4,   HDR_H + 116, 190, 22};
    R_DEVBR = {200, HDR_H + 34,  W - 204, 20};
    R_INFO  = {200, HDR_H + 58,  W - 204, 24};
    R_TIME  = {200, HDR_H + 86,  W - 204, 24};
  }
}

static void layoutZoneBar(int x, int y, int w, int h) {
  const int gap = 3;
  int sw = (w - gap * (ZONE_COUNT - 1)) / ZONE_COUNT;
  for (int i = 0; i < ZONE_COUNT; i++) {
    int xi = x + i * (sw + gap);
    tft.fillRect(xi, y, sw, h, C_PANEL);
    tft.drawRect(xi, y, sw, h, C_BORDER);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(ZONE_NAMES[i], xi + sw / 2, y + h / 2, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

static void updateZoneBar(int x, int y, int w, int h, int active) {
  const int gap = 3;
  int sw = (w - gap * (ZONE_COUNT - 1)) / ZONE_COUNT;
  for (int i = 0; i < ZONE_COUNT; i++) {
    int xi = x + i * (sw + gap);
    bool on = (i == active);
    uint16_t bg = on ? zoneCol[i] : C_PANEL;
    tft.fillRect(xi, y, sw, h, bg);
    tft.drawRect(xi, y, sw, h, on ? C_FG : C_BORDER);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? C_BG : C_DIM, bg);
    tft.drawString(ZONE_NAMES[i], xi + sw / 2, y + h / 2, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

// --- barra deviazione: pannello + banda disegnati UNA volta, poi si muove solo il cursore
static int dbX, dbY, dbW, dbH, dbCx, dbHalf, dbTw;
static int dbPx = 0x7FFFFFFF;   // ultima posizione del cursore (sentinel = mai disegnato)

static void layoutDevBar(int x, int y, int w, int h, float range, float toll) {
  dbX = x; dbY = y; dbW = w; dbH = h;
  tft.fillRect(x, y, w, h, C_PANEL);
  tft.drawRect(x, y, w, h, C_BORDER);
  dbCx = x + w / 2;
  dbHalf = w / 2 - 3;
  dbTw = (int)(toll / range * dbHalf);
  if (dbTw > 0) tft.fillRect(dbCx - dbTw, y + 1, dbTw * 2, h - 2, tft.color565(0x1e, 0x3a, 0x2a));
  tft.drawFastVLine(dbCx, y + 1, h - 2, C_FG);
  tft.fillRect(dbCx - 2, y + 1, 5, h - 2, C_GREEN);   // cursore a centro (deviazione 0)
  dbPx = dbCx;
}

// Ridisegna SOLO il tratto fra vecchio e nuovo cursore (entrambi i segmenti partono dal centro).
static void updateDevBar(float dev, float range, float toll) {
  float k = dev / range;
  if (k > 1) k = 1;
  if (k < -1) k = -1;
  int px = dbCx + (int)(k * dbHalf);
  if (px == dbPx) return;

  int c0 = min(dbPx, px); if (c0 > dbCx) c0 = dbCx;
  int c1 = max(dbPx, px); if (c1 < dbCx) c1 = dbCx;
  c0 -= 3; c1 += 3;                       // il cursore e' largo 5 px
  if (c0 < dbX + 1) c0 = dbX + 1;
  if (c1 > dbX + dbW - 1) c1 = dbX + dbW - 1;

  tft.fillRect(c0, dbY + 1, c1 - c0, dbH - 2, C_PANEL);
  if (dbTw > 0) {
    int b0 = dbCx - dbTw, b1 = dbCx + dbTw;
    if (b0 < c0) b0 = c0;
    if (b1 > c1) b1 = c1;
    if (b1 > b0) tft.fillRect(b0, dbY + 1, b1 - b0, dbH - 2, tft.color565(0x1e, 0x3a, 0x2a));
  }
  tft.drawFastVLine(dbCx, dbY + 1, dbH - 2, C_FG);

  float a = fabs(dev);
  uint16_t col = (a <= toll) ? C_GREEN : (a <= 2 * toll ? C_YELLOW : C_RED);
  int s0 = min(dbCx, px), s1 = max(dbCx, px);
  if (s1 > s0) tft.fillRect(s0, dbY + 3, s1 - s0, dbH - 6, col);
  tft.fillRect(px - 2, dbY + 1, 5, dbH - 2, col);
  dbPx = px;
}

// riga del target: pannello + etichetta una volta, valore a ogni cambio
static void layoutTargetRow(const char *label) {
  int r_x = R_TGT.x, r_y = R_TGT.y, r_w = R_TGT.w, r_h = R_TGT.h;
  tft.fillRoundRect(r_x, r_y, r_w, r_h, 6, C_PANEL);
  tft.drawRoundRect(r_x, r_y, r_w, r_h, 6, C_BORDER);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(label, r_x + 8, r_y + r_h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static void updateTargetValue(const char *value, uint16_t col) {
  int r_x = R_TGT.x, r_y = R_TGT.y, r_w = R_TGT.w, r_h = R_TGT.h;
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(col, C_PANEL);
  tft.setTextPadding(r_w / 2 + 8);
  tft.drawString(value, r_x + r_w - 8, r_y + r_h / 2, 4);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

// valore gigante: pannello + unita' una volta, numero a ogni cambio
static void layoutCostHero(const char *unit) {
  int r_x = R_HERO.x, r_y = R_HERO.y, r_w = R_HERO.w, r_h = R_HERO.h;
  tft.fillRoundRect(r_x, r_y, r_w, r_h, 6, C_PANEL);
  tft.drawRoundRect(r_x, r_y, r_w, r_h, 6, C_BORDER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.setTextPadding(60);
  tft.drawString(unit, r_x + r_w - 46, r_y + r_h - 20, 2);
  tft.setTextPadding(0);
}

static void updateCostHeroValue(const char *value, uint16_t col) {
  int r_x = R_HERO.x, r_y = R_HERO.y, r_w = R_HERO.w, r_h = R_HERO.h;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, C_PANEL);
  tft.setTextPadding(r_w - 10);
  tft.drawString(value, r_x + r_w / 2, r_y + r_h / 2 - 8, 7);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

static void layoutInfoRow() {
  int r_x = R_INFO.x, r_y = R_INFO.y, r_w = R_INFO.w, r_h = R_INFO.h;
  tft.fillRoundRect(r_x, r_y, r_w, r_h, 6, C_PANEL);
  tft.drawRoundRect(r_x, r_y, r_w, r_h, 6, C_BORDER);
}

static void updateInfoValue(const char *s) {
  int r_x = R_INFO.x, r_y = R_INFO.y, r_w = R_INFO.w, r_h = R_INFO.h;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_FG, C_PANEL);
  tft.setTextPadding(r_w - 12);
  tft.drawString(s, r_x + 8, r_y + r_h / 2, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

static void drawTimeRow(const char *s) {
  int r_x = R_TIME.x, r_y = R_TIME.y, r_w = R_TIME.w, r_h = R_TIME.h;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextPadding(r_w - 12);
  tft.drawString(s, r_x + 8, r_y + r_h / 2, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

// --- valori comuni alle pagine di allenamento
static void costTimeStr(char *buf, size_t n) {
  snprintf(buf, n, "in target %lu:%02lu",
           (unsigned long)(timeInTarget / 3600), (unsigned long)((timeInTarget % 3600) / 60));
}

// --- COST SPEED (velocita' costante)
static void layoutCostSpeed() {
  tft.fillScreen(C_BG);
  invalidateCache();
  drawHeaderStatic();
  costGeometry();
  layoutTargetRow("TARGET");
  layoutZoneBar(R_ZBAR.x, R_ZBAR.y, R_ZBAR.w, R_ZBAR.h);
  layoutCostHero("km/h");
  layoutDevBar(R_DEVBR.x, R_DEVBR.y, R_DEVBR.w, R_DEVBR.h, DEV_RANGE_SPEED, TOLL_SPEED);
  layoutInfoRow();
  drawTimeRow("in target --:--");
  updateTargetValue("--", C_GREEN);
  updateCostHeroValue("--", C_FG);
  updateInfoValue("--");
}

static void updateCostSpeed() {
  char tgt[24], hero[24], dev[24], info[40], tm[32];
  bool live = gpsLive() || hrLive() || tel.lastRx != 0;
  int zone = hrLive() ? hrZone(tel.hr) : -1;

  snprintf(tgt, sizeof(tgt), "%.1f km/h", targetSpeed);
  if (strcmp(tgt, lastTgt) != 0) {
    updateTargetValue(tgt, C_GREEN);
    strncpy(lastTgt, tgt, sizeof(lastTgt) - 1);
  }

  if (zone != lastZoneShown) {
    updateZoneBar(R_ZBAR.x, R_ZBAR.y, R_ZBAR.w, R_ZBAR.h, zone);
    lastZoneShown = zone;
  }

  float d = tel.spd - targetSpeed;
  bool inT = live && fabs(d) <= TOLL_SPEED;
  if (live) snprintf(hero, sizeof(hero), "%.1f", tel.spd); else strcpy(hero, "-.-");
  uint16_t hcol = !live ? C_DIM : (inT ? C_GREEN : C_FG);
  if (strcmp(hero, lastHeroC) != 0 || hcol != lastHeroCCol) {
    updateCostHeroValue(hero, hcol);
    strncpy(lastHeroC, hero, sizeof(lastHeroC) - 1);
    lastHeroCCol = hcol;
  }

  if (live) snprintf(dev, sizeof(dev), "%c  %+.1f km/h", inT ? '=' : (d > 0 ? '^' : 'v'), d);
  else      strcpy(dev, "--");
  if (strcmp(dev, lastDev) != 0) {
    uint16_t c = !live ? C_DIM : (inT ? C_GREEN : (fabs(d) <= 2 * TOLL_SPEED ? C_YELLOW : C_RED));
    const Rect &r = R_DEV;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c, C_BG);
    tft.setTextPadding(r.w - 10);
    tft.drawString(dev, r.x + r.w / 2, r.y + r.h / 2, 4);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    strncpy(lastDev, dev, sizeof(lastDev) - 1);
  }
  updateDevBar(live ? d : 0, DEV_RANGE_SPEED, TOLL_SPEED);

  if (live) {
    char hrb[12];
    if (hrLive()) snprintf(hrb, sizeof(hrb), "%d", tel.hr); else strcpy(hrb, "--");
    snprintf(info, sizeof(info), "HR %s bpm  %s  %.2f km", hrb,
             (zone >= 0) ? ZONE_NAMES[zone] : "--", tel.dst);
  } else strcpy(info, "nessun dato GPS");
  if (strcmp(info, lastInfo) != 0) {
    updateInfoValue(info);
    strncpy(lastInfo, info, sizeof(lastInfo) - 1);
  }

  costTimeStr(tm, sizeof(tm));
  if (strcmp(tm, lastTime) != 0) {
    drawTimeRow(tm);
    strncpy(lastTime, tm, sizeof(lastTime) - 1);
  }
}

// --- COST BPM (carico costante)
static void layoutCostBpm() {
  tft.fillScreen(C_BG);
  invalidateCache();
  drawHeaderStatic();
  costGeometry();
  layoutTargetRow("TARGET");
  layoutZoneBar(R_ZBAR.x, R_ZBAR.y, R_ZBAR.w, R_ZBAR.h);
  layoutCostHero("bpm");
  layoutDevBar(R_DEVBR.x, R_DEVBR.y, R_DEVBR.w, R_DEVBR.h, DEV_RANGE_HR, TOLL_HR);
  layoutInfoRow();
  drawTimeRow("in target --:--");
  updateTargetValue("--", C_RED);
  updateCostHeroValue("--", C_FG);
  updateInfoValue("--");
}

static void updateCostBpm() {
  char tgt[24], hero[24], dev[24], info[40], tm[32];
  bool live = hrLive();
  int zone = live ? hrZone(tel.hr) : -1;

  snprintf(tgt, sizeof(tgt), "%d bpm", targetHr);
  if (strcmp(tgt, lastTgt) != 0) {
    updateTargetValue(tgt, C_RED);
    strncpy(lastTgt, tgt, sizeof(lastTgt) - 1);
  }

  if (zone != lastZoneShown) {
    updateZoneBar(R_ZBAR.x, R_ZBAR.y, R_ZBAR.w, R_ZBAR.h, zone);
    lastZoneShown = zone;
  }

  int d = tel.hr - targetHr;
  bool inT = live && abs(d) <= TOLL_HR;
  if (live) snprintf(hero, sizeof(hero), "%d", tel.hr); else strcpy(hero, "--");
  uint16_t hcol = !live ? C_DIM : zoneColor(zone);
  if (strcmp(hero, lastHeroC) != 0 || hcol != lastHeroCCol) {
    updateCostHeroValue(hero, hcol);
    strncpy(lastHeroC, hero, sizeof(lastHeroC) - 1);
    lastHeroCCol = hcol;
  }

  if (live) snprintf(dev, sizeof(dev), "%c  %+d bpm", inT ? '=' : (d > 0 ? '^' : 'v'), d);
  else      strcpy(dev, "--");
  if (strcmp(dev, lastDev) != 0) {
    uint16_t c = !live ? C_DIM : (inT ? C_GREEN : (abs(d) <= 2 * TOLL_HR ? C_YELLOW : C_RED));
    const Rect &r = R_DEV;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(c, C_BG);
    tft.setTextPadding(r.w - 10);
    tft.drawString(dev, r.x + r.w / 2, r.y + r.h / 2, 4);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    strncpy(lastDev, dev, sizeof(lastDev) - 1);
  }
  updateDevBar(live ? (float)d : 0, DEV_RANGE_HR, TOLL_HR);

  bool g = gpsLive() || tel.lastRx != 0;
  if (g) snprintf(info, sizeof(info), "%.1f km/h  %s  %.2f km", tel.spd,
                  (zone >= 0) ? ZONE_NAMES[zone] : "--", tel.dst);
  else   strcpy(info, "no GPS (app non collegata)");
  if (strcmp(info, lastInfo) != 0) {
    updateInfoValue(info);
    strncpy(lastInfo, info, sizeof(lastInfo) - 1);
  }

  costTimeStr(tm, sizeof(tm));
  if (strcmp(tm, lastTime) != 0) {
    drawTimeRow(tm);
    strncpy(lastTime, tm, sizeof(lastTime) - 1);
  }
}

// --- SETUP SOGLIE
static int srX, srY, srW, srH;      // geometria della riga corrente (evita tipi custom nelle firme)
static void setupRowRect(int i) {
  if (portrait) {
    srX = 4; srY = HDR_H + 6 + i * 46; srW = W - 8; srH = 42;
  } else {
    srX = 4; srY = HDR_H + 4 + i * 32; srW = 200; srH = 28;
  }
}

static void drawSetupRow(int i, bool selected) {
  setupRowRect(i);
  uint16_t bg = selected ? C_PANEL : C_BG;
  tft.fillRoundRect(srX, srY, srW, srH, 6, bg);
  tft.drawRoundRect(srX, srY, srW, srH, 6, selected ? C_YELLOW : C_BORDER);

  char label[16], value[16];
  snprintf(label, sizeof(label), "%s / %s", ZONE_NAMES[i], ZONE_NAMES[i + 1]);
  snprintf(value, sizeof(value), "%d bpm", zoneLim[i]);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(selected ? C_YELLOW : C_DIM, bg);
  tft.setTextPadding(12);
  tft.drawString(selected ? ">" : " ", srX + 6, srY + srH / 2, 2);
  tft.setTextPadding(0);
  tft.setTextColor(selected ? C_FG : C_DIM, bg);
  tft.setTextPadding(120);
  tft.drawString(label, srX + 20, srY + srH / 2, 2);
  tft.setTextPadding(0);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(selected ? C_FG : C_DIM, bg);
  tft.setTextPadding(srW / 2);
  tft.drawString(value, srX + srW - 10, srY + srH / 2, 4);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

static void setupZoneBarRect(int &zbx, int &zby, int &zbw, int &zbh) {
  if (portrait) {
    setupRowRect(N_ZLIM - 1);
    zbx = 4; zby = srY + srH + 12; zbw = W - 8; zbh = 34;
  } else {
    zbx = 212; zby = HDR_H + 4; zbw = W - 216; zbh = 30;
  }
}

static void layoutSetup() {
  tft.fillScreen(C_BG);
  invalidateCache();
  drawHeaderStatic();
  for (int i = 0; i < N_ZLIM; i++) drawSetupRow(i, i == setupField);

  int zbx, zby, zbw, zbh;
  setupZoneBarRect(zbx, zby, zbw, zbh);
  layoutZoneBar(zbx, zby, zbw, zbh);

  if (portrait) {
    int ay = zby + zbh + 10;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextPadding(W - 12);
    tft.drawString("destro: campo   sinistro: +1", 6, ay, 2);
    tft.drawString("sinistro lungo: -1 bpm", 6, ay + 18, 2);
    tft.drawString("destro lungo: torna a RIDE", 6, ay + 36, 2);
    tft.setTextPadding(0);
  }
}

static void updateSetup() {
  for (int i = 0; i < N_ZLIM; i++) {
    bool selChanged = ((i == setupField) != (i == lastSetupField));
    if (lastSetupVal[i] == zoneLim[i] && !selChanged) continue;
    drawSetupRow(i, i == setupField);
    lastSetupVal[i] = zoneLim[i];
  }
  lastSetupField = setupField;

  int zone = hrLive() ? hrZone(tel.hr) : -1;
  if (zone != lastZoneShown) {
    int zbx, zby, zbw, zbh;
    setupZoneBarRect(zbx, zby, zbw, zbh);
    updateZoneBar(zbx, zby, zbw, zbh, zone);
    lastZoneShown = zone;
  }
}

// ---------------------------------------------------------------- disegno completo
static void drawFull() {
  const char *vals[8];
  uint16_t cols[8];
  switch (page) {
    case P_RIDE:
      tft.fillScreen(C_BG);
      invalidateCache();
      drawHeaderStatic();
      layoutRide();
      updateRide();
      break;
    case P_COST_SPEED:
      layoutCostSpeed();
      updateCostSpeed();
      break;
    case P_COST_BPM:
      layoutCostBpm();
      updateCostBpm();
      break;
    case P_SETUP:
      layoutSetup();
      updateSetup();
      break;
    case P_DIAG:
      tft.fillScreen(C_BG);
      invalidateCache();
      drawHeaderStatic();
      setGrid(GRID_FULL, 8);
      layoutGrid(DIAG_LABELS, 8);
      diagValues(vals, cols);
      updateGrid(vals, cols, 8);
      break;
  }
}

static void drawValues() {
  const char *vals[8];
  uint16_t cols[8];
  switch (page) {
    case P_RIDE:
      updateRide();
      break;
    case P_COST_SPEED:
      updateCostSpeed();
      break;
    case P_COST_BPM:
      updateCostBpm();
      break;
    case P_SETUP:
      updateSetup();
      break;
    case P_DIAG:
      setGrid(GRID_FULL, 8);
      diagValues(vals, cols);
      updateGrid(vals, cols, 8);
      break;
  }
}

// ---------------------------------------------------------------- impostazioni (NVS)
static void saveSettings() {
  prefs.begin("bikegps", false);
  prefs.putBytes("zoneLim", zoneLim, sizeof(zoneLim));
  prefs.putFloat("tgtSpeed", targetSpeed);
  prefs.putInt("tgtHr", targetHr);
  prefs.end();
#if SERIAL_DEBUG
  Serial.printf("NVS salvata: limite=%d tgtSpeed=%.1f tgtHr=%d\n",
                zoneLim[setupField], targetSpeed, targetHr);
#endif
}

static void loadSettings() {
  prefs.begin("bikegps", true);
  if (prefs.isKey("zoneLim")) prefs.getBytes("zoneLim", zoneLim, sizeof(zoneLim));
  targetSpeed = prefs.getFloat("tgtSpeed", targetSpeed);
  targetHr    = prefs.getInt("tgtHr", targetHr);
  prefs.end();
  page = P_RIDE;                 // si parte sempre dalla pagina RIDE
  clampTargetSpeed(false);
  if (targetHr < TGT_HR_MIN) targetHr = TGT_HR_MIN;
  if (targetHr > TGT_HR_MAX) targetHr = TGT_HR_MAX;
  for (int i = 1; i < N_ZLIM; i++) if (zoneLim[i] <= zoneLim[i - 1]) zoneLim[i] = zoneLim[i - 1] + 1;
}

// Salvataggio NVS RITARDATO: il tasto non scrive piu' in flash ad ogni pressione.
// Un task a 1 Hz salva solo se i valori sono cambiati e nessuno li tocca da 1 s.
static bool settingsDirty = false;
static uint32_t settingsDirtyAt = 0;

static void markSettingsDirty() {
  settingsDirty = true;
  settingsDirtyAt = millis();
}

// ---------------------------------------------------------------- pulsanti
struct Button { uint8_t pin; bool last; uint32_t tDown; bool longFired; };
static Button btn1 = {BTN_SET,  HIGH, 0, false};
static Button btn2 = {BTN_PAGE, HIGH, 0, false};

static void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BL, on ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);
#if SERIAL_DEBUG
  Serial.printf("backlight %s (pin %d = %d)\n", on ? "ON" : "OFF", TFT_BL, digitalRead(TFT_BL));
#endif
}

static void printStatus() {
#if SERIAL_DEBUG
  Serial.printf("stato: backlightOn=%d pinBL(%d)=%d lcdPower(%d)=%d heap=%u page=%d/%d portrait=%d up=%lus\n",
                backlightOn, TFT_BL, digitalRead(TFT_BL), PIN_LCD_POWER, digitalRead(PIN_LCD_POWER),
                (unsigned)ESP.getFreeHeap(), page + 1, pageCount, (int)portrait, (unsigned long)(millis() / 1000));
  Serial.printf("tasti: SET(btn1,%d)=%d PAGE(btn2,%d)=%d | soglie: %d %d %d %d | tgtSpeed=%.1f tgtHr=%d | campo=%d\n",
                PIN_BTN1, digitalRead(PIN_BTN1), PIN_BTN2, digitalRead(PIN_BTN2),
                zoneLim[0], zoneLim[1], zoneLim[2], zoneLim[3], targetSpeed, targetHr, setupField);
#endif
}

// Dopo un cambio di target: NON ridisegnare lo schermo intero.
// Basta forzare la riga TARGET e lasciare che la cache aggiorni solo cio' che e' cambiato
// (hero, dev, barra di deviazione).
static void refreshTarget() {
  lastTgt[0] = 0;
  drawValues();
}

// --- tasto SET (sinistro): pressione corta
static void onSetShort() {
  bool changed = false;
  switch (page) {
    case P_COST_SPEED:
      targetSpeed += TGT_SPEED_STEP;
      clampTargetSpeed(true);      // oltre 35 km/h riparte da 25
      markSettingsDirty();
      refreshTarget();
      return;
    case P_COST_BPM:
      targetHr++;
      if (targetHr > TGT_HR_MAX) targetHr = TGT_HR_MIN;
      markSettingsDirty();
      refreshTarget();
      return;
    case P_SETUP:
      zoneLim[setupField]++;
      if (zoneLim[setupField] > 220) zoneLim[setupField] = 220;
      for (int i = 1; i < N_ZLIM; i++) if (zoneLim[i] < zoneLim[i - 1] + 1) zoneLim[i] = zoneLim[i - 1] + 1;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
#if SERIAL_DEBUG
    Serial.printf("set: tgtSpeed=%.1f tgtHr=%d zoneLim=%d,%d,%d,%d\n",
                  targetSpeed, targetHr, zoneLim[0], zoneLim[1], zoneLim[2], zoneLim[3]);
#endif
    markSettingsDirty();
    invalidateCache();
    drawFull();
  }
}

// --- tasto SET (sinistro): pressione lunga
static void onSetLong() {
  switch (page) {
    case P_COST_SPEED:
      if (gpsLive() || tel.lastRx != 0) {
        targetSpeed = roundf(tel.spd * 10.0f) / 10.0f;
        clampTargetSpeed(false);     // resta nell'intervallo 25..35
        markSettingsDirty();
        refreshTarget();
      }
      break;
    case P_COST_BPM:
      if (hrLive()) {
        targetHr = tel.hr;
        markSettingsDirty();
        refreshTarget();
      }
      break;
    case P_SETUP:
      zoneLim[setupField]--;
      if (zoneLim[setupField] < 60) zoneLim[setupField] = 60;
      for (int i = N_ZLIM - 1; i > 0; i--) if (zoneLim[i - 1] > zoneLim[i] - 1) zoneLim[i - 1] = zoneLim[i] - 1;
      markSettingsDirty();
      invalidateCache();
      drawFull();
      break;
    default:
      setBacklight(!backlightOn);   // su RIDE/DIAG: retroilluminazione
      break;
  }
}

// --- tasto PAGE (destro)
static void onPageShort() {
  if (page == P_SETUP) {            // in SETUP il tasto destro cambia il campo selezionato
    setupField++;
    if (setupField >= N_ZLIM) {     // dopo l'ultimo campo si passa alla pagina successiva
      setupField = 0;
      page = advancePage(page, +1);
    }
    invalidateCache();
    drawFull();
#if SERIAL_DEBUG
    Serial.printf("setup campo %d, pagina -> %d/%d\n", setupField, page + 1, pageCount);
#endif
    return;
  }
  page = advancePage(page, +1);
  drawFull();
#if SERIAL_DEBUG
  Serial.printf("pagina -> %d/%d\n", page + 1, pageCount);
#endif
}

static void onPageLong() {
  if (page != P_RIDE) {             // scorciatoia: torna alla pagina RIDE
    page = P_RIDE;
    drawFull();
  }
}

// pagina precedente (usata dal comando seriale di test)
static void onPagePrev() {
  if (page == P_SETUP) {
    setupField = (setupField == 0) ? (N_ZLIM - 1) : (setupField - 1);
    invalidateCache();
    drawFull();
    return;
  }
  page = advancePage(page, -1);
  drawFull();
}

static void handleButtons() {
  // I primi istanti dopo il boot i pin dei tasti non sono assestati (GPIO0 e'
  // anche pin di strapping): ignorali, altrimenti sfoglia le pagine da solo.
  if (millis() - bootTime < BUTTON_GUARD_MS) {
    btn1.last = digitalRead(btn1.pin);
    btn1.longFired = false;
    btn2.last = digitalRead(btn2.pin);
    btn2.longFired = false;
    return;
  }

  bool s1 = digitalRead(btn1.pin);
  bool s2 = digitalRead(btn2.pin);

  // --- entrambi i tasti premuti per >1 s: apre/chiude la pagina SOGLIE CARDIO
  static uint32_t bothSince = 0;
  static bool bothFired = false;
  if (s1 == LOW && s2 == LOW) {
    if (bothSince == 0) bothSince = millis();
    if (!bothFired && millis() - bothSince > 1000) {
      bothFired = true;
      if (page == P_SETUP) {
        page = P_RIDE;
      } else {
        page = P_SETUP;
        setupField = 0;
      }
#if SERIAL_DEBUG
      Serial.printf("pagina -> %s\n", pageTitle());
#endif
      invalidateCache();
      drawFull();
    }
    // durante la doppia pressione le azioni dei singoli tasti sono soppresse
    btn1.last = s1; btn1.longFired = true;
    btn2.last = s2; btn2.longFired = true;
    return;
  }
  if (bothSince != 0) {          // rilascio dopo la doppia pressione: nessuna azione
    bothSince = 0;
    bothFired = false;
    btn1.last = HIGH; btn1.longFired = true;
    btn2.last = HIGH; btn2.longFired = true;
    return;
  }

  // tasto SET ("sinistro")
  bool n1 = digitalRead(btn1.pin);
  if (n1 == LOW && btn1.last == HIGH) {
    btn1.tDown = millis();
    btn1.longFired = false;
  }
  if (n1 == LOW && !btn1.longFired && millis() - btn1.tDown > 800) {
    btn1.longFired = true;
    onSetLong();
  }
  if (n1 == HIGH && btn1.last == LOW) {
    if (!btn1.longFired && millis() - btn1.tDown > DEBOUNCE_MS) onSetShort();
  }
  btn1.last = n1;

  // tasto PAGE ("destro")
  bool n2 = digitalRead(btn2.pin);
  if (n2 == LOW && btn2.last == HIGH) {
    btn2.tDown = millis();
    btn2.longFired = false;
  }
  if (n2 == LOW && !btn2.longFired && millis() - btn2.tDown > 800) {
    btn2.longFired = true;
    onPageLong();
  }
  if (n2 == HIGH && btn2.last == LOW) {
    if (!btn2.longFired && millis() - btn2.tDown > DEBOUNCE_MS) onPageShort();
  }
  btn2.last = n2;
}

// ---------------------------------------------------------------- setup
// in avvio i task partono tutti "adesso": cosi' il jitter non conta il tempo dall'accensione
static void schedInit();

void setup() {
#if SERIAL_DEBUG
  Serial.begin(115200);
  { uint32_t t = millis(); while (millis() - t < 300) vTaskDelay(1); }   // USB CDC pronta (no delay)
  Serial.println("\nBikeGPS T-Display-S3");
#endif

  // alimentazione pannello: senza questo lo schermo resta nero
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);

  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  btn1.last = digitalRead(btn1.pin);
  btn2.last = digitalRead(btn2.pin);
  bootTime = millis();

  // batteria: partitore 1:2, attenuazione massima per arrivare a ~4,2 V
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  tft.init();
  tft.setRotation(ROTATION);
  setupGeometry();
  setBacklight(true);

  C_BG     = rgb(0x16, 0x17, 0x20);
  C_PANEL  = rgb(0x1f, 0x23, 0x35);
  C_BORDER = rgb(0x33, 0x3b, 0x57);
  C_FG     = rgb(0xc0, 0xca, 0xf5);
  C_DIM    = rgb(0x56, 0x5f, 0x89);
  C_BLUE   = rgb(0x7a, 0xa2, 0xf7);
  C_GREEN  = rgb(0x9e, 0xce, 0x6a);
  C_BATT   = rgb(0x55, 0x90, 0x3a);   // verde piu' scuro, solo per la batteria
  C_RED    = rgb(0xf7, 0x76, 0x8e);
  C_YELLOW = rgb(0xe0, 0xaf, 0x68);
  C_CYAN   = rgb(0x7d, 0xcf, 0xff);

  // colori delle 5 zone cardiache (grigio, verde, giallo, arancio, rosso)
  zoneCol[0] = rgb(0x6b, 0x73, 0x96);
  zoneCol[1] = rgb(0x9e, 0xce, 0x6a);
  zoneCol[2] = rgb(0xe0, 0xaf, 0x68);
  zoneCol[3] = rgb(0xf0, 0x8b, 0x4f);
  zoneCol[4] = rgb(0xf7, 0x76, 0x8e);

  // soglie, target e ultima pagina recuperati dalla flash
  loadSettings();
#if SERIAL_DEBUG
  printStatus();
#endif

  // misura iniziale batteria (media di qualche lettura, oversampling ADC)
  float acc = 0;
  for (int i = 0; i < 8; i++) acc += battVolts();
  battFiltered = acc / 8.0f;

  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_BLUE, C_BG);
  tft.drawString("BikeGPS", 10, 40, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("avvio BLE...", 10, 70, 2);

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

  // il primo disegno avviene subito; da qui in poi lo aggiorna il task display
  drawFull();
  schedInit();
}

// ---------------------------------------------------------------- task a tempo
// Nessun delay() di attesa: ogni task ha il suo periodo e viene eseguito quando scade.
static uint32_t schedRuns = 0, schedMaxJitter = 0;   // statistiche dello scheduler

static void taskBattery() {                 // 1 Hz: media esponenziale della batteria
  float v = battVolts();
  battFiltered = (battFiltered <= 0) ? v : (battFiltered * 0.9f + v * 0.1f);
}

static void taskTargetTime() {              // 1 Hz: secondi passati nel target
  if (page == P_COST_BPM) {
    if (hrLive() && abs(tel.hr - targetHr) <= TOLL_HR) timeInTarget++;
  } else if (page == P_COST_SPEED) {
    if ((gpsLive() || tel.lastRx != 0) && fabs(tel.spd - targetSpeed) <= TOLL_SPEED) timeInTarget++;
  }
}

static void taskDisplay() {                 // 4 Hz: ridisegna solo i valori cambiati (cache)
  if (!freezeDraw) drawValues();
}

static void taskHeader() { updateHeader(); }          // 1 Hz: GPS, BLE, batteria

// 2 Hz: alterna il cuore in acquisizione (1 Hz acceso/spento)
static void taskBlink() {
  hrBlinkOn = !hrBlinkOn;
  if (hrAcquiring() && !hrLive())
    drawHeartIcon(54, (HDR_H - 12) / 2, hrBlinkOn ? C_RED : C_DIM);
}

static void taskSaveSettings() {            // 1 Hz: salva in flash solo se serve
  if (!settingsDirty || millis() - settingsDirtyAt < 1000) return;
  saveSettings();
  settingsDirty = false;
}

// comandi di test da seriale: n = pagina avanti, p = indietro, b = retroilluminazione
//   d = riaccendi + ridisegna, s = stato pin/heap
//   r = schermo rosso pieno (test pannello diretto), v = verde, k = nero
static void taskSerial() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'n') {
      freezeDraw = false;
      onPageShort();
    } else if (c == 'p') {
      freezeDraw = false;
      onPagePrev();
    } else if (c == 'b') setBacklight(!backlightOn);
    else if (c == 's') printStatus();
    else if (c == 't') {
      // test: cattura il valore corrente come target della pagina allenamento
      onSetLong();
    } else if (c == 'z') {
      // test: +1 sul valore della pagina corrente
      onSetShort();
    } else if (c == 'g') {
      // test: forza il prossimo campo nella pagina SETUP
      setupField = (setupField + 1) % N_ZLIM;
      invalidateCache();
      drawFull();
    } else if (c == 'a') {
      // test: apre/chiude la pagina SOGLIE CARDIO
      if (page == P_SETUP) {
        page = P_RIDE;
      } else {
        page = P_SETUP;
        setupField = 0;
      }
      invalidateCache();
      drawFull();
#if SERIAL_DEBUG
      Serial.printf("pagina -> %s\n", pageTitle());
#endif
    } else if (c == 'r' || c == 'v' || c == 'k') {
      // test diretto sul pannello, senza passare dallo sprite
      uint16_t col = (c == 'r') ? TFT_RED : (c == 'v') ? TFT_GREEN : TFT_BLACK;
      freezeDraw = true;
      tft.fillScreen(col);
#if SERIAL_DEBUG
      Serial.printf("test pannello: fillScreen(0x%04X) colore=%c\n", col, c);
#endif
    }
    else if (c == 'm') {
      // benchmark del rendering: tempi REALI sul pannello, tutte le pagine.
      //   fillScreen = throughput puro del bus parallelo 8 bit
      //   drawFull   = layout + tutti i valori
      //   warm       = nessun valore cambiato (solo controllo cache)
      //   cold       = cache invalidata, ridisegna tutti i campi
      uint8_t save = page;
      Serial.printf("BENCH %s  cpu=%u MHz  bus=%u byte/pixel\n",
                    portrait ? "verticale 170x320" : "orizzontale 320x170",
                    (unsigned)getCpuFrequencyMhz(), 2);
      {
        uint32_t t0 = micros(); tft.fillScreen(C_BG); uint32_t t1 = micros();
        uint32_t px = (uint32_t)W * H;
        uint32_t us = t1 - t0;
        Serial.printf("  fillScreen = %lu us su %lu px  =>  %lu ns/px  (%.2f MB/s sul bus)\n",
                      (unsigned long)us, (unsigned long)px,
                      (unsigned long)(us * 1000UL / px), px * 2.0f / us);
      }
      for (uint8_t p = 0; p < PAGE_COUNT; p++) {
        if (p == P_SETUP) continue;      // la SETUP non si visita col giro tasti
        page = p;
        uint32_t t0 = micros(); tft.fillScreen(C_BG);  uint32_t t1 = micros();
        drawFull();                                     uint32_t t2 = micros();
        drawValues();                                   uint32_t t3 = micros();
        invalidateCache(); drawValues();                uint32_t t4 = micros();
        Serial.printf("  %-10s fill=%5luus  drawFull=%6luus  warm=%5luus  cold=%6luus\n",
                      pageTitle(),
                      (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                      (unsigned long)(t3 - t2), (unsigned long)(t4 - t3));
      }
      page = save;
      invalidateCache();
      drawFull();
      Serial.println("BENCH fine");
    }
    else if (c == 'q') {
      // profilo per componente: invalida UNA cache alla volta e misura drawValues().
      uint8_t save = page;
      uint32_t t0;
      auto misura = [&](const char *nome) {
        uint32_t t1 = micros();
        Serial.printf("  %-22s = %6lu us\n", nome, (unsigned long)(t1 - t0));
      };
      for (uint8_t p = 0; p < PAGE_COUNT; p++) {
        if (p == P_SETUP) continue;
        page = p;
        invalidateCache();
        drawFull();
        Serial.printf("PROFILO %s\n", pageTitle());
        t0 = micros(); drawValues(); misura("warm (nessun cambio)");
        if (page == P_RIDE) {
          lastHeroVal[0] = 0;
          t0 = micros(); drawValues(); misura("hero velocita");
        }
        if (page == P_RIDE || page == P_DIAG) {
          lastVals[0][0] = 0;
          t0 = micros(); drawValues(); misura("1 cella griglia");
          if (page == P_DIAG) {
            for (int i = 0; i < 8; i++) lastVals[i][0] = 0;
            t0 = micros(); drawValues(); misura("8 celle griglia");
          }
        }
        if (page == P_COST_SPEED || page == P_COST_BPM) {
          lastTgt[0] = 0;
          t0 = micros(); drawValues(); misura("riga TARGET");
          lastZoneShown = -9;
          t0 = micros(); drawValues(); misura("barra zone");
          lastHeroC[0] = 0;
          t0 = micros(); drawValues(); misura("hero + unita'");
          lastDev[0] = 0;
          t0 = micros(); drawValues(); misura("dev testo (no barra)");
          {
            float rg = (page == P_COST_SPEED) ? DEV_RANGE_SPEED : DEV_RANGE_HR;
            float tl = (page == P_COST_SPEED) ? TOLL_SPEED : TOLL_HR;
            t0 = micros();
            for (int i = 0; i < 20; i++) updateDevBar((i & 1) ? tl : -tl, rg, tl);
            uint32_t t1 = micros();
            Serial.printf("  %-22s = %6lu us  (%.0f us/movimento)\n",
                          "20x movimento barra", (unsigned long)(t1 - t0), (t1 - t0) / 20.0f);
          }
          lastInfo[0] = 0;
          t0 = micros(); drawValues(); misura("riga info");
          lastTime[0] = 0;
          t0 = micros(); drawValues(); misura("riga tempo");
          {
            float sSave = targetSpeed; int hrSave = targetHr;
            t0 = micros();
            for (int i = 0; i < 10; i++) onSetShort();   // 10 pressioni reali del tasto SET
            uint32_t t1 = micros();
            Serial.printf("  %-22s = %6lu us  (%.0f us/pressione)\n",
                          "10x tasto SET", (unsigned long)(t1 - t0), (t1 - t0) / 10.0f);
            targetSpeed = sSave; targetHr = hrSave; saveSettings();   // la NVS torna ai valori pre-test
            lastTgt[0] = 0; drawValues();
          }
        }
      }
      page = save;
      invalidateCache();
      drawFull();
      Serial.println("PROFILO fine");
    }
    else if (c == 'j') {
      // stato dello scheduler: quante esecuzioni e quanto sono in ritardo
      Serial.printf("scheduler: esecuzioni=%lu jitterMax=%lu ms | hrConnect=%d\n",
                    (unsigned long)schedRuns, (unsigned long)schedMaxJitter, (int)hrConnectResult);
      schedMaxJitter = 0;
    }
    else if (c == 'h') {
      // diagnostica cardio: stato della macchina, client, ultimo battito
      Serial.printf("hr: state=%d found=%d client=%p result=%d bpm=%d eta=%lums live=%d acq=%d scan=%d\n",
                    (int)hrState, (int)hrFound, (void *)hrClient, (int)hrConnectResult,
                    tel.hr, (unsigned long)(tel.hrLastRx ? millis() - tel.hrLastRx : 0),
                    (int)hrLive(), (int)hrAcquiring(),
                    (int)NimBLEDevice::getScan()->isScanning());
    }
    else if (c == 'c') {
      // test: simula una fascia trovata a un indirizzo inesistente. La richiesta
      // la inoltra la macchina a stati (HR_SCANNING), mai direttamente.
      hrAddress = NimBLEAddress(std::string("DE:AD:BE:EF:00:01"), BLE_ADDR_PUBLIC);
      hrFound = true;
      hrState = HR_SCANNING;
      Serial.println("test connect: fascia finta trovata");
    }
    else if (c == 'd') {
      printStatus();
      digitalWrite(PIN_LCD_POWER, HIGH);
      tft.init();                       // re-init del pannello
      tft.setRotation(ROTATION);
      setBacklight(true);
      freezeDraw = false;
      drawFull();
      printStatus();
    }
  }
}

// ---------------------------------------------------------------- scheduler
// Tabella di task periodici: il loop esegue i task scaduti e poi cede la CPU.
// vTaskDelay(1) e' uno yield al sistema operativo (1 ms), non un'attesa a tempo.
struct TaskDef { const char *name; uint32_t periodMs; uint32_t last; void (*fn)(); };
static TaskDef sched[] = {
  {"buttons",    10, 0, handleButtons},
  {"hr",         50, 0, hrTask},
  {"serial",     20, 0, taskSerial},
  {"display",   250, 0, taskDisplay},
  {"blink",     500, 0, taskBlink},
  {"header",   1000, 0, taskHeader},
  {"nvs",      1000, 0, taskSaveSettings},
  {"battery",  1000, 0, taskBattery},
  {"target",   1000, 0, taskTargetTime},
};
static const size_t N_TASKS = sizeof(sched) / sizeof(sched[0]);

static void schedInit() {
  const uint32_t t = millis();
  for (size_t i = 0; i < N_TASKS; i++) sched[i].last = t;
}

void loop() {
  const uint32_t now = millis();
  for (size_t i = 0; i < N_TASKS; i++) {
    uint32_t elapsed = now - sched[i].last;
    if (elapsed >= sched[i].periodMs) {
      uint32_t jit = elapsed - sched[i].periodMs;   // ritardo rispetto alla scadenza
      if (jit > schedMaxJitter) schedMaxJitter = jit;
      schedRuns++;
      sched[i].last = now;
      sched[i].fn();
    }
  }
  vTaskDelay(1);   // yield: lascia girare i task di sistema, niente busy-wait
}
