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

// ---------------------------------------------------------------- pin scheda
#define PIN_BTN1      0     // pulsante BOOT integrato (attivo basso)
#define PIN_BTN2      14    // pulsante 2 integrato (attivo basso)
#define PIN_LCD_POWER 15    // LCD_POWER_ON: va portato HIGH o il pannello resta nero
#define PIN_BAT       4     // partitore di misura batteria (1:2)

#define SERIAL_DEBUG 1

#define ROTATION 0          // 0/2 = verticale, 1/3 = orizzontale (cambia il verso)
#define HDR_H 26            // altezza della riga di stato in alto

TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------- colori (Tokyo Night)
static uint16_t C_BG, C_PANEL, C_BORDER, C_FG, C_DIM, C_BLUE, C_GREEN, C_RED, C_YELLOW, C_CYAN;
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

static bool hrConnect() {
  if (hrClient == nullptr) hrClient = NimBLEDevice::createClient();
  if (hrClient == nullptr) return false;
  if (!hrClient->connect(hrAddress)) {
    NimBLEDevice::deleteClient(hrClient);
    hrClient = nullptr;
    return false;
  }
  NimBLERemoteService *svc = hrClient->getService(HR_SERVICE_UUID);
  if (svc == nullptr) { hrClient->disconnect(); return false; }
  NimBLERemoteCharacteristic *ch = svc->getCharacteristic(HR_MEAS_UUID);
  if (ch == nullptr) { hrClient->disconnect(); return false; }
  if (!ch->subscribe(true, hrNotify)) { hrClient->disconnect(); return false; }

  // Intervallo lungo (~1 s): la Geonaute lo pretende, Android non glielo concede.
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
        scan->start(0, true, false);   // continua finche' non trovata
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
enum Page { P_RIDE, P_STATS, P_SYS, PAGE_COUNT };
static uint8_t page = P_RIDE;
static bool backlightOn = true;
static bool freezeDraw = false;   // usato dal test 'r'/'v'/'k' per non sovrascrivere il colore

// cache dei valori mostrati: si ridisegna solo se il testo cambia (meno flicker, meno lavoro)
static char lastVals[6][24];
static char lastHeroVal[24];
static bool lastHeroLive = false;
static char lastHeader[32];
static uint16_t lastHeaderCol = 0;

static void invalidateCache() {
  memset(lastVals, 0, sizeof(lastVals));
  lastHeroVal[0] = 0;
  lastHeroLive = false;
  lastHeader[0] = 0;
  lastHeaderCol = 0;
}

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
    if (mode == GRID_RIDE) {
      gRows = 2;
      gX = 4;
      gY = heroY + heroH + gap;
    } else {
      gRows = (nItems > 4) ? 3 : 2;
      gX = 4;
      gY = HDR_H + gap;
    }
    gW = (W - gap * 3) / 2;
    gH = ((H - gY - gap) - (gRows - 1) * gap) / gRows;
  } else {
    gCols = 3;
    gRows = 2;
    gX = 4;
    gY = HDR_H + 3;
    gW = (W - gap * 4) / 3;
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
static void drawHeaderStatic() {
  tft.fillRect(0, 0, W, HDR_H, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_BLUE, C_BG);
  tft.drawString("BikeGPS", 6, 5, 2);
}

// parte variabile dell'intestazione: stato BLE + batteria (solo se cambia)
static void updateHeader() {
  char st[32];
  snprintf(st, sizeof(st), "%s  %d%%", bleConnected ? "BLE" : "no BLE", battPercent());
  uint16_t col = bleConnected ? C_GREEN : C_RED;
  if (strcmp(st, lastHeader) == 0 && col == lastHeaderCol) return;

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(col, C_BG);
  tft.setTextPadding(100);          // cancella solo l'area del testo (anti-flicker)
  tft.drawString(st, W - 6, 5, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);

  strncpy(lastHeader, st, sizeof(lastHeader) - 1);
  lastHeader[sizeof(lastHeader) - 1] = 0;
  lastHeaderCol = col;
}

// --- formattazioni condivise
static void fmtTime(uint32_t t, char *buf, size_t n) {
  snprintf(buf, n, "%lu:%02lu:%02lu",
           (unsigned long)(t / 3600), (unsigned long)((t % 3600) / 60), (unsigned long)(t % 60));
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
static const char *const SYS_LABELS[6]   = {"BLE", "CARDIO bpm", "SATELLITI", "BATTERIA", "HEAP", "UPTIME"};

// ---------------------------------------------------------------- pagina RIDE
static void layoutRide() {
  // pannello velocita' + etichetta unita'
  tft.fillRoundRect(heroX, heroY, heroW, heroH, 6, C_PANEL);
  tft.drawRoundRect(heroX, heroY, heroW, heroH, 6, C_BORDER);
  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("km/h", heroX + heroW - 8, heroY + heroH - 6, 2);
  tft.setTextDatum(TL_DATUM);
  tft.drawFastHLine(heroX + 8, heroY + heroH - 24, heroW - 16, C_BORDER);

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
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(live ? C_FG : C_DIM, C_PANEL);
    tft.setTextPadding(heroW - 12);   // cancella solo l'area del testo (anti-flicker)
    tft.drawString(v, heroX + heroW / 2, heroY + heroH / 2 - 8, 7);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.setTextPadding(heroW - 16);
    tft.drawString(live ? "" : "no GPS", heroX + 8, heroY + heroH - 20, 2);
    tft.setTextPadding(0);
    strncpy(lastHeroVal, v, sizeof(lastHeroVal) - 1);
    lastHeroVal[sizeof(lastHeroVal) - 1] = 0;
    lastHeroLive = live;
  }

  const char *vals[6];
  uint16_t cols[6];

  if (portrait) {
    setGrid(GRID_RIDE, 4);
    statsValues(vals, cols);
    updateGrid(STATS_LABELS, vals, cols, 4);
    return;
  }

  // orizzontale
  const int gx = heroX + heroW + 4, gw = W - gx - 4;
  const int cy = heroY + heroH + 4, ch = H - cy - 4;
  const int cw = (heroW - 4) / 2;
  tft.setTextDatum(ML_DATUM);
  if (live) snprintf(v, sizeof(v), "%.2f", tel.dst); else strcpy(v, "--");
  drawFitted(heroX + 8, cy + ch - 16, cw, v, C_CYAN, C_PANEL);
  if (live) fmtTime(tel.mov, v, sizeof(v)); else strcpy(v, "--");
  drawFitted(heroX + cw + 12, cy + ch - 16, cw, v, C_CYAN, C_PANEL);
  if (live) fmtAvg(v, sizeof(v)); else strcpy(v, "--");
  drawFitted(gx + 8, heroY + (heroH - 4) / 2 - 16, gw, v, C_YELLOW, C_PANEL);
  if (live) snprintf(v, sizeof(v), "%.1f", tel.mx); else strcpy(v, "--");
  drawFitted(gx + 8, heroY + (heroH - 4) / 2 + 4 + (heroH - 4) / 2 - 16, gw, v, C_YELLOW, C_PANEL);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------- pagine a griglia
static void gridCell(int i, int &x, int &y) {
  x = gX + (i % gCols) * (gW + 4);
  y = gY + (i / gCols) * (gH + 4);
}

static void layoutGrid(const char *const labels[6], int n) {
  for (int i = 0; i < n; i++) {
    int x, y;
    gridCell(i, x, y);
    drawCell(x, y, gW, gH, labels[i], "--", C_FG);
  }
}

static void updateGrid(const char *const labels[6], const char *const vals[6], const uint16_t cols[6], int n) {
  (void)labels;
  tft.setTextDatum(ML_DATUM);
  for (int i = 0; i < n; i++) {
    if (strcmp(lastVals[i], vals[i]) == 0) continue;   // invariato: non ridisegnare
    int x, y;
    gridCell(i, x, y);
    drawFitted(x + 8, y + gH - 16, gW, vals[i], cols[i], C_PANEL);
    strncpy(lastVals[i], vals[i], sizeof(lastVals[i]) - 1);
    lastVals[i][sizeof(lastVals[i]) - 1] = 0;
  }
  tft.setTextDatum(TL_DATUM);
}

static void statsValues(const char *vals[6], uint16_t cols[6]) {
  static char a[24], b[24], c[24], d[24], e[24], f[24];
  bool live = gpsLive();
  if (live) snprintf(a, sizeof(a), "%.2f", tel.dst);      else snprintf(a, sizeof(a), "--");
  if (live) fmtTime(tel.mov, b, sizeof(b));               else snprintf(b, sizeof(b), "--");
  if (live) fmtAvg(c, sizeof(c));                         else snprintf(c, sizeof(c), "--");
  if (live) snprintf(d, sizeof(d), "%.1f", tel.mx);       else snprintf(d, sizeof(d), "--");
  if (live) snprintf(e, sizeof(e), "%d", tel.alt);        else snprintf(e, sizeof(e), "--");
  if (live) snprintf(f, sizeof(f), "%+.1f", tel.slp);     else snprintf(f, sizeof(f), "--");
  vals[0] = a; vals[1] = b; vals[2] = c; vals[3] = d; vals[4] = e; vals[5] = f;
  for (int i = 0; i < 6; i++) cols[i] = live ? C_FG : C_DIM;
}

static void sysValues(const char *vals[6], uint16_t cols[6]) {
  static char a[24], b[24], c[24], d[24], e[24], f[24];
  snprintf(a, sizeof(a), "%s", bleConnected ? "connesso" : "assente");
  if (hrLive()) snprintf(b, sizeof(b), "%d", tel.hr);
  else          snprintf(b, sizeof(b), "--");
  if (gpsLive()) snprintf(c, sizeof(c), "%d", tel.sat);
  else           snprintf(c, sizeof(c), "--");
  snprintf(d, sizeof(d), "%d%% (%.2fV)", battPercent(), battFiltered);
  snprintf(e, sizeof(e), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  fmtUptime(f, sizeof(f));
  vals[0] = a; vals[1] = b; vals[2] = c; vals[3] = d; vals[4] = e; vals[5] = f;
  cols[0] = bleConnected ? C_GREEN : C_RED;
  cols[1] = hrLive() ? C_RED : C_DIM;
  cols[2] = gpsLive() ? C_GREEN : C_DIM;
  cols[3] = (battPercent() < 20) ? C_RED : (battPercent() < 40 ? C_YELLOW : C_GREEN);
  cols[4] = C_DIM;
  cols[5] = C_DIM;
}

// ---------------------------------------------------------------- disegno completo
static void drawFull() {
  const char *vals[6];
  uint16_t cols[6];
  tft.fillScreen(C_BG);
  invalidateCache();
  drawHeaderStatic();
  switch (page) {
    case P_RIDE:
      layoutRide();
      updateRide();
      break;
    case P_STATS:
      setGrid(GRID_FULL, 6);
      layoutGrid(STATS_LABELS, 6);
      statsValues(vals, cols);
      updateGrid(STATS_LABELS, vals, cols, 6);
      break;
    case P_SYS:
      setGrid(GRID_FULL, 6);
      layoutGrid(SYS_LABELS, 6);
      sysValues(vals, cols);
      updateGrid(SYS_LABELS, vals, cols, 6);
      break;
  }
}

static void drawValues() {
  const char *vals[6];
  uint16_t cols[6];
  updateHeader();
  switch (page) {
    case P_RIDE:
      updateRide();
      break;
    case P_STATS:
      setGrid(GRID_FULL, 6);
      statsValues(vals, cols);
      updateGrid(STATS_LABELS, vals, cols, 6);
      break;
    case P_SYS:
      setGrid(GRID_FULL, 6);
      sysValues(vals, cols);
      updateGrid(SYS_LABELS, vals, cols, 6);
      break;
  }
}

// ---------------------------------------------------------------- pulsanti
struct Button { uint8_t pin; bool last; uint32_t tDown; bool longFired; };
static Button btn1 = {PIN_BTN1, HIGH, 0, false};
static Button btn2 = {PIN_BTN2, HIGH, 0, false};

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
#endif
}

static void handleButtons() {
  // BTN1: corto = pagina avanti, lungo = retroilluminazione
  bool n1 = digitalRead(btn1.pin);
  if (n1 == LOW && btn1.last == HIGH) {
    btn1.tDown = millis();
    btn1.longFired = false;
#if SERIAL_DEBUG
    Serial.println("btn1 premuto");
#endif
  }
  if (n1 == LOW && !btn1.longFired && millis() - btn1.tDown > 800) {
    btn1.longFired = true;
    setBacklight(!backlightOn);
  }
  if (n1 == HIGH && btn1.last == LOW) {
    if (!btn1.longFired && millis() - btn1.tDown > 30) {
      page = (page + 1) % pageCount;
#if SERIAL_DEBUG
      Serial.printf("pagina -> %d/%d\n", page + 1, pageCount);
#endif
      drawFull();
    }
  }
  btn1.last = n1;

  // BTN2: pagina indietro
  bool n2 = digitalRead(btn2.pin);
  if (n2 == LOW && btn2.last == HIGH) {
    btn2.tDown = millis();
#if SERIAL_DEBUG
    Serial.println("btn2 premuto");
#endif
  }
  if (n2 == HIGH && btn2.last == LOW && millis() - btn2.tDown > 30) {
    page = (page + pageCount - 1) % pageCount;
#if SERIAL_DEBUG
    Serial.printf("pagina -> %d/%d\n", page + 1, pageCount);
#endif
    drawFull();
  }
  btn2.last = n2;
}

// ---------------------------------------------------------------- setup
void setup() {
#if SERIAL_DEBUG
  Serial.begin(115200);
  delay(300);
  Serial.println("\nBikeGPS T-Display-S3");
#endif

  // alimentazione pannello: senza questo lo schermo resta nero
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);

  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);

  // batteria: partitore 1:2, attenuazione massima per arrivare a ~4,2 V
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  tft.init();
  tft.setRotation(ROTATION);
  setupGeometry();
  setBacklight(true);

  // buffer di disegno (prima del BLE: cosi' l'allocazione ha tutta la RAM libera)

  C_BG     = rgb(0x16, 0x17, 0x20);
  C_PANEL  = rgb(0x1f, 0x23, 0x35);
  C_BORDER = rgb(0x33, 0x3b, 0x57);
  C_FG     = rgb(0xc0, 0xca, 0xf5);
  C_DIM    = rgb(0x56, 0x5f, 0x89);
  C_BLUE   = rgb(0x7a, 0xa2, 0xf7);
  C_GREEN  = rgb(0x9e, 0xce, 0x6a);
  C_RED    = rgb(0xf7, 0x76, 0x8e);
  C_YELLOW = rgb(0xe0, 0xaf, 0x68);
  C_CYAN   = rgb(0x7d, 0xcf, 0xff);

  // misura iniziale batteria (media di qualche lettura)
  float acc = 0;
  for (int i = 0; i < 8; i++) { acc += battVolts(); delay(5); }
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

  delay(600);
  drawFull();
}

