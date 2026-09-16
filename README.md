# BikeGPS — mirror GPS → ESP32 + OLED

App Android (Kotlin + Jetpack Compose) che legge il **GPS nativo del telefono**
e ne fa il **mirror via BLE** su un **ESP32 + OLED SSD1306**. Pensata per la bici:
il telefono sta in tasca, il display sta sul manubrio.

Nessuna dipendenza da Google Play Services: posizione con `android.location`.

## Architettura

```
[Telefono]                                   [ESP32 + SSD1306]
 GPS nativo (GPS_PROVIDER)                     NimBLE peripheral "BikeGPS"
 RideCalculator (dist/media/pendenza)  --BLE-->  riceve CSV
 Foreground service (schermo spento)             mostra velocità + info
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

Premi **AVVIA**: parte il foreground service (notifica "BikeGPS attivo"),
continua anche a schermo spento. **STOP** ferma tutto.

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

Pagine: Velocità · Distanza · Tempo · Media · Max · Quota · Pendenza · Satelliti.
Cambio pagina col pulsante su `BUTTON_PIN` (default **GPIO0 = BOOT integrato**,
attivo basso). Per un pulsante esterno cambia `BUTTON_PIN` nello sketch.
Se il BLE è scollegato il valore diventa `---` e compare `no BLE`.

## Note

- Il `GPS_PROVIDER` richiede cielo aperto; all'interno può non agganciare.
- Se il BLE si scollega, entrambi i lati si riconnettono da soli.
- Se non arrivano dati per 5 s, l'ESP32 azzera la velocità mostrata.
