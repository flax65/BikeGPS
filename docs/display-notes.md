# Note hardware e display

Appunti per quando si cambia display. Il lato BLE/payload dell'app **non cambia**
con nessuna di queste schede: cambia solo la parte di disegno nello sketch.

## Confronto display

| | SSD1306 (attuale) | LILYGO T-Display | LILYGO T-Display-S3 |
|---|---|---|---|
| Tecnologia | OLED mono | IPS colore | IPS colore |
| Diagonale | 0.96" | 1.14" | 1.9" |
| Risoluzione | 128×64 (8.192 px) | 135×240 | 170×320 (54.400 px) |
| Area attiva | ~21.7×11.2 mm | ~25×14 mm | ~42.6×22.6 mm (landscape) |
| MCU | (ESP32 esterno) | ESP32 classico dual-core | ESP32-S3 dual-core |
| BLE | — | BLE 4.2 (NimBLE ok) | BLE 5 (NimBLE ok) |
| USB | CH340 | CH9102F | nativa USB-C |
| Batteria | — | JST + ADC GPIO34 | JST + ADC GPIO4 |

## Sketch attuale: SSD1306 (I2C)

- `esp32/BikeGPS_Mirror/BikeGPS_Mirror.ino`
- OLED 128×64, banda gialla in alto (0..15), area blu (16..63).
- Una pagina per dato, valore grande centrato nell'area blu.
- Pulsante cambio pagina: `BUTTON_PIN 0` (classico) / `9` (C3), attivo basso.
- Librerie: `Adafruit_SSD1306`, `Adafruit_GFX`, `NimBLE-Arduino`.

## LILYGO T-Display (1.14", ESP32 classico)

- Display: **SPI** ST7789, 135×240, `CGRAM_OFFSET`.
- TFT_eSPI: attivare `#include <User_Setups/Setup25_TTGO_T_Display.h>` in
  `TFT_eSPI/User_Setup_Select.h`.
- Pin display: `MOSI=19, SCLK=18, CS=5, DC=16, RST=23, BL=4` (SPI 40 MHz).
- Pulsanti: `LEFT_BUTTON=0`, `RIGHT_BUTTON=35`.
- Batteria: `BAT_VOLT=34`.
- FQBN: `esp32:esp32:lilygo_t_display`.

## LILYGO T-Display-S3 (1.9", ESP32-S3)

- Display: **8-bit parallelo** ST7789, 170×320, `TFT_INVERSION_ON`, `INIT_SEQUENCE_3`.
- TFT_eSPI: attivare `#include <User_Setups/Setup206_LilyGo_T_Display_S3.h>`.
- Pin display (parallelo):
  `CS=6, DC=7, RST=5, WR=8, RD=9, D0..D7=39,40,41,42,45,46,47,48, BL=38`.
- **Importante**: `LCD_POWER_ON = GPIO15` va messo HIGH, altrimenti lo schermo
  resta nero (oltre al backlight GPIO38).
- Pulsanti: `BUTTON_1=0`, `BUTTON_2=14`.
- Batteria: `BAT_VOLT=4`.
- I2C libero: `SDA=18, SCL=17`.
- Pin esposti P1: 43,44,18,17,21,16 · P2: 1,2,3,10,11,12,13.
- USB nativa: appare come `/dev/ttyACM0` (a volte serve tenere premuto BOOT al collegamento).
- FQBN: `esp32:esp32:lilygo_t_display_s3`.

## Sketch T-Display-S3 — FATTO

Stato al 2026-09-18: **T-Display-S3 arrivato e funzionante**.

- Hardware rilevato: `esptool` → **ESP32-S3** (QFN56, rev v0.2, 8 MB PSRAM,
  USB-Serial/JTAG, MAC `a0:f2:62:e9:0d:14`) → è il T-Display-S3 da 1.9".
- Porta seriale: `/dev/ttyACM0`.
- Sketch: `esp32/BikeGPS_TDisplayS3/BikeGPS_TDisplayS3.ino` (lo sketch SSD1306
  `esp32/BikeGPS_Mirror/` è rimasto intatto).

### Setup TFT_eSPI — senza toccare la libreria

Nella cartella dello sketch c'è **`tft_setup.h`** con la configurazione di
`Setup206_LilyGo_T_Display_S3.h`. TFT_eSPI include da solo quel file
(`__has_include(<tft_setup.h>)` in `TFT_eSPI.h`), quindi
`~/Arduino/libraries/TFT_eSPI/User_Setup.h` **resta quello di sempre
(GC9A01, rotondo)** e gli altri sketch non si rompono.

### Comandi

```bash
cd ~/AndroidStudioProjects/GpsPosition/esp32/BikeGPS_TDisplayS3
arduino-cli compile --upload -p /dev/ttyACM0 --fqbn esp32:esp32:lilygo_t_display_s3 .
```

### Problema risolto: libreria TFT_eSPI incompleta

La compilazione falliva con `fatal error: Fonts/glcdfont.c: No such file or
directory`: nella copia installata mancava **solo** quel file. Recuperato dalla
release ufficiale **V2.5.43** (tag GitHub con la V maiuscola) e copiato in
`~/Arduino/libraries/TFT_eSPI/Fonts/`. `diff -rq` con la release: l'unica altra
differenza è `User_Setup.h` (personalizzato GC9A01, da tenere).

### Layout scelto (5 pagine)