// ---------------------------------------------------------------- loop
void loop() {
  handleButtons();
  hrTask();

  // batteria: media esponenziale, una lettura ogni secondo
  static uint32_t lastBatt = 0;
  if (millis() - lastBatt >= 1000) {
    lastBatt = millis();
    float v = battVolts();
    battFiltered = (battFiltered <= 0) ? v : (battFiltered * 0.9f + v * 0.1f);
  }

  static uint32_t lastDraw = 0;
  if (!freezeDraw && millis() - lastDraw >= 250) {
    lastDraw = millis();
    drawValues();
  }

  // comandi di test da seriale: n = pagina avanti, p = indietro, b = retroilluminazione
  //   d = riaccendi + ridisegna, s = stato pin/heap
  //   r = schermo rosso pieno (test pannello diretto), v = verde, k = nero
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'n') {
      freezeDraw = false;
      page = (page + 1) % pageCount;
#if SERIAL_DEBUG
      Serial.printf("pagina -> %d/%d\n", page + 1, pageCount);
#endif
      drawFull();
    } else if (c == 'p') {
      freezeDraw = false;
      page = (page + pageCount - 1) % pageCount;
#if SERIAL_DEBUG
      Serial.printf("pagina -> %d/%d\n", page + 1, pageCount);
#endif
      drawFull();
    } else if (c == 'b') setBacklight(!backlightOn);
    else if (c == 's') printStatus();
    else if (c == 'r' || c == 'v' || c == 'k') {
      // test diretto sul pannello, senza passare dallo sprite
      uint16_t col = (c == 'r') ? TFT_RED : (c == 'v') ? TFT_GREEN : TFT_BLACK;
      freezeDraw = true;
      tft.fillScreen(col);
#if SERIAL_DEBUG
      Serial.printf("test pannello: fillScreen(0x%04X) colore=%c\n", col, c);
#endif
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
  delay(5);
}
