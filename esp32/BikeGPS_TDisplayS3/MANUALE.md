# BikeGPS T-Display-S3 — Manuale

Manuale d'uso e diagnostica del firmware `BikeGPS_TDisplayS3.ino`.
Scheda: **LILYGO T-Display-S3** (ESP32-S3, TFT IPS ST7789 170×320, bus parallelo 8 bit, batteria LiPo).

Il dispositivo è un **mirror GPS via BLE**: l'app Android gli manda la telemetria, lui la
mostra sul display e in parallelo legge **direttamente** la fascia cardio via BLE
(Heart Rate Service 0x180D).

---

## 1. Accensione e spegnimento

| azione | gesto |
|---|---|
| **accendere** | premi uno qualunque dei due tasti (o collega l'USB) |
| **spegnere** | tieni premuti **entrambi i tasti per 3 s** → deep sleep |
| riavviare (software) | comando seriale `R` |
| riavviare (hardware) | pulsante EN/RESET della scheda |

Lo spegnimento è un **deep sleep**: display e pannello spenti, BLE spento, CPU ferma
(consumo ~10–20 µA + ~100–300 µA della scheda). La **batteria resta collegata** e, se c'è
l'USB-C, **continua a caricarsi** (la ricarica è hardware, indipendente dalla CPU).
La scheda non ha un interruttore di alimentazione: il deep sleep è l'unico "power off"
possibile via firmware. All'avvio, se il boot è un risveglio dal sonno, il log seriale lo indica
(`wake da deep sleep: cause=2 (tasti)`).

---

## 2. Tasti

Due pulsanti integrati:

- **SET / sinistro** = `GPIO0` (il tasto BOOT)
- **PAGE / destro** = `GPIO14`

I primi **2 secondi** dopo il boot i tasti sono ignorati (i pin non sono ancora assestati).
Anti-rimbalzo: 50 ms.

| gesto | pagina | effetto |
|---|---|---|
| **destro corto** | RIDE / DIAG | pagina successiva (RIDE ↔ DIAG) |
| **destro corto** | SETUP | campo successivo; dopo l'ultimo esce dalla SETUP |
| **destro lungo** (0,8 s) | RIDE | — (niente) |
| **destro lungo** (0,8 s) | DIAG / SETUP | torna subito a RIDE |
| **destro lungo** (3 s) | RIDE | **STOP trip** |
| **sinistro corto** | SETUP | **+1** sulla soglia del campo selezionato |
| **sinistro corto** | RIDE / DIAG | — (niente) |
| **sinistro lungo** (0,8 s) | SETUP | **−1** sulla soglia del campo |
| **sinistro lungo** (3 s) | RIDE | **START trip** |
| **entrambi 1 s** | RIDE / DIAG | apre la pagina **SETUP** (o la chiude) |
| **entrambi 3 s** | ovunque | **spegnimento** (deep sleep) |

> Attenzione alla finestra tra 1 s e 3 s: per aprire la SETUP rilascia entro i 3 secondi,
> altrimenti la scheda si spegne.

---

## 3. Pagine

### 3.1 RIDE (uso in bici)
- **pannello velocità** gigante (font VLW 48 px, due decimali: `38.24`; `--.--` se assente)
- **BPM** affiancato, con il colore della zona cardiaca
- tre celle sotto: **TEMPO** (h:mm, con auto-pausa), **CADENZA**, **WATT**
- icone di stato (vedi §5) nel pannello velocità

### 3.2 SETUP (soglie cardio)
Si apre/chiude con **entrambi i tasti per 1 s**. Non è nel giro normale delle pagine.
- 4 righe: `Z1/Z2`, `Z2/Z3`, `Z3/Z4`, `Z4/Z5` — sono i **limiti in bpm** tra le zone
- riga selezionata evidenziata in giallo
- **destro** = campo successivo · **sinistro corto** = +1 · **sinistro lungo** = −1
- limiti: 60…220 bpm, sempre monotoni crescenti (modificandone una si aggiustano le altre)
- barra delle zone in basso, con la zona corrente del battito
- soglie **salvate in flash (NVS)**, restano dopo lo spegnimento
- in orizzontale le istruzioni non sono stampate a schermo (solo in verticale)

### 3.3 DIAG (diagnostica a display)
8 celle: **BLE**, **CARDIO bpm**, **SATELLITI**, **BATTERIA**, **QUOTA m**, **PENDENZA %**,
**HEAP**, **UPTIME**.

---

## 4. Trip (cronometro di allenamento)

- **START**: tasto sinistro tenuto 3 s sulla pagina RIDE
- **STOP**: tasto destro tenuto 3 s sulla pagina RIDE
- **auto-pausa** sotto i **4 km/h**
- il tempo compare nella cella TEMPO, formato h:mm
- colori: **grigio** = fermo, **verde** = in movimento, **giallo** = in auto-pausa

---

## 5. Icone di stato (header)

| icona | significato |
|---|---|
| testo **GPS** | blu se arrivano dati dal telefono (ultimi 5 s), grigio se offline |
| runa **Bluetooth** | blu se l'app è connessa, grigia se no |
| **cuore** | verde se legge i bpm · rosso lampeggiante (1 Hz) se sta acquisendo · grigio se assente |
| **batteria** | pittogramma a 4 segmenti; verde >40 %, giallo 20–40 %, rosso <20 % |

Soglie di "dato presente": telemetria GPS entro 5 s, battito entro 5 s.
La percentuale batteria è lineare tra 3,30 V (0 %) e 4,20 V (100 %); la tensione è letta
sul partitore 1:2 di `GPIO4`.

---

## 6. Zone cardiache

| zona | colore | intervallo |
|---|---|---|
| Z1 | grigio | sotto la 1ª soglia |
| Z2 | verde | 1ª…2ª soglia |
| Z3 | giallo | 2ª…3ª soglia |
| Z4 | arancio | 3ª…4ª soglia |
| Z5 | rosso | oltre la 4ª soglia |

Soglie di default (per FCmax 170): **102 / 119 / 136 / 153 bpm**.
La fascia cardio usata nei test è una **Geonaute BLE**.

---

## 7. Profilo a basso consumo

Blocco `LP_*` in cima allo sketch, attivo di default:

| manopola | default | significato |
|---|---|---|
| `LP_ENABLE` | 1 | 1 = profilo low power, 0 = come prima |
| `LP_CPU_MHZ` | 80 | 240 / 160 / 80 (40 NON affidabile col BLE) |
| `LP_WIFI_OFF` | 1 | spegne esplicitamente il WiFi (BikeGPS non lo usa) |
| `LP_ADV_MS` | 500 | periodo advertising BLE (ms) |
| `LP_BL_DUTY` | 255 | retroilluminazione 0…255 (PWM) |

Applicato in `setup()`: `setCpuFrequencyMhz()` **prima** di BLE e pannello, WiFi off,
advertising a 500 ms, backlight via `ledcWrite`. La retroilluminazione è **sempre accesa**
(niente dim automatico). Override in compilazione:

```bash
arduino-cli compile -b esp32:esp32:lilygo_t_display_s3 \
  --build-property "compiler.cpp.extra_flags=-DLP_ENABLE=0"
```

Costo del profilo sul rendering (il bus TFT è in bit-banging, quindi dipende dalla CPU):
`fillScreen` 27,5 ms a 240 MHz → **38,6 ms** a 80 MHz; cambio pagina RIDE 68,8 → 127,8 ms.
Il disegno è incrementale, quindi il frame tipico in bici resta ~5 ms. Lo scheduler resta
stabile (`jitterMax=0` a regime).

---

## 8. Compilazione e caricamento

```bash
cd ~/AndroidStudioProjects/GpsPosition/esp32/BikeGPS_TDisplayS3

# compila
arduino-cli compile -b esp32:esp32:lilygo_t_display_s3 .

# carica (libera prima la porta: chiudi il monitor seriale!)
arduino-cli upload -p /dev/ttyACM0 -b esp32:esp32:lilygo_t_display_s3 .
```

> Compila e carica **sempre con lo stesso FQBN**, altrimenti `arduino-cli` può sbagliare i
> parametri di flash ("Unexpected chip ID").

Librerie: **NimBLE-Arduino ≥2.x**, **TFT_eSPI 2.5.43** (setup in `tft_setup.h` locale allo
sketch: non tocca la `User_Setup.h` globale), font VLW in `vlw_fonts.h`.

Configurazione display: `ROTATION 1` = **orizzontale** (320×170); `ROTATION 0/2` = verticale
(170×320), con geometria adattiva.

---

## 9. Comandi seriali (diagnostica)

Monitor seriale **115200**. Digita il comando e premi Invio.
Strumento pronto: `~/bin/monitor-seriale.py /dev/ttyACM0 115200` (traduce `\n`→`\r\n` e si
riconnette da solo dopo un deep sleep).

### Stato e misure

| cmd | effetto |
|---|---|
| `s` | stato completo: pagina, trip, heap, tasti, soglie, e `power: cpu/apb/wifi/adv/bl` |
| `j` | scheduler: numero esecuzioni, `jitterMax` (poi azzera), `hrConnect` |
| `h` | diagnostica cardio: stato macchina, client, ultimo bpm, scansione |
| `m` | benchmark rendering di tutte le pagine (fill / drawFull / warm / cold) |
| `q` | profilo per componente (hero velocità, celle griglia, tasto SET) |
| `C` | **cicla la CPU a caldo** 240 → 160 → 80 → 40 MHz (per i test consumi) |
| `f` | larghezze reali dei font + geometria dei 7 segmenti |
| `w` | test font VLW (3 righe + riga rossa di riferimento) |

### Pagine

| cmd | effetto |
|---|---|
| `n` | pagina successiva (RIDE ↔ DIAG) |
| `p` | pagina precedente |
| `a` | apre/chiude la pagina SETUP |
| `g` | in SETUP: campo successivo |

### Tasti simulati

| cmd | effetto |
|---|---|
| `z` | pressione breve SET = +1 sulla soglia (in SETUP) |
| `t` | pressione lunga SET: in SETUP −1 (fuori da SETUP nessuna azione) |

### Simulazione e cardio di test

| cmd | effetto |
|---|---|
| `y` | simulazione velocità/cadenza/watt ON/OFF |
| `c` | simula una fascia a un MAC inesistente `DE:AD:BE:EF:00:01` (caso peggiore) |

### Pannello e sistema

| cmd | effetto |
|---|---|
| `r` / `v` / `k` | fillScreen **rosso** / **verde** / **nero** (test pannello, bypassa gli sprite) |
| `d` | re-init del pannello + redraw completo + stato |
| `B` | **cicla la luminosità** del backlight a caldo (100 → 78 → 62 → 50 → 37 → 25 → 12 → 0%) |
| `R` | **riavvio** della scheda |
| `l` | **curva di scarica**: stampa i campioni del datalogger + durata e autonomia stimate |
| `L` | azzera il log di scarica |

`B` cicla la luminosità del backlight a caldo, per misurare il consumo a banco. Il valore
**non è persistente** (al riavvio torna a `LP_BL_DUTY`); la manopola `LP_BL_DUTY` resta il default.

---

## 10. Modalità test durata batteria (provvisoria)

Con `TEST_MODE 1` il firmware attiva due cose automatiche, utili solo per misurare
l'autonomia della batteria:

- **datalogger batteria**: ogni `TEST_LOG_PERIOD_S` salva in flash (NVS, namespace `blog`)
  un campione `{millis, mV}` a **ring buffer** di `TEST_LOG_MAX` voci (256 × 5 min ≈ 21 h).
  I dati **sopravvivono** alla scarica completa e a un reset.
- **carico ciclico**: ogni `TEST_PAGE_S` alterna RIDE ↔ DIAG e tiene la simulazione attiva
  (consumo da "scheda in funzione", non a schermo fermo).

| parametro | default | significato |
|---|---|---|
| `TEST_MODE` | 1 | 1 = datalogger + carico automatico, **0 a fine test** |
| `TEST_LOG_PERIOD_S` | 300 | campionamento batteria (s) |
| `TEST_PAGE_S` | 30 | cambio pagina automatico (s) |
| `TEST_LOG_MAX` | 256 | campioni conservati in flash (ring) |
| `BATT_MAH` | 140 | capacità dichiarata della batteria (mAh) |

Sono tutti sovrascrivibili in compilazione (`#ifndef`), es.:

```bash
arduino-cli compile -b esp32:esp32:lilygo_t_display_s3 \
  --build-property "compiler.cpp.extra_flags=-DTEST_LOG_PERIOD_S=60 -DTEST_PAGE_S=10" .
```

**Procedura del test**: si flasha, si azzera il log (`L`), si **stacca l'USB** e si lascia la
scheda accesa a batteria. Al ritorno si ricollega l'USB (attenzione: ricollegando parte la
**ricarica**, quindi il test finisce lì) e si legge il log con `l`.

