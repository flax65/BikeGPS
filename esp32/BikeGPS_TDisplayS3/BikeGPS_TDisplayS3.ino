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
 * Consumo: profilo a basso consumo attivo di default (blocco "risparmio energia"
 * in cima): WiFi spento, CPU a 80 MHz, advertising BLE a 500 ms e backlight
 * regolabile. Per tornare al profilo pieno compilare con -DLP_ENABLE=0.
 *
 * ATTENZIONE: compila e carica con lo STESSO FQBN, altrimenti
 * arduino-cli puo' sbagliare i parametri di flash ("Unexpected chip ID").
 */

// ---------------------------------------------------------------- risparmio energia
// BikeGPS non usa il WiFi: lo spegniamo esplicitamente e abbassiamo la CPU.
// Le manopole si possono forzare in compilazione senza toccare il file, es.:
//   arduino-cli compile -b esp32:esp32:lilygo_t_display_s3 \
//     --build-property "compiler.cpp.extra_flags=-DLP_CPU_MHZ=240 -DLP_ENABLE=0" .
// (questo blocco sta PRIMA degli include: LP_WIFI_OFF decide se includere WiFi.h)
#ifndef LP_ENABLE
#define LP_ENABLE     1     // 1 = profilo a basso consumo attivo, 0 = come prima
#endif
#ifndef LP_CPU_MHZ
#define LP_CPU_MHZ    80    // 240 default / 160 compromesso / 80 minimo con BLE
                            // (40 MHz NON e' affidabile con il controller BLE)
#endif
#ifndef LP_WIFI_OFF
#define LP_WIFI_OFF   1     // spegne il WiFi (mai inizializzato da BikeGPS)
#endif
#ifndef LP_ADV_MS
#define LP_ADV_MS     500   // periodo advertising BLE in ms (0 = default 100 ms)
                            // piu' lungo = meno consumo, ma il telefono ci mette
                            // piu' a ritrovare la scheda dopo una disconnessione
#endif
#ifndef LP_BL_DUTY
#define LP_BL_DUTY    255   // retroilluminazione 0..255 (255 = 100%, 128 = 50%)
#endif

#include <TFT_eSPI.h>
#include <NimBLEDevice.h>
#include <Preferences.h>   // soglie e target salvati in flash (NVS)
#include <esp_sleep.h>     // deep sleep: l'unico "power off" possibile (niente load switch)
#include <driver/rtc_io.h> // pull-up RTC per il risveglio con i tasti
#include "vlw_fonts.h"      // font VLW antialiased generati (16/26/48 px)
#if LP_WIFI_OFF
#include <WiFi.h>          // solo per spegnere esplicitamente il WiFi
#endif

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

#define ROTATION 1          // 0/2 = verticale, 1/3 = orizzontale (cambia il verso)

// Rendering della velocita' nel riquadro RIDE:
//   0 = font bitmap di TFT_eSPI (bello, ma dimensione fissa)
//   1 = 7 segmenti vettoriale (scalabile, ma estetica "a rettangoli")
// Simulazione delle grandezze (velocita'/cadenza/watt) attiva all'avvio:
// comoda in fase di test, da mettere a 0 a fine sviluppo. Si comanda anche col tasto 'y'.
// Parametri di connessione cardio: 1 = 800/800/0/600 (come sul vecchio ESP32), 0 = non toccarli
#define HR_CONN_PARAMS 1

#define SIM_DEFAULT_ON 0

#define HERO_RENDER_SEG 0
#define HDR_H 26            // altezza della riga di stato in alto

// ---------------------------------------------------------- modalita' test durata batteria
// PROVVISORIA: datalogger della tensione batteria in flash + carico ciclico
// automatico, per misurare quanto dura la batteria. Rimettere TEST_MODE a 0
// a fine test (la versione normale non deve cambiare pagina da sola).
#define TEST_MODE         0     // 1 = datalogger + cambio pagina automatici
#ifndef TEST_LOG_PERIOD_S
#define TEST_LOG_PERIOD_S 300   // campionamento batteria (s): 5 min
#endif
#ifndef TEST_PAGE_S
#define TEST_PAGE_S       30    // cambio pagina automatico (carico ciclico)
#endif
#ifndef TEST_LOG_MAX
#define TEST_LOG_MAX      256   // record conservati in flash (ring) = 21 h
#endif
#ifndef BATT_MAH
#define BATT_MAH          140   // capacita' dichiarata della batteria (mAh)
#endif

TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------- colori (Tokyo Night)
static uint16_t C_BG, C_PANEL, C_BORDER, C_FG, C_DIM, C_BLUE, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_BATT, C_SEG_EDGE;
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

  // sensori BLE futuri: cadenza (CSC 0x1816) e potenza (CPS 0x1818)
  int      cad = 0;      // rpm
  int      pw = 0;       // W
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
static uint32_t hrReadyAt = 0;   // millis del passaggio a HR_READY
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
#if SERIAL_DEBUG
    else if (dev->haveName()) {     // log degli altri device visti (diagnostica scansione)
      Serial.printf("scan: %s [%s] rssi=%d\n", dev->getName().c_str(),
                    dev->getAddress().toString().c_str(), dev->getRSSI());
    }
#endif
  }
};
static HrScanCallbacks hrScanCallbacks;
static uint32_t hrScanAt = 0;      // inizio dell'ultima scansione (per il retry)

// Avvia la ricerca in modo PULITO: stop, poi start con restart. Con i duplicati
// attivi il callback onResult arriva anche se la fascia era gia' stata vista:
// senza questo, dopo una perdita non viene piu' ritrovata (hrFound resta false).
static void hrScanStart() {
  // Stessi parametri del vecchio sketch (che funziona): niente duplicati e
  // start semplice. Con i duplicati attivi il controller riportava migliaia di
  // pacchetti e la Geonaute non veniva mai consegnata.
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->stop();
  scan->setScanCallbacks(&hrScanCallbacks, false);
  scan->setActiveScan(true);
  scan->start(0, true, false);
  hrScanAt = millis();
}

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
    // La Geonaute sul vecchio ESP32 pretendeva un intervallo lungo (~1 s).
    // Sul controller dell'S3 questo puo' non essere applicato: si prova a non toccarlo.
#if HR_CONN_PARAMS
    hrClient->updateConnParams(800, 800, 0, 600);
