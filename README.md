# BikeGPS — mirror GPS → ESP32 + OLED

App Android (Kotlin + Jetpack Compose) che legge il **GPS nativo del telefono**
e ne fa il **mirror via BLE** su un **ESP32 + OLED SSD1306**. Pensata per la bici:
il telefono sta in tasca, il display sta sul manubrio.

Nessuna dipendenza da Google Play Services: posizione con `android.location`.

## Architettura

```
[Telefono]                                   [ESP32 + SSD1306]
 GPS nativo (GPS_PROVIDER)                     NimBLE peripheral "BikeGPS"
 RideCalculator (dist/media/pendenza)  --BLE-->  riceve binario
 Cardio BLE 0x180D (bpm)                        mostra velocità + info
 Foreground service (schermo spento)
```

- **Trasporto**: BLE, ESP32 = peripheral, telefono = central.
- **Frequenza**: 1 Hz (più che sufficiente in bici).
- **Autonomia**: nessun problema per 5 h (ESP32+OLED ≪ 100 mA).

## Protocollo BLE

| Elemento | UUID |
|---|---|
| Service | `0000a001-0000-1000-8000-00805f9b34fb` |
| Telemetry (Write Without Response) | `0000a002-0000-1000-8000-00805f9b34fb` |

MTU negoziato a 64. Payload **binario compatto da 15 byte** (little-endian), così entra
anche nel caso peggiore di MTU 23:

| Offset | Tipo | Campo | Unità |
|---|---|---|---|
| 0 | u16 | velocità | 0.1 km/h |
| 2 | u32 | distanza | 0.01 km |
| 6 | u16 | tempo in movimento | s |
| 8 | i16 | quota | m |
| 10 | i8 | pendenza | 0.5 % |
| 11 | u8 | satelliti | — |
| 12 | u16 | velocità max | 0.1 km/h |
| 14 | u8 | battito | bpm (0 = nessun cardio) |

## App Android

Pacchetti:
- `location/GpsTracker.kt` — GPS nativo (`LocationManager` + `GnssStatus`)
- `ride/RideStats.kt` — distanza (Haversine), tempo in movimento, media/max, pendenza, payload
- `ride/RideState.kt` — stato condiviso service ↔ UI (`StateFlow`)
- `ride/HeartRate.kt` — stato del cardio (bpm, contatto)
- `ble/BleMirror.kt` — central BLE con riconnessione automatica
- `ble/HeartRateMonitor.kt` — central BLE per cardio standard (Heart Rate Service)
- `service/RideService.kt` — foreground service tipo `location`
- `MainActivity.kt` — UI + permessi

Permessi richiesti: posizione, Bluetooth (Scan/Connect), notifiche.

### Build e installazione

```bash
export ANDROID_HOME=/home/flavio/Android/Sdk
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Premi **AVVIA**: parte il foreground service (notifica "BikeGPS attivo"),
continua anche a schermo spento. **STOP** ferma tutto.

## Cardio BLE (opzionale)

L'app legge anche un **cardiofrequenzimetro BLE standard** (Heart Rate Service
`0x180D`, characteristic `0x2A37`): Geonaute/Decathlon, Polar, Garmin, Wahoo, ecc.

- Basta accendere il cardio e indossarlo: l'app lo cerca e si connette da sola,
  riceve il battito via notifiche BLE e lo mostra a schermo.
- Il battito viene incluso nel payload verso l'ESP32 (pagina **Battito**).
- È indipendente sia dal GPS sia dall'ESP32: se il cardio non c'è, l'app funziona
  lo stesso.
- Permessi: usa gli stessi permessi Bluetooth del mirror, quindi vanno concessi
  (card *"BLE non attivo (opzionale)"*).
- Se la cintura si connette (stato *pronto*) ma il battito resta `--` e la
  connessione cade dopo pochi secondi, il sospetto numero uno è la **batteria
  scarica** (CR2032): con pila quasi esaurita la radio smette di rispondere
  (supervision timeout) pur riuscendo ad annunciarsi e connettersi.
- Il mirror è indipendente: eventuali problemi della cintura non fermano GPS o ESP32.

## ESP32 (Arduino)

Sketch: `~/Arduino/BikeGPS_Mirror/BikeGPS_Mirror.ino`
Librerie: `NimBLE-Arduino` (≥2.x), `Adafruit_SSD1306`, `Adafruit_GFX`.

Collegamenti OLED (ESP32 classico):
```
OLED VCC -> 3V3
OLED GND -> GND
OLED SCL -> GPIO22
OLED SDA -> GPIO21
```

Compila e carica:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 ~/Arduino/BikeGPS_Mirror
arduino-cli upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 ~/Arduino/BikeGPS_Mirror
```

L'OLED (SSD1306 bicolore) mostra **un dato alla volta**, grande e centrato
nell'area blu; l'etichetta+unità e l'indice pagina stanno nella banda gialla:

```
┌─ gialla (0..15) ─────────┐
│ VELOCITA' km/h      1/8  │
├─ blu (16..63) ───────────┤
│                          │
│          27.4            │
│                          │
└──────────────────────────┘
```

Pagine: Velocità · Distanza · Tempo · Media · Max · Quota · Pendenza · Satelliti · Battito.
Cambio pagina col pulsante su `BUTTON_PIN` (default **GPIO0 = BOOT integrato**,
attivo basso). Per un pulsante esterno cambia `BUTTON_PIN` nello sketch.
Se il BLE è scollegato il valore diventa `---` e compare `no BLE`.

## Display / hardware

Lo sketch attuale usa un OLED **SSD1306 I2C 128x64**. In `docs/display-notes.md`
ci sono pin, setup TFT_eSPI e il piano per passare a **LILYGO T-Display (1.14")**
o **T-Display-S3 (1.9")** senza toccare il codice BLE.

## Note

- Il `GPS_PROVIDER` richiede cielo aperto; all'interno può non agganciare.
- Se il BLE si scollega, entrambi i lati si riconnettono da soli.
- Se non arrivano dati per 5 s, l'ESP32 azzera la velocità mostrata.