**Verticale (170x320, rotazione 0 — impostazione attuale)**
1. **RIDE**: velocità gigante + griglia 2x2 (DIST, TEMPO, MEDIA, MAX).
2. **COST SPEED**: riga target, barra delle zone, velocità gigante (verde in
   target), scostamento `^/v` con barra centrata, riga info (HR/zona/distanza),
   tempo passato in target.
3. **COST BPM**: come sopra ma il dato gigante è il battito, colorato con la
   zona attiva; l'ospite è la velocità.
4. **SETUP SOGLIE**: 4 righe (limiti Z1|Z2 … Z4|Z5) con selezione evidenziata,
   barra delle zone, legenda comandi.
5. **DIAG**: griglia 2x4 (BLE, cardio, satelliti, batteria, quota, pendenza,
   heap, uptime).

**Orizzontale (320x170, rotazione 1)**: le stesse pagine con le pagine di
allenamento a due colonne (le griglie diventano 4x2).

La geometria è scelta a runtime in `setupGeometry()`/`setGrid()`/`costGeometry()`
in base a `tft.width()/height()`: basta cambiare `ROTATION` (0/2 verticale,
1/3 orizzontale).

### Comandi

| Tasto | Pressione | Azione |
|---|---|---|
| DESTRO (GPIO14) | corta | pagina successiva; in SETUP = campo successivo |
| DESTRO | lunga (>0,8 s) | torna a RIDE |
| SINISTRO (GPIO0) | corta | +1 sul valore (target o soglia) |
| SINISTRO | lunga | pagine allenamento: cattura valore attuale come target; SETUP: -1; RIDE/DIAG: retroilluminazione |

In `COST SPEED` e `COST BPM`: tolleranza ±1 km/h / ±5 bpm, fondoscala barra
deviazione ±5 km/h / ±15 bpm. Soglie, target e ultima pagina in **NVS**
(`Preferences`, namespace `bikegps`): sopravvivono al riavvio.

Colori zone: Z1 grigio, Z2 verde, Z3 giallo, Z4 arancio, Z5 rosso.
Default soglie 102/119/136/153 bpm (FCmax 170).
- Righe di stato in alto: `BikeGPS` + stato BLE (verde/rosso) + batteria %.
- GPIO0 corto = pagina avanti; GPIO0 lungo (>0,8 s) = retroilluminazione;
  GPIO14 = pagina indietro.
- Batteria: `analogReadMilliVolts(GPIO4)` x2 (partitore 1:2), media esponenziale,
  percentuale 3,30-4,20 V.

Nota di sviluppo: niente `enum` come parametro di funzione (l'auto-prototyping di
Arduino mette i prototipi in cima e il tipo non è ancora dichiarato) → usare
`#define`/`uint8_t`.

### Problema risolto: black screen con il doppio buffer (sprite)

**Non usare `TFT_eSprite` a tutto schermo + `pushSprite()` su questo pannello.**
Su questa scheda il `pushSprite` manda il pannello in **black screen dopo pochi
secondi** (il chip resta vivo e la telemetria BLE continua regolarmente, il
backlight e `LCD_POWER_ON` restano corretti, e con `tft.fillScreen()` diretto lo
schermo si colora benissimo: era proprio il `pushSprite`).

Strategia adottata (disegno diretto, niente flicker):

1. **`tft.setTextPadding(w)`** prima di `drawString`: la libreria cancella solo
   l'area del testo invece di tutto il pannello.
2. **Cache dei valori**: si ridisegna un campo solo se la stringa è cambiata
   (`lastHeroVal`, `lastVals[]`, `lastHeader`), non a ogni ciclo da 250 ms.
3. Header statico (`drawHeaderStatic`) separato dalla parte variabile
   (`updateHeader`).

### Comandi di diagnostica seriale (nello sketch)

Da terminale con la seriale aperta a 115200:

| Tasto | Azione |
|---|---|
| `n` / `p` | pagina avanti / indietro |
| `b` | retroilluminazione on/off |
| `s` | stampa stato (pin backlight, LCD power, heap, pagina, uptime) |
| `d` | re-init del pannello + ridisegno della pagina |
| `r` / `v` / `k` | `fillScreen` rosso / verde / nero (test pannello, blocca il refresh) |

Esempio: `arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200` e poi premere i tasti.

---

### Promemoria generali

`LCD_POWER_ON` (GPIO15) va portato HIGH prima di `tft.init()`, altrimenti lo
schermo resta nero.

---

## Piano storico sketch T-Display / T-Display-S3 (superato)

1. Nuovo sketch separato `BikeGPS_TDisplay/` (o `BikeGPS_TDisplayS3/`) **senza
   toccare** quello SSD1306.
2. Stesso codice BLE + parsing binario (14 byte), cambia solo il disegno.
3. Layout landscape (320×170 o 240×135): velocità grande + 2-3 info
   (distanza/tempo), batteria e satelliti.
4. Pagine con i pulsanti integrati (GPIO0/GPIO35 sul T-Display, GPIO0/GPIO14 sul S3).
5. Dim/spegnimento backlight (GPIO4 sul T-Display, GPIO38 sul S3) + `LCD_POWER_ON` (GPIO15) sul S3.
6. Lettura batteria (GPIO34 sul T-Display, GPIO4 sul S3) → percentuale a schermo.
7. Compilazione verificata con il FQBN corretto (`compile --upload` con lo stesso FQBN!).

## Promemoria flash

```bash
# compila E carica con lo STESSO FQBN (altrimenti "Unexpected chip ID")
arduino-cli compile --upload -p /dev/ttyACM0 --fqbn esp32:esp32:lilygo_t_display_s3 ~/Arduino/BikeGPS_TDisplayS3
```
