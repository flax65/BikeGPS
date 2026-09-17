# BikeGPS — mirror GPS → ESP32 + OLED

App Android (Kotlin + Jetpack Compose) che legge il **GPS nativo del telefono**
e ne fa il **mirror via BLE** su un **ESP32 + OLED SSD1306**. Pensata per la bici:
il telefono sta in tasca, il display sta sul manubrio.

Nessuna dipendenza da Google Play Services: posizione con `android.location`.

## Architettura

```
[Telefono]                                   [ESP32 + SSD1306]
 GPS nativo (GPS_PROVIDER)                     NimBLE peripheral "BikeGPS"
 RideCalculator (dist/media/pendenza)  --BLE-->  riceve binario (14 B)
 Foreground service (schermo spento)             mostra velocità + info
                                               NimBLE central --BLE--> cintura cardio
                                                          (Heart Rate 0x180D)
```

- **Trasporto**: BLE, ESP32 = peripheral, telefono = central.
- **Frequenza**: 1 Hz (più che sufficiente in bici).
- **Autonomia**: nessun problema per 5 h (ESP32+OLED ≪ 100 mA).

## Protocollo BLE

| Elemento | UUID |
|---|---|
| Service | `0000a001-0000-1000-8000-00805f9b34fb` |
| Telemetry (Write Without Response) | `0000a002-0000-1000-8000-00805f9b34fb` |

MTU negoziato a 64. Payload **binario compatto da 14 byte** (little-endian), così entra
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

## App Android

Pacchetti:
- `location/GpsTracker.kt` — GPS nativo (`LocationManager` + `GnssStatus`)
- `ride/RideStats.kt` — distanza (Haversine), tempo in movimento, media/max, pendenza, payload
- `ride/RideState.kt` — stato condiviso service ↔ UI (`StateFlow`)
- `ble/BleMirror.kt` — central BLE con riconnessione automatica
- `service/RideService.kt` — foreground service tipo `location`
- `MainActivity.kt` — UI + permessi

Permessi richiesti: posizione, Bluetooth (Scan/Connect), notifiche.

### Build e installazione

```bash
export ANDROID_HOME=/home/flavio/Android/Sdk
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

L'app **parte da sola** all'apertura (serve solo il permesso posizione) e si
collega all'ESP32: il telefono fa da unità GPS. **STOP** ferma tutto.

Per farla partire **anche all'accensione del telefono**, concedi la posizione
"Consenti sempre" (Android 14+). L'app mostra una card con il collegamento alle
impostazioni. Senza quel permesso l'avvio automatico funziona solo aprendo l'app.

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
│ VELOCITA' km/h      1/9  │
├─ blu (16..63) ───────────┤
│                          │
│          27.4            │
│                          │
└──────────────────────────┘
```

Pagine: Velocità · Distanza · Tempo · Media · Max · Quota · Pendenza · Satelliti · Battito.
Cambio pagina col pulsante su `BUTTON_PIN` (default **GPIO0 = BOOT integrato**,
attivo basso). Per un pulsante esterno cambia `BUTTON_PIN` nello sketch.
Se il BLE è scollegato il valore diventa `---` e compare `no BLE`
(sulla pagina Battito compare `no HR` se la cintura non è connessa).

## Cardio (letto dall'ESP32)

L'ESP32 fa anche da **central BLE** verso un cardiofrequenzimetro standard
(Heart Rate Service `0x180D`, characteristic `0x2A37`): Geonaute/Decathlon, Polar,
Garmin, Wahoo, ecc.

Perché qui e non sul telefono: alcune cinture (Geonaute) pretendono un intervallo
di connessione lungo (~1 s) che la cintura stessa richiede. Android lo impone a
~45-100 ms e la cintura smette di trasmettere, mentre l'ESP32 può impostarlo
(`setConnectionParams`/`updateConnParams`) e quindi funziona in modo affidabile.

- Connessione: appena acceso l'ESP32 cerca la cintura e si connette (1-3 s); se
  cade, si riconnette da solo.
- Il battito è **locale**: pagina **Battito** sull'OLED (bpm + contatto).
- Il telefono **non** legge il cardio; invia solo GPS e riceve/calcola le statistiche
  di uscita.

## Display / hardware

Lo sketch attuale usa un OLED **SSD1306 I2C 128x64**. In `docs/display-notes.md`
ci sono pin, setup TFT_eSPI e il piano per passare a **LILYGO T-Display (1.14")**
o **T-Display-S3 (1.9")** senza toccare il codice BLE.

## Note

- Il `GPS_PROVIDER` richiede cielo aperto; all'interno può non agganciare.
- Se il BLE si scollega, entrambi i lati si riconnettono da soli.
- Se non arrivano dati per 5 s, l'ESP32 azzera la velocità mostrata.