Il comando `l` stampa i campioni (`hh.hh h · mV · %`) e un riepilogo con durata osservata,
pendenza (%/h), autonomia stimata da 0-100 % e **corrente media** (`BATT_MAH × %/h ÷ 100`).

> A fine test rimettere `TEST_MODE 0`: la versione normale non deve cambiare pagina da sola.
> Dopo un deep sleep o una scarica completa il tempo `millis` riparte da zero: se il log
> mescola sessioni diverse il comando segnala `tempo non monotono`.

---

## 11. Note tecniche

- Niente `delay()`: lo scheduler è cooperativo a tabella (`buttons` 10 ms, `hr` 5 ms,
  `serial` 20 ms, `display` 500 ms, `battery` 1000 ms, `nvs` 1000 ms, …); il `loop()` fa solo
  `vTaskDelay(1)` come yield.
- `hrTask` gira **ogni 5 ms** e la connessione cardio avviene **nel loop** (core 1):
  NimBLE è pinnato al core 0 e un worker bloccante sullo stesso core faceva cadere la
  connessione.
- Salvataggio NVS **ritardato**: le pressioni dei tasti marcano "dirty" e un task a 1 Hz
  scrive solo se i valori sono cambiati e fermi da 1 s.
- Il disegno è **incrementale**: cache dei valori + `setTextPadding` (cancella solo l'area del
  testo) e sprite per velocità e BPM, per non avere flicker.
- L'anti-flicker con sprite **a tutto schermo** su questa scheda manda il pannello in black
  screen: si usano sprite piccoli, mai `pushSprite` full-screen.
- ⚠️ **Attenzione**: `Serial` è condivisa tra log e comandi; niente `delay()` nel percorso di
  spegnimento, `Serial.flush()` basta.
- ⚠️ **PWM del backlight**: `TFT_eSPI::init()` riconfigura `TFT_BL` (GPIO38) come uscita
  digitale e lo mette HIGH (TFT_eSPI.cpp, `#if defined TFT_BL && TFT_BACKLIGHT_ON`),
  annullando un `ledcAttach` fatto prima. Il `ledcAttach` va quindi eseguito **dopo**
  `tft.init()` e dopo ogni re-init (es. comando `d`), altrimenti `ledcWrite` non ha effetto
  e il backlight resta al 100%.