#endif
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
        hrScanStart();
        hrStateTo(HR_SCANNING, "avvio scansione");
      }
      break;

    case HR_SCANNING:
      if (hrFound) {
        NimBLEDevice::getScan()->stop();
        hrStateTo(HR_CONNECTING, "fascia trovata");
      }
      break;

    case HR_CONNECTING:
      // Connessione NEL LOOP, come nella versione che funzionava.
      // (connect+subscribe sono sincroni: il display si ferma per la durata)
      hrConnectResult = hrConnect() ? 1 : 0;
      if (hrConnectResult > 0) {
        hrReadyAt = millis();
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
      } else {
        // connesso ma senza battito: se il segnale si perde (o non arriva mai)
        // la connessione e' "zombie" -> si riparte da capo, come dopo un'accensione.
        // Il primo battito puo' richiedere qualche secondo: 10 s di grazia; poi 3 s.
        const uint32_t age   = tel.hrLastRx ? (millis() - tel.hrLastRx) : (millis() - hrReadyAt);
        const uint32_t limit = tel.hrLastRx ? 3000 : 10000;
        if (age > limit) {
          NimBLEDevice::deleteClient(hrClient);
          hrClient = nullptr;
          tel.hr = 0;
          hrNextTry = millis() + 1000;
          hrStateTo(HR_IDLE, "nessun battito da 3 s");
        }
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

#if TEST_MODE
// ---------------------------------------------------------------- datalogger batteria
// Ring buffer di campioni in NVS (namespace "blog"): sopravvive alla scarica
// completa e a un eventuale reset. Letto col comando seriale 'l'.
struct __attribute__((packed)) BattRec { uint32_t t; uint16_t mv; };   // 6 byte
static BattRec blogBuf[TEST_LOG_MAX];
static uint16_t blogCount = 0;      // campioni validi
static uint16_t blogPos   = 0;      // prossima posizione da scrivere (ring)

static void blogSave() {
  Preferences p;
  p.begin("blog", false);
  p.putUShort("n", blogCount);
  p.putUShort("p", blogPos);
  p.putBytes("r", blogBuf, sizeof(blogBuf));
  p.end();
}

static void blogLoad() {
  Preferences p;
  p.begin("blog", true);
  blogCount = p.getUShort("n", 0);
  blogPos   = p.getUShort("p", 0);
  p.getBytes("r", blogBuf, sizeof(blogBuf));
  p.end();
  if (blogCount > TEST_LOG_MAX) blogCount = TEST_LOG_MAX;
  if (blogPos >= TEST_LOG_MAX)  blogPos = 0;
}

static void blogReset() {
  blogCount = 0;
  blogPos = 0;
  blogSave();
}

static void blogSample() {
  float v = (battFiltered > 0.5f) ? battFiltered : battVolts();
  blogBuf[blogPos].t  = millis();
  blogBuf[blogPos].mv = (uint16_t)(v * 1000.0f + 0.5f);
  blogPos = (uint16_t)((blogPos + 1) % TEST_LOG_MAX);
  if (blogCount < TEST_LOG_MAX) blogCount++;
  blogSave();
#if SERIAL_DEBUG
  Serial.printf("battlog: %u campioni, ultimo %u mV @ %lus\n",
                blogCount, blogBuf[(blogPos + TEST_LOG_MAX - 1) % TEST_LOG_MAX].mv,
                (unsigned long)(millis() / 1000));
#endif
}

static int recPct(uint16_t mv) {
  int p = (int)((mv / 1000.0f - 3.30f) / 0.90f * 100.0f + 0.5f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

static void printDrainLog() {
  Serial.printf("=== CURVA DI SCARICA === batteria %u mAh, campioni ogni %us\n",
                (unsigned)BATT_MAH, (unsigned)TEST_LOG_PERIOD_S);
  if (blogCount == 0) { Serial.println("log vuoto (nessun campione)"); return; }
  const uint16_t start = (uint16_t)((blogPos + TEST_LOG_MAX - blogCount) % TEST_LOG_MAX);
  BattRec *prev = nullptr;
  bool nonMono = false;
  for (uint16_t i = 0; i < blogCount; i++) {
    BattRec *r = &blogBuf[(start + i) % TEST_LOG_MAX];
    if (prev && r->t < prev->t) nonMono = true;
    Serial.printf("  %8.2f h  %5u mV  %3d%%\n", r->t / 3600000.0f, r->mv, recPct(r->mv));
    prev = r;
  }
  BattRec *f = &blogBuf[start];
  BattRec *l = &blogBuf[(blogPos + TEST_LOG_MAX - 1) % TEST_LOG_MAX];
  const float dtH = (l->t - f->t) / 3600000.0f;
  const float p0  = recPct(f->mv), p1 = recPct(l->mv);
  Serial.printf("--- riepilogo ---\n  campioni=%u  durata=%.2f h  %.2f -> %.2f V\n",
                blogCount, dtH, f->mv / 1000.0f, l->mv / 1000.0f);
  if (nonMono) Serial.println("  ATTENZIONE: tempo non monotono (reset durante il test?)");
  if (dtH <= 0.0f) { Serial.println("  dati insufficienti per la stima"); return; }
  const float dPct = p0 - p1;
  if (dPct <= 0.5f) { Serial.println("  batteria stabile o in carica: nessuna stima"); return; }
  const float rate = dPct / dtH;                             // %/h
  Serial.printf("  scarica: %.1f %% in %.2f h  =>  %.1f %%/h\n", dPct, dtH, rate);
  Serial.printf("  autonomia da %.0f%%: %.1f h (%.1f giorni)\n", p0, p0 / rate, p0 / rate / 24.0f);
  Serial.printf("  autonomia da 100%%: %.1f h (%.1f giorni)\n", 100.0f / rate, 100.0f / rate / 24.0f);
  Serial.printf("  corrente media stimata: %.1f mA\n", (float)BATT_MAH * rate / 100.0f);
}
#endif  // TEST_MODE

// ---------------------------------------------------------------- pagine
enum Page { P_RIDE, P_SETUP, P_DIAG, PAGE_COUNT };
static uint8_t page = P_RIDE;
static bool backlightOn = true;
static uint8_t blDuty = LP_BL_DUTY;   // luminosita' corrente (modificabile a caldo col comando 'B')

// ---------------------------------------------------------- risparmio: luminosita' adattiva
// 100% per i primi BL_FULL_MS, poi rampa lineare fino a BL_MIN_DUTY in BL_RAMP_MS,
// poi display spento (DISPOFF + backlight 0). Qualsiasi tasto riporta al 100% e
// ricomincia il ciclo. Il comando 'B' disattiva la logica per i test manuali, 'A' la riattiva.
#define BL_OFF_ENABLE    0                   // 1 = dopo la fase al 10% spegne il display; 0 = resta al 10%
#define BL_FULL_MS       10000UL             // 10 s a piena luminosita'
#define BL_RAMP_MS       60000UL             // 60 s di rampa 100% -> 10%
#define BL_MIN_HOLD_MS   60000UL             // (solo se BL_OFF_ENABLE) quanto resta al 10% prima di spegnersi
#define BL_MIN_DUTY      26                  // 10% di 255
#define BL_OFF_AFTER_MS  (BL_FULL_MS + BL_RAMP_MS + BL_MIN_HOLD_MS)
static bool     autoDim    = true;           // logica automatica attiva
static uint32_t blIdleAt   = 0;              // ultimo tasto premuto
static bool     blPanelOff = false;          // pannello in DISPOFF
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

// ---------------------------------------------------------------- allenamento (soglie cardio)
// Soglie cardiache = limiti Z1|Z2, Z2|Z3, Z3|Z4, Z4|Z5 (bpm).
// Default per FCmax 170: 102 / 119 / 136 / 153.
#define ZONE_COUNT 5
#define N_ZLIM     4
static int zoneLim[N_ZLIM] = {102, 119, 136, 153};
static const char *const ZONE_NAMES[ZONE_COUNT] = {"Z1", "Z2", "Z3", "Z4", "Z5"};
static uint16_t zoneCol[ZONE_COUNT];       // riempiti in setup()

static uint8_t setupField = 0;             // campo selezionato nella pagina SETUP
static Preferences prefs;

// cache dei valori mostrati: si ridisegna solo se il testo cambia (meno flicker, meno lavoro)
static char lastVals[8][24];
static uint16_t lastValsCol[8];
static char lastHeroVal[24];
static bool lastHeroLive = false;
static int  lastHeaderState = -1;  // BLE + GPS + stato colore cuore (-1 = da disegnare)
static int  lastBattPct = -1;      // percentuale batteria disegnata (segmenti)
static bool hrBlinkOn = false;     // lampeggio del cuore in acquisizione

// simulazione delle grandezze per provare il display senza sensori (comando 'y')
static bool  simEnabled = SIM_DEFAULT_ON;
static float simSpd = 0.0f, simCad = 0.0f, simPw = 0.0f;
static int   simDir = 1, simCadDir = 1, simPwDir = 1;   // +1 sale, -1 scende

// --- trip: tempo di allenamento con auto-pausa sotto i 4 km/h ---
static bool     tripActive = false;
static uint32_t tripMs = 0;            // tempo accumulato (ms)
static uint32_t tripLast = 0;
#define TRIP_MIN_SPEED 4.0f            // km/h: sotto questa il cronometro si ferma

static void tripStart() {
  tripActive = true;
  tripMs = 0;
  tripLast = millis();
  lastVals[0][0] = 0;
}

static void tripStop() { tripActive = false; }

static void taskTrip() {               // 100 ms: accumula solo sopra i 4 km/h
  if (!tripActive) return;
  const uint32_t now = millis();
  const uint32_t dt = now - tripLast;
  tripLast = now;
  const float spd = simEnabled ? simSpd : tel.spd;
  if (spd >= TRIP_MIN_SPEED) tripMs += dt;
}


// velocita' a 7 segmenti vettoriale (scalabile al riquadro)
static float heroSegUnits = 0;     // unita' di larghezza per cui e' tarata la geometria
static int   heroSegLen = -1;      // numero di caratteri disegnati
static bool  heroSegLive = true;   // colore usato nell'ultimo disegno
static uint8_t heroSegDrawn[8];    // maschera segmenti disegnata per posizione
static bool    heroSegInit[8];     // posizione gia' disegnata (per la sagoma dei segmenti spenti)
static int segW, segH, segT, segGap, segY;
// cache delle pagine di allenamento
// cache della pagina SETUP
static int lastSetupField = -1;
static int lastSetupVal[N_ZLIM] = {-1, -1, -1, -1};

static int lastZoneShown = -9;      // cache della barra zone (pagina SETUP)

static void invalidateCache() {
  memset(lastVals, 0, sizeof(lastVals));
  memset(lastValsCol, 0, sizeof(lastValsCol));
  lastHeroVal[0] = 0;
  lastHeroLive = false;
  lastHeaderState = -1;
  lastBattPct = -1;
  heroSegLen = -1;
  memset(heroSegDrawn, 0, sizeof(heroSegDrawn));
  memset(heroSegInit, 0, sizeof(heroSegInit));
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
static int bpmX, bpmY, bpmW, bpmH;      // cella del battito, accanto alla velocita'

// sprite dedicato alla velocita': si disegna in RAM e si riversa in UNA passata
// (i font VLW non hanno setTextPadding, quindi il metodo diretto farebbe flicker)
static TFT_eSprite heroSprite = TFT_eSprite(&tft);
static bool heroSpriteOk = false;
// TFT_eSprite vuole il puntatore al TFT nel costruttore: tre istanze + indice
static TFT_eSprite sprCell0(&tft), sprCell1(&tft), sprCell2(&tft);
static TFT_eSprite *sprCell[3] = {&sprCell0, &sprCell1, &sprCell2};
static TFT_eSprite sprBpm(&tft);
static bool sprOk = false;

// riga di celle sotto i pannelli: i bordi si allineano a quelli sopra
//   [ TEMPO ][ CADENZA ] sotto la velocita'  |  [ POT W ] sotto il BPM
static int pcX[3], pcW[3], pcY, pcH;
static void rideCellsGeometry() {
  const int gap = 4;
  if (portrait) {
    pcY = bpmY + bpmH + gap;
    pcH = H - pcY - gap;
    const int w3 = (W - 8 - gap * 2) / 3;
    for (int i = 0; i < 3; i++) { pcX[i] = 4 + i * (w3 + gap); pcW[i] = w3; }
  } else {
    pcY = heroY + heroH + gap;
    pcH = H - pcY - gap;
    const int w2 = (heroW - gap) / 2;
    pcX[0] = heroX;             pcW[0] = w2;                   // tempo
    pcX[1] = heroX + w2 + gap;  pcW[1] = heroW - w2 - gap;    // cadenza (chiude a filo velocita')
    pcX[2] = bpmX;              pcW[2] = bpmW;                 // potenza (a filo BPM)
  }
}

// griglia corrente (impostata da setGrid)
static int gCols, gRows, gX, gY, gW, gH;

// GRID_FULL = griglia della pagina intera; GRID_RIDE = griglia sotto l'hero (solo verticale)
#define GRID_FULL 0
#define GRID_RIDE 1

static void setGrid(uint8_t mode, int nItems) {
  const int gap = 4;
  // griglia della RIDE: UNA riga di celle sotto il pannello velocita'
  if (mode == GRID_RIDE) {
    gRows = 1;
    gCols = nItems;
    gX = 4;
    gY = (portrait ? bpmY + bpmH : heroY + heroH) + gap;
    gW = (W - gap * (gCols + 1)) / gCols;
    gH = H - gY - gap;
    return;
  }
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
    pageCount = PAGE_COUNT;
    heroX = 4;
    heroY = 2;
    heroW = W - 8;
    heroH = 110;
    bpmX = 4; bpmY = heroY + heroH + 4; bpmW = W - 8; bpmH = 56;
  } else {
    pageCount = PAGE_COUNT;
    // orizzontale: tutto dal bordo superiore (l'header non e' piu' una banda a parte);
    // velocita' a sinistra, BPM affiancato a destra, tre celle sotto
    heroX = 4;
    heroY = 2;
    heroW = 198;
    heroH = 100;
    bpmX = heroX + heroW + 4;
    bpmY = heroY;
    bpmW = W - 4 - bpmX;
    bpmH = heroH;
  }
  rideCellsGeometry();
  // sprite della velocita' (stessa larghezza del pannello, 56 px di altezza)
  heroSprite.setColorDepth(16);
  heroSpriteOk = (heroSprite.createSprite(heroW - 8, 84) != nullptr);
  sprOk = true;
  for (int i = 0; i < 3; i++) {
    sprCell[i]->setColorDepth(16);
    if (sprCell[i]->createSprite(pcW[i], pcH - 26) == nullptr) sprOk = false;
  }
  sprBpm.setColorDepth(16);
  if (sprBpm.createSprite(bpmW, bpmH - 28) == nullptr) sprOk = false;
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

// ------------------------------------------------ velocita' 7 segmenti vettoriale
// Ogni cifra e' fatta di 7 segmenti disegnati come rettangoli: la dimensione si
// calcola dal riquadro (heroW x heroH), quindi la velocita' lo riempie davvero.
// Cache per posizione: si ridisegnano solo i segmenti che cambiano.
#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40
#define SEG_DOT 0x80

static const uint8_t SEG_DIGITS[10] = {
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,              // 0
  SEG_B|SEG_C,                                      // 1
  SEG_A|SEG_B|SEG_G|SEG_E|SEG_D,                    // 2
  SEG_A|SEG_B|SEG_G|SEG_C|SEG_D,                    // 3
  SEG_F|SEG_G|SEG_B|SEG_C,                          // 4
  SEG_A|SEG_F|SEG_G|SEG_C|SEG_D,                    // 5
  SEG_A|SEG_F|SEG_G|SEG_E|SEG_C|SEG_D,              // 6
  SEG_A|SEG_B|SEG_C,                                // 7
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,        // 8
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,              // 9
};

// --- primitive dei 7 segmenti vettoriali (il secondo decimale le usa sempre;
//     il rendering completo del riquadro si attiva con HERO_RENDER_SEG == 1) ---
static uint8_t segMask(char c) {
  if (c >= '0' && c <= '9') return SEG_DIGITS[c - '0'];
  if (c == '-') return SEG_G;
  if (c == '.') return SEG_DOT;
  return 0;
}

#if HERO_RENDER_SEG

// larghezza della stringa in "unita'": cifre 1.0, punto 0.30, spazio 0.06
static float heroUnits(const char *s) {
  float u = 0;
  for (int i = 0; s[i]; i++) {
    if (i) u += 0.06f;
    u += (s[i] == '.') ? 0.30f : 1.0f;
  }
  return u;
}

static void heroSegLayout(float units) {
  const int MX = 10, MY = 10;
  int availW = heroW - 2 * MX, availH = heroH - 2 * MY;
  float h = (availW / units) / 0.58f;    // larghezza cifra = 0.58 * altezza
  if (h > availH) h = availH;            // vincolo verticale
  segH = (int)h;
  segW = (int)(segH * 0.58f);
  segT = segH / 8; if (segT < 3) segT = 3;
  segGap = segW / 12; if (segGap < 2) segGap = 2;
  segY = heroY + (heroH - segH) / 2;
}

static void heroClear() {
  tft.fillRect(heroX + 1, segY - 2, heroW - 2, segH + 4, C_PANEL);
  memset(heroSegDrawn, 0, sizeof(heroSegDrawn));
  memset(heroSegInit, 0, sizeof(heroSegInit));
}
#endif  // HERO_RENDER_SEG (layout a celle del riquadro velocita')

// --- segmenti come trapezi: riempimento di un colore + bordo di un altro
static void segTrapH(int x, int y, int w, int t, uint16_t fill, uint16_t edge) {
  int p = t / 2; if (p < 1) p = 1;
  tft.fillTriangle(x, y, x + w, y, x + p, y + t, fill);
  tft.fillTriangle(x + w, y, x + w - p, y + t, x + p, y + t, fill);
  tft.drawLine(x, y, x + w, y, edge);
  tft.drawLine(x + w, y, x + w - p, y + t, edge);
  tft.drawLine(x + w - p, y + t, x + p, y + t, edge);
  tft.drawLine(x + p, y + t, x, y, edge);
}

// verticale con base esterna a sinistra (segmenti F, E)
static void segTrapV(int x, int y, int h, int t, uint16_t fill, uint16_t edge) {
  int p = t / 2; if (p < 1) p = 1;
  tft.fillTriangle(x, y, x + t, y + p, x, y + h, fill);
  tft.fillTriangle(x + t, y + p, x + t, y + h - p, x, y + h, fill);
  tft.drawLine(x, y, x + t, y + p, edge);
  tft.drawLine(x + t, y + p, x + t, y + h - p, edge);
  tft.drawLine(x + t, y + h - p, x, y + h, edge);
  tft.drawLine(x, y + h, x, y, edge);
}

// verticale con base esterna a destra (segmenti B, C)
static void segTrapVR(int x, int y, int h, int t, uint16_t fill, uint16_t edge) {
  int p = t / 2; if (p < 1) p = 1;
  tft.fillTriangle(x, y + p, x + t, y, x + t, y + h, fill);
  tft.fillTriangle(x, y + p, x + t, y + h, x, y + h - p, fill);
  tft.drawLine(x, y + p, x + t, y, edge);
  tft.drawLine(x + t, y, x + t, y + h, edge);
  tft.drawLine(x + t, y + h, x, y + h - p, edge);
  tft.drawLine(x, y + h - p, x, y + p, edge);
}

// disegna UN segmento (bit) di una cifra; col bordo 'edgeOn' quando e' acceso
static void drawSeg7(int x, int y, int w, int h, int t, uint8_t bit, bool on, uint16_t colOn, uint16_t edgeOn) {
  const uint16_t fill = on ? colOn : C_PANEL;
  const uint16_t edge = on ? edgeOn : C_PANEL;
  const int hh = h / 2;
  const int hlen = hh - t;
  switch (bit) {
    case SEG_A: segTrapH(x + t / 2, y, w - t, t, fill, edge); break;
    case SEG_G: segTrapH(x + t / 2, y + hh - t / 2, w - t, t, fill, edge); break;
    case SEG_D: segTrapH(x + t / 2, y + h - t, w - t, t, fill, edge); break;
    case SEG_F: segTrapV(x, y + t / 2, hlen, t, fill, edge); break;
    case SEG_B: segTrapVR(x + w - t, y + t / 2, hlen, t, fill, edge); break;
    case SEG_E: segTrapV(x, y + hh + t / 2, hlen, t, fill, edge); break;
    case SEG_C: segTrapVR(x + w - t, y + hh + t / 2, hlen, t, fill, edge); break;
    case SEG_DOT:
      tft.fillRect(x + 1, y + h - t, t, t, fill);
      tft.drawRect(x + 1, y + h - t, t, t, edge);
      break;
  }
}

// cifra singola "piena" (senza bordo) per il secondo decimale della velocita'
static void drawDigitSeg(int x, int y, int w, int h, int t, char c, uint16_t col) {
  const uint8_t mask = segMask(c);
  for (int b = 0; b < 7; b++) {
    const uint8_t bit = (uint8_t)(1 << b);
    drawSeg7(x, y, w, h, t, bit, (mask & bit) != 0, col, col);
  }
}

#if HERO_RENDER_SEG

static void drawHeroValue(const char *s, bool live) {
  int n = (int)strlen(s);
  if (n > 8) n = 8;                      // gli array di cache sono da 8 posizioni

  // Layout FISSO a celle (come un vero display): "88.8" = 3 celle-cifra + 1 punto.
  // Le celle non usate restano visibili come sagoma spenta, cosi' le posizioni
  // non cambiano quando il numero e' piu' corto (es. "5.2" invece di "38.2").
  int cells = 4;
  if (n > cells) cells = n;              // caso raro: "100.0"
  const int pad = cells - n;             // celle vuote a sinistra (allineato a destra)

  float uRef = (float)(cells - 1) + 0.30f + 0.06f * (cells - 1);   // 4 celle -> 3.48
  if (uRef != heroSegUnits) { heroSegLayout(uRef); heroSegUnits = uRef; heroSegLen = -1; }
  if (cells != heroSegLen || live != heroSegLive) { heroClear(); heroSegLen = cells; heroSegLive = live; }

  const uint16_t col = live ? C_FG : C_DIM;
  int total = 0;
  for (int i = 0; i < cells; i++) {
    const char c = (i < pad) ? ' ' : s[i - pad];
    total += (c == '.') ? (int)(segW * 0.30f) : segW;
    if (i) total += segGap;
  }
  int x = heroX + (heroW - total) / 2;
  for (int i = 0; i < cells; i++) {
    const char c = (i < pad) ? ' ' : s[i - pad];
    const int cw = (c == '.') ? (int)(segW * 0.30f) : segW;
    const uint8_t mask = segMask(c);            // ' ' -> 0 (tutti spenti)
    if (!heroSegInit[i] || mask != heroSegDrawn[i]) {
      const uint8_t diff = heroSegInit[i] ? (uint8_t)(mask ^ heroSegDrawn[i]) : 0xFF;
      for (int b = 0; b < 8; b++) {
        const uint8_t bit = (uint8_t)(1 << b);
        if (diff & bit) drawSeg7(x, segY, cw, segH, segT, bit, (mask & bit) != 0, col, C_SEG_EDGE);
      }
      heroSegDrawn[i] = mask;
      heroSegInit[i] = true;
    }
    x += cw + segGap;
  }
}

#endif  // HERO_RENDER_SEG

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
static void drawBatteryIcon(int x, int y, int pct, uint16_t bg) {
  const int w = 26, h = 13;
  tft.drawRect(x, y, w, h, C_DIM);
  tft.fillRect(x + w, y + 4, 2, h - 8, C_DIM);       // tappo
  int filled = (pct + 12) / 25;                       // 0..4 segmenti
  if (filled > 4) filled = 4;
  if (filled < 0) filled = 0;
  uint16_t col = (pct < 20) ? C_RED : (pct < 40 ? C_YELLOW : C_BATT);
  for (int i = 0; i < 4; i++)
    tft.fillRect(x + 2 + i * 6, y + 2, 4, h - 4, (i < filled) ? col : bg);
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

  const int iy = 7;
  const uint16_t hbg = (page == P_RIDE) ? C_PANEL : C_BG;   // la RIDE non ha la banda header
  if (page == P_RIDE) {
    // RIDE: GPS + BLE + cuore + batteria giustificati a SINISTRA nel pannello velocita'
    const int gap = 6;
    const int gx = heroX + 6;                        // GPS (testo)
    const int gpsW = tft.textWidth("GPS", 2);
    const int bx = gx + gpsW + gap;                  // BLE
    const int hx = bx + 12 + gap;                    // cuore
    const int battX = hx + 12 + gap;                 // batteria
    tft.fillRect(gx - 3, iy - 3, (battX + 28) - gx + 6, 18, hbg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(gpsOn ? C_BLUE : C_DIM, hbg);
    tft.drawString("GPS", gx, iy, 2);
    drawBleIcon(bx, iy, bleOn ? C_BLUE : C_DIM);
    drawHeartIcon(hx, iy, hrLive() ? C_GREEN : (hrAcquiring() ? C_RED : C_DIM));
    drawBatteryIcon(battX, heroY + 6, pct, hbg);
  } else {
    tft.fillRect(4, iy, 62, 12, hbg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(gpsOn ? C_BLUE : C_DIM, hbg);
    tft.drawString("GPS", 4, iy, 2);
    drawBleIcon(34, iy, bleOn ? C_BLUE : C_DIM);
    drawHeartIcon(54, iy, hrLive() ? C_GREEN : (hrAcquiring() ? C_RED : C_DIM));
    drawBatteryIcon(W - 36, (HDR_H - 13) / 2, pct, hbg);
  }

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
static const char *const RIDE_LABELS[3]  = {"TEMPO", "CADENZA", "WATT"};
static const char *const DIAG_LABELS[8]  = {"BLE", "CARDIO bpm", "SATELLITI", "BATTERIA", "QUOTA m", "PENDENZA %", "HEAP", "UPTIME"};

// ---------------------------------------------------------------- pagina RIDE
static void updateCell(int slot, int x, int y, int w, int h, const char *value, uint16_t col) {
  if (strcmp(lastVals[slot], value) == 0 && lastValsCol[slot] == col) return;
  drawFitted(x + 8, y + h - 16, w, value, col, C_PANEL);
  strncpy(lastVals[slot], value, sizeof(lastVals[slot]) - 1);
  lastVals[slot][sizeof(lastVals[slot]) - 1] = 0;
  lastValsCol[slot] = col;
}

// cella del battito (slot cache 3): etichetta in alto centrata, numero grande centrato
static void layoutBpmCell() {
  tft.fillRoundRect(bpmX, bpmY, bpmW, bpmH, 6, C_PANEL);
  tft.drawRoundRect(bpmX, bpmY, bpmW, bpmH, 6, C_BORDER);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.loadFont(VLW_LABEL);
  tft.drawString("BPM", bpmX + bpmW / 2, bpmY + 4);
  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);
}

static void updateBpmValue(const char *v, uint16_t col) {
  if (strcmp(lastVals[3], v) == 0 && lastValsCol[3] == col) return;
  if (sprOk) {
    sprBpm.fillSprite(C_PANEL);
    sprBpm.setTextDatum(MC_DATUM);
    sprBpm.setTextColor(col, C_PANEL);
    sprBpm.loadFont(VLW_BIG);
    sprBpm.drawString(v, bpmW / 2, bpmH / 2 - 22);
    sprBpm.unloadFont();
    sprBpm.pushSprite(bpmX, bpmY + 28);
  } else {
    tft.fillRect(bpmX + 4, bpmY + 28, bpmW - 8, bpmH - 32, C_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_PANEL);
    tft.loadFont(VLW_BIG);
    tft.drawString(v, bpmX + bpmW / 2, bpmY + bpmH / 2 + 6);
    tft.unloadFont();
    tft.setTextDatum(TL_DATUM);
  }
  strncpy(lastVals[3], v, sizeof(lastVals[3]) - 1);
  lastVals[3][sizeof(lastVals[3]) - 1] = 0;
  lastValsCol[3] = col;
}

static void layoutRide() {
  // pannello velocita' (tutto per il numero)
  tft.fillRoundRect(heroX, heroY, heroW, heroH, 6, C_PANEL);
  tft.drawRoundRect(heroX, heroY, heroW, heroH, 6, C_BORDER);
  // battito affiancato alla velocita'
  layoutBpmCell();
  // celle sotto, allineate ai confini dei pannelli sopra
  for (int i = 0; i < 3; i++) {
    tft.fillRoundRect(pcX[i], pcY, pcW[i], pcH, 6, C_PANEL);
    tft.drawRoundRect(pcX[i], pcY, pcW[i], pcH, 6, C_BORDER);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_DIM, C_PANEL);
    tft.loadFont(VLW_LABEL);                            // 16 px antialiased
    tft.drawString(RIDE_LABELS[i], pcX[i] + pcW[i] / 2, pcY + 4);
    tft.unloadFont();
    tft.setTextDatum(TL_DATUM);
  }
}

// valore di una cella della riga RIDE: centrato, come i pannelli sopra
static void updateProCell(int i, const char *val, uint16_t col) {
  if (strcmp(lastVals[i], val) == 0 && lastValsCol[i] == col) return;
  if (sprOk) {
    sprCell[i]->fillSprite(C_PANEL);
    sprCell[i]->setTextDatum(BC_DATUM);
    sprCell[i]->setTextColor(col, C_PANEL);
    sprCell[i]->loadFont(VLW_VALUE);
    sprCell[i]->drawString(val, pcW[i] / 2, (pcH - 26) - 4);
    sprCell[i]->unloadFont();
    sprCell[i]->pushSprite(pcX[i], pcY + 26);
  } else {
    tft.fillRect(pcX[i] + 4, pcY + 26, pcW[i] - 8, pcH - 30, C_PANEL);
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(col, C_PANEL);
    tft.loadFont(VLW_VALUE);
    tft.drawString(val, pcX[i] + pcW[i] / 2, pcY + pcH - 6);
    tft.unloadFont();
    tft.setTextDatum(TL_DATUM);
  }
  strncpy(lastVals[i], val, sizeof(lastVals[i]) - 1);
  lastVals[i][sizeof(lastVals[i]) - 1] = 0;
  lastValsCol[i] = col;
}

static void updateRide() {
  char v[24];
  const float spd = simEnabled ? simSpd : tel.spd;
  bool live = simEnabled || gpsLive() || hrLive() || tel.lastRx != 0;

  if (live) snprintf(v, sizeof(v), "%05.2f", spd);   // larghezza fissa: 00.00 invece di 0.00
  else      snprintf(v, sizeof(v), "--.--");
  if (strcmp(v, lastHeroVal) != 0 || live != lastHeroLive) {
#if HERO_RENDER_SEG
    drawHeroValue(v, live);
#else
    // numero (2 decimali) in font VLW 48 px, su sprite -> UNA passata, niente flicker
    const int cy = heroY + heroH / 2 + 6;                 // stessa riga del BPM
    const uint16_t hcol = live ? C_FG : C_DIM;
    if (heroSpriteOk) {
      heroSprite.fillSprite(C_PANEL);
      heroSprite.setTextDatum(MC_DATUM);
      heroSprite.setTextColor(hcol, C_PANEL);
      // 52 px per "38.24"; se il numero ha 6 cifre (>= 100.00) si ripiega a 42 px
      const uint8_t *hf = (strlen(v) > 5) ? VLW_BIG : VLW_SPEED;
      heroSprite.loadFont(hf);
      heroSprite.drawString(v, (heroW - 8) / 2, 39);   // 5 px piu' in su
      heroSprite.unloadFont();
      heroSprite.pushSprite(heroX + 4, cy - 39);
      lastHeaderState = -1;      // lo sprite copre le icone: ridisegnale subito
      updateHeader();
    } else {
      tft.fillRect(heroX + 4, cy - 39, heroW - 8, 78, C_PANEL);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(hcol, C_PANEL);
      tft.loadFont((strlen(v) > 5) ? VLW_BIG : VLW_SPEED);
      tft.drawString(v, heroX + heroW / 2, cy);
      tft.unloadFont();
      tft.setTextDatum(TL_DATUM);
    }
    tft.setTextColor(C_DIM, C_PANEL);
#endif
    strncpy(lastHeroVal, v, sizeof(lastHeroVal) - 1);
    lastHeroVal[sizeof(lastHeroVal) - 1] = 0;
    lastHeroLive = live;
  }

  // battito, affiancato alla velocita' (colore = zona cardiaca)
  {
    char bs[24];
    if (hrLive()) snprintf(bs, sizeof(bs), "%d", tel.hr); else strcpy(bs, "--");
    updateBpmValue(bs, zoneColor(hrLive() ? hrZone(tel.hr) : -1));
  }

  // tre celle sotto: tempo, cadenza, potenza
  uint16_t cols[3];
  static char t[24], c[24], p[24];
  if (tripActive || tripMs > 0) {
    const uint32_t ts = tripMs / 1000;
    snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(ts / 3600), (unsigned long)((ts % 3600) / 60));
  } else {
    strcpy(t, "--");
  }
  if (simEnabled) snprintf(c, sizeof(c), "%d", (int)simCad);
  else if (tel.cad > 0) snprintf(c, sizeof(c), "%d", tel.cad); else strcpy(c, "--");
  if (simEnabled) snprintf(p, sizeof(p), "%d", (int)simPw);
  else if (tel.pw  > 0) snprintf(p, sizeof(p), "%d", tel.pw);  else strcpy(p, "--");
  cols[0] = !tripActive ? C_DIM : (((simEnabled ? simSpd : tel.spd) >= TRIP_MIN_SPEED) ? C_GREEN : C_YELLOW);
  cols[1] = (simEnabled || tel.cad > 0) ? C_FG : C_DIM;
  cols[2] = (simEnabled || tel.pw  > 0) ? C_FG : C_DIM;
  updateProCell(0, t, cols[0]);
  updateProCell(1, c, cols[1]);
  updateProCell(2, p, cols[2]);
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
// --- barra deviazione: pannello + banda disegnati UNA volta, poi si muove solo il cursore
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
      layoutRide();
      updateHeader();      // le icone di stato stanno dentro i pannelli
      updateRide();
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
  prefs.end();
#if SERIAL_DEBUG
  Serial.printf("NVS salvata: limite=%d\n", zoneLim[setupField]);
#endif
}

static void loadSettings() {
  prefs.begin("bikegps", true);
  if (prefs.isKey("zoneLim")) prefs.getBytes("zoneLim", zoneLim, sizeof(zoneLim));
  prefs.end();
  page = P_RIDE;                 // si parte sempre dalla pagina RIDE
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
struct Button { uint8_t pin; bool last; uint32_t tDown; bool longFired; bool tripFired; };
static Button btn1 = {BTN_SET,  HIGH, 0, false, false};
static Button btn2 = {BTN_PAGE, HIGH, 0, false, false};

static void setBacklight(bool on) {
  backlightOn = on;
#if LP_ENABLE
  ledcWrite(TFT_BL, on ? blDuty : 0);   // PWM: la luminosita' e' regolabile
#else
  digitalWrite(TFT_BL, on ? TFT_BACKLIGHT_ON : !TFT_BACKLIGHT_ON);
#endif
#if SERIAL_DEBUG
  Serial.printf("backlight %s\n", on ? "ON" : "OFF");
#endif
}

// ---------------------------------------------------------------- spegnimento
// La scheda non ha un load switch/PMIC: non esiste un comando che taglia la
// batteria. Il "power off" e' quindi un deep sleep (decine di uA): display,
// pannello e radio spenti, batteria che resta collegata e continua a caricarsi
// se c'e' l'USB. Si riaccende premendo uno dei due tasti (EXT1, attivo basso).
static void powerOff() {
#if SERIAL_DEBUG
  Serial.println("power off: deep sleep (premi un tasto per riaccendere)");
#endif
  saveSettings();                  // il task nvs non girera' piu'

  // display: display off + sleep, poi via l'alimentazione del pannello
  tft.writecommand(0x28);          // DISPOFF
  tft.writecommand(0x10);          // SLPIN
  setBacklight(false);
#if LP_ENABLE
  ledcDetach(TFT_BL);              // il PWM non sopravvive al deep sleep
#endif
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, !TFT_BACKLIGHT_ON);
  digitalWrite(PIN_LCD_POWER, LOW);

  NimBLEDevice::deinit(true);      // radio spenta

  // mantiene bassi retroilluminazione e alimentazione pannello mentre dorme
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_hold_en((gpio_num_t)PIN_LCD_POWER);
  gpio_deep_sleep_hold_en();

  // risveglio: uno qualunque dei due tasti (pull-up RTC mantenuti accesi)
  rtc_gpio_pullup_en((gpio_num_t)BTN_SET);
  rtc_gpio_pulldown_dis((gpio_num_t)BTN_SET);
  rtc_gpio_pullup_en((gpio_num_t)BTN_PAGE);
  rtc_gpio_pulldown_dis((gpio_num_t)BTN_PAGE);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_SET) | (1ULL << BTN_PAGE),
                               ESP_EXT1_WAKEUP_ANY_LOW);
#if SERIAL_DEBUG
  Serial.flush();
#endif
  esp_deep_sleep_start();          // da qui non si torna
}

// stato del risparmio energia (usato dal comando seriale 's' e nei log di avvio)
static int wifiModeForLog() {
#if LP_WIFI_OFF
  return (int)WiFi.getMode();   // 0 = WIFI_OFF
#else
  return -1;
#endif
}

static void printStatus() {
#if SERIAL_DEBUG
  Serial.printf("stato: backlightOn=%d sim=%d trip=%d t=%lus pinBL(%d)=%d lcdPower(%d)=%d heap=%u page=%d/%d portrait=%d up=%lus\n",
                backlightOn, (int)simEnabled, (int)tripActive, (unsigned long)(tripMs / 1000),
                TFT_BL, digitalRead(TFT_BL), PIN_LCD_POWER, digitalRead(PIN_LCD_POWER),
                (unsigned)ESP.getFreeHeap(), page + 1, pageCount, (int)portrait, (unsigned long)(millis() / 1000));
  Serial.printf("power: cpu=%uMHz apb=%uMHz wifi=%d adv=%ums bl=%d\n",
                (unsigned)getCpuFrequencyMhz(), (unsigned)(getApbFrequency() / 1000000),
                wifiModeForLog(), LP_ADV_MS, blDuty);
  Serial.printf("batteria: %.3f V (%d%%)\n", battFiltered, battPercent());
  Serial.printf("tasti: SET(btn1,%d)=%d PAGE(btn2,%d)=%d | soglie: %d %d %d %d | campo=%d\n",
                PIN_BTN1, digitalRead(PIN_BTN1), PIN_BTN2, digitalRead(PIN_BTN2),
                zoneLim[0], zoneLim[1], zoneLim[2], zoneLim[3], setupField);
#endif
}

// --- tasto SET (sinistro): pressione corta
static void onSetShort() {
  bool changed = false;
  switch (page) {
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
    Serial.printf("set: zoneLim=%d,%d,%d,%d\n",
                  zoneLim[0], zoneLim[1], zoneLim[2], zoneLim[3]);
#endif
    markSettingsDirty();
    invalidateCache();
    drawFull();
  }
}

// --- tasto SET (sinistro): pressione lunga
static void onSetLong() {
  switch (page) {
    case P_SETUP:
      zoneLim[setupField]--;
      if (zoneLim[setupField] < 60) zoneLim[setupField] = 60;
      for (int i = N_ZLIM - 1; i > 0; i--) if (zoneLim[i - 1] > zoneLim[i] - 1) zoneLim[i - 1] = zoneLim[i] - 1;
      markSettingsDirty();
      invalidateCache();
      drawFull();
      break;
    default:
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
    btn1.tripFired = false;
    btn2.last = digitalRead(btn2.pin);
    btn2.longFired = false;
    btn2.tripFired = false;
    return;
  }

  bool s1 = digitalRead(btn1.pin);
  bool s2 = digitalRead(btn2.pin);

  // qualsiasi tasto riporta la retroilluminazione al 100% e riavvia il ciclo
  if (s1 == LOW || s2 == LOW) blIdleAt = millis();

  // --- entrambi i tasti premuti per >1 s: apre/chiude la pagina SOGLIE CARDIO
  static uint32_t bothSince = 0;
  static bool bothFired = false;
  if (s1 == LOW && s2 == LOW) {
    if (bothSince == 0) bothSince = millis();
    // entrambi i tasti tenuti per >3 s: spegnimento (deep sleep)
    if (millis() - bothSince > 3000) powerOff();
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
    btn1.tripFired = false;
  }
  if (n1 == LOW && !btn1.longFired && millis() - btn1.tDown > 800) {
    btn1.longFired = true;
    onSetLong();
  }
  if (n1 == LOW && page == P_RIDE && !btn1.tripFired && millis() - btn1.tDown > 3000) {
    btn1.tripFired = true; tripStart();      // START trip (RIDE)
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
    btn2.tripFired = false;
  }
  if (n2 == LOW && !btn2.longFired && millis() - btn2.tDown > 800) {
    btn2.longFired = true;
    onPageLong();
  }
  if (n2 == LOW && page == P_RIDE && !btn2.tripFired && millis() - btn2.tDown > 3000) {
    btn2.tripFired = true; tripStop();       // STOP trip (RIDE)
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

#if LP_ENABLE
  // CPU: va impostata subito, prima di inizializzare BLE e pannello.
  // 80 MHz e' il minimo sicuro con il controller BLE. Il bus TFT e' in
  // bit-banging, quindi ne risente: misurato a 80 MHz fillScreen 38,6 ms
  // (era 27,5 a 240), cambio pagina RIDE 127,8 ms (era 68,8).
  setCpuFrequencyMhz(LP_CPU_MHZ);
#if SERIAL_DEBUG
  Serial.printf("power: CPU %u MHz, APB %u MHz\n",
                (unsigned)getCpuFrequencyMhz(), (unsigned)(getApbFrequency() / 1000000));
#endif
#endif

#if LP_WIFI_OFF
  // BikeGPS non usa il WiFi: spegnimento esplicito (driver init + stop).
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
#if SERIAL_DEBUG
  Serial.printf("power: WiFi %s (mode=%d)\n",
                WiFi.getMode() == WIFI_OFF ? "spento" : "ATTIVO", (int)WiFi.getMode());
#endif
#endif

  // al risveglio dal deep sleep retroilluminazione e pannello erano "held":
  // rilasciarli, altrimenti gli schermi restano spenti.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)TFT_BL);
  gpio_hold_dis((gpio_num_t)PIN_LCD_POWER);
#if SERIAL_DEBUG
  {
    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    if (wc != ESP_SLEEP_WAKEUP_UNDEFINED)
      Serial.printf("wake da deep sleep: cause=%d (%s)\n", (int)wc,
                    wc == ESP_SLEEP_WAKEUP_EXT1 ? "tasti" : "altra");
  }
#endif

  // alimentazione pannello: senza questo lo schermo resta nero
  pinMode(PIN_LCD_POWER, OUTPUT);
  digitalWrite(PIN_LCD_POWER, HIGH);

  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  btn1.last = digitalRead(btn1.pin);
  btn2.last = digitalRead(btn2.pin);
  bootTime = millis();
  blIdleAt = millis();         // parte il ciclo della luminosita' adattiva

  // batteria: partitore 1:2, attenuazione massima per arrivare a ~4,2 V
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  tft.init();
  tft.setRotation(ROTATION);
  setupGeometry();

  // tft.init() riconfigura TFT_BL come uscita digitale (TFT_eSPI.cpp): il PWM
  // del backlight va (ri)attivato DOPO l'init, altrimenti ledcWrite non ha effetto.
  Serial.printf("ledcAttach(BL=%d) -> %d  (duty 50%% = %d)\n",
                TFT_BL, (int)ledcAttach(TFT_BL, 5000, 8), (50 * 255) / 100);
  setBacklight(true);
  simEnabled = SIM_DEFAULT_ON;   // simulazione attiva all'avvio (vedi SIM_DEFAULT_ON)

  C_BG     = rgb(0x16, 0x17, 0x20);
  C_PANEL  = rgb(0x1f, 0x23, 0x35);
  C_BORDER = rgb(0x33, 0x3b, 0x57);
  C_FG     = rgb(0xc0, 0xca, 0xf5);
  C_DIM    = rgb(0x56, 0x5f, 0x89);
  C_BLUE   = rgb(0x7a, 0xa2, 0xf7);
  C_GREEN  = rgb(0x9e, 0xce, 0x6a);
  C_BATT   = rgb(0x55, 0x90, 0x3a);   // verde piu' scuro, solo per la batteria
  C_SEG_EDGE = rgb(0x0d, 0x0f, 0x17); // bordo dei segmenti accesi (effetto display)
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
#if TEST_MODE
  blogLoad();
#if SERIAL_DEBUG
  Serial.printf("test durata: log con %u campioni, carico ciclico ogni %us, batteria %u mAh\n",
                blogCount, (unsigned)TEST_PAGE_S, (unsigned)BATT_MAH);
#endif
#endif
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
#if LP_ENABLE && LP_ADV_MS > 0
  // advertising piu' rado: meno radio accesa, stesso servizio
  adv->setMinInterval((uint16_t)(LP_ADV_MS * 8 / 5));   // unita' da 0,625 ms
  adv->setMaxInterval((uint16_t)(LP_ADV_MS * 8 / 5));
#endif
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
static void taskSim() {                     // 4 Hz: grandezze simulate (velocita', cadenza, watt)
  if (!simEnabled) return;
  simSpd += simDir * 0.07f;                 // ~2,8 km/h al secondo, 2 decimali che variano
  if (simSpd >= 45.0f) { simSpd = 45.0f; simDir = -1; }
  if (simSpd <= 0.0f)  { simSpd = 0.0f;  simDir = 1; }

  simCad += simCadDir * 0.9f;               // 55..100 rpm
  if (simCad >= 100.0f) { simCad = 100.0f; simCadDir = -1; }
  if (simCad <= 55.0f)  { simCad = 55.0f;  simCadDir = 1; }

  simPw += simPwDir * 4.0f;                 // 0..380 W
  if (simPw >= 380.0f) { simPw = 380.0f; simPwDir = -1; }
  if (simPw <= 0.0f)   { simPw = 0.0f;   simPwDir = 1; }
}

static void taskDisplay() {                 // 4 Hz: ridisegna solo i valori cambiati (cache)
  drawValues();
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

#if TEST_MODE
// carico ciclico del test durata: alterna RIDE <-> DIAG e tiene la simulazione
// attiva, cosi' il consumo misurato e' quello "in funzione", non a schermo fermo.
static void taskStress() {
  simEnabled = true;
  page = (page == P_RIDE) ? P_DIAG : P_RIDE;
  invalidateCache();
  drawFull();
}

static void taskBattLog() { blogSample(); }   // campionamento batteria in flash
#endif

// risparmio energetico del display: calcola la luminosita' in base al tempo
// dall'ultimo tasto e spegne del tutto il display quando non serve.
static void taskAutoDim() {
  if (!autoDim || !backlightOn) return;
  const uint32_t idle = millis() - blIdleAt;
  uint8_t d;
  if (idle < BL_FULL_MS) {
    d = LP_BL_DUTY;
  } else if (idle < BL_FULL_MS + BL_RAMP_MS) {
    const uint32_t k = idle - BL_FULL_MS;
    d = (uint8_t)(LP_BL_DUTY - (uint32_t)(LP_BL_DUTY - BL_MIN_DUTY) * k / BL_RAMP_MS);
  } else if (BL_OFF_ENABLE && idle >= BL_OFF_AFTER_MS) {
    d = 0;                               // display spento (solo con BL_OFF_ENABLE)
  } else {
    d = BL_MIN_DUTY;                     // resta al 10%
  }
  if (d == 0) {
    if (!blPanelOff) {
      blDuty = 0;
      ledcWrite(TFT_BL, 0);
      tft.writecommand(0x28);            // DISPOFF: pannello spento ma GRAM conservata
      blPanelOff = true;
#if SERIAL_DEBUG
      Serial.println("display: spento (inattivita')");
#endif
    }
  } else {
    if (blPanelOff) {
      tft.writecommand(0x29);            // DISPON: si riaccende
      blPanelOff = false;
      invalidateCache();
      drawFull();
#if SERIAL_DEBUG
      Serial.println("display: riacceso");
#endif
    }
    if (d != blDuty) {
      blDuty = d;
      ledcWrite(TFT_BL, d);
    }
  }
}

// comandi di test da seriale: n = pagina avanti, p = indietro, b = retroilluminazione
//   d = riaccendi + ridisegna, s = stato pin/heap
//   r = schermo rosso pieno (test pannello diretto), v = verde, k = nero
static void taskSerial() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'n') {
      onPageShort();
    } else if (c == 'p') {
      onPagePrev();
    }
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
          heroSegLen = -1;              // caso peggiore: ridisegna tutta la velocita'
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
    else if (c == 'C') {
      // test consumi: cicla la frequenza della CPU (240 -> 160 -> 80 -> 40 MHz).
      // Utile per misurare il consumo a banco senza ricompilare.
      static const uint32_t freqs[] = {240, 160, 80, 40};
      uint32_t cur = getCpuFrequencyMhz();
      uint8_t idx = 0;
      for (uint8_t i = 0; i < 4; i++) if (freqs[i] == cur) { idx = i; break; }
      idx = (idx + 1) % 4;
      bool ok = setCpuFrequencyMhz(freqs[idx]);
      Serial.printf("CPU: %lu -> %lu MHz (ok=%d) APB=%lu MHz%s\n",
                    (unsigned long)cur, (unsigned long)freqs[idx], (int)ok,
                    (unsigned long)(getApbFrequency() / 1000000),
                    (freqs[idx] <= 40) ? "  [40 MHz: BLE instabile!]" : "");
    }
    else if (c == 'y') {
      // test: attiva/disattiva la simulazione della velocita'
      simEnabled = !simEnabled;
      if (simEnabled) { simSpd = 0.0f; simCad = 55.0f; simPw = 0.0f; simDir = 1; simCadDir = 1; simPwDir = 1; }
      lastHeroVal[0] = 0;
      heroSegLen = -1;
      Serial.printf("simulazione velocita': %s\n", simEnabled ? "ON" : "OFF");
    }
#if TEST_MODE
    else if (c == 'l') {
      printDrainLog();                     // curva di scarica + durata stimata
    } else if (c == 'L') {
      blogReset();
      Serial.println("log batteria azzerato");
    }
#endif
    else if (c == 'B') {
      // test: cicla la luminosita' del backlight (per misurare il consumo a banco).
      // Disattiva la luminosita' adattiva: si torna al comportamento automatico con 'A'.
      autoDim = false;
      static const uint8_t lv[] = {255, 200, 160, 128, 96, 64, 32, 0};
      uint8_t idx = 0;
      for (uint8_t i = 0; i < sizeof(lv); i++) if (lv[i] == blDuty) { idx = i; break; }
      blDuty = lv[(idx + 1) % sizeof(lv)];
      setBacklight(backlightOn);
      Serial.printf("backlight duty: %u/255 (%u%%) [auto-dim OFF]\n",
                    blDuty, (unsigned)(blDuty * 100UL / 255));
    } else if (c == 'A') {
      // riattiva la luminosita' adattiva
      autoDim = true;
      blIdleAt = millis();
      Serial.println("luminosita' adattiva: ON");
    }
    else if (c == 'R') {
      // riavvio remoto della scheda
      Serial.println("riavvio...");
      Serial.flush();     // nessun delay: il flush basta
      esp_restart();
    }
    else if (c == 'w') {
      // test dei font VLW: se textWidth e' 0 il glifo non viene trovato
      tft.loadFont(VLW_BIG);
      Serial.printf("VLW_BIG:   '0'=%d '38.24'=%d '--'=%d\n",
                    tft.textWidth("0"), tft.textWidth("38.24"), tft.textWidth("--"));
      tft.unloadFont();
      tft.loadFont(VLW_VALUE);
      Serial.printf("VLW_VALUE: '0'=%d '1:23'=%d\n", tft.textWidth("0"), tft.textWidth("1:23"));
      tft.unloadFont();
      tft.loadFont(VLW_LABEL);
      Serial.printf("VLW_LABEL: 'T'=%d 'TEMPO'=%d\n", tft.textWidth("T"), tft.textWidth("TEMPO"));
      tft.unloadFont();
      // prova visiva diretta: sfondo nero, cifre in bianco, righe separate
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(TFT_WHITE);
      tft.loadFont(VLW_BIG);
      tft.drawString("0123456789", 4, 4);          // una sola riga, per vedere se le cifre si accavallano
      tft.unloadFont();
      tft.loadFont(VLW_VALUE);
      tft.drawString("0123456789", 4, 80);
      tft.unloadFont();
      tft.loadFont(VLW_LABEL);
      tft.drawString("ABCDEFGHIJKLM", 4, 130);
      tft.unloadFont();
      tft.drawFastHLine(0, 118, 320, TFT_RED);      // riga di riferimento
      Serial.println("test: 3 righe separate (grande/medio/piccolo) + riga rossa a y=118");
    }
    else if (c == 'f') {
      // larghezze reali dei font: serve a sapere quali stringhe entrano nel riquadro
      Serial.printf("hero: heroW=%d utile=%d\n", heroW, heroW - 16);
      const char *tests[] = {"25.4", "30.5", "55.5", "99.9", "100.0", "-.-  "};
      for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        Serial.printf("  '%s': f8=%d f7=%d\n", tests[i],
                      tft.textWidth(tests[i], 8), tft.textWidth(tests[i], 7));
      Serial.printf("seg: w=%d h=%d t=%d gap=%d y=%d | box %dx%d @%d,%d | units=%.2f\n",
                    segW, segH, segT, segGap, segY, heroW, heroH, heroX, heroY, heroSegUnits);
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
      ledcAttach(TFT_BL, 5000, 8);      // tft.init() rilascia il PWM del backlight
      setBacklight(true);
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
  {"hr",          5, 0, hrTask},
  {"serial",     20, 0, taskSerial},
  {"display",   500, 0, taskDisplay},
  {"sim",       250, 0, taskSim},
  {"trip",      100, 0, taskTrip},
  {"blink",    1000, 0, taskBlink},
  {"header",   2500, 0, taskHeader},
  {"nvs",      1000, 0, taskSaveSettings},
  {"battery",  1000, 0, taskBattery},
  {"autodim",   100, 0, taskAutoDim},
#if TEST_MODE
  {"stress",  TEST_PAGE_S * 1000, 0, taskStress},
  {"battlog", TEST_LOG_PERIOD_S * 1000, 0, taskBattLog},
#endif
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
