// Setup TFT_eSPI per LILYGO T-Display-S3 (ESP32-S3, ST7789 170x320, bus parallelo 8 bit).
//
// Questo file sta NELLA CARTELLA DELLO SKETCH: TFT_eSPI lo carica da solo
// (grazie a __has_include(<tft_setup.h>) in TFT_eSPI.h) senza toccare
// ~/Arduino/libraries/TFT_eSPI/User_Setup.h, che resta valido per gli altri sketch.
//
// Equivale a User_Setups/Setup206_LilyGo_T_Display_S3.h

#define USER_SETUP_ID 206

#define ST7789_DRIVER
#define INIT_SEQUENCE_3   // migliora l'immagine sul T-Display-S3

#define CGRAM_OFFSET
#define TFT_RGB_ORDER TFT_RGB   // ordine colori Red-Green-Blue

#define TFT_INVERSION_ON

#define TFT_PARALLEL_8_BIT

#define TFT_WIDTH  170
#define TFT_HEIGHT 320

// Bus parallelo 8 bit
#define TFT_CS  6
#define TFT_DC  7
#define TFT_RST 5
#define TFT_WR  8
#define TFT_RD  9

#define TFT_D0 39
#define TFT_D1 40
#define TFT_D2 41
#define TFT_D3 42
#define TFT_D4 45
#define TFT_D5 46
#define TFT_D6 47
#define TFT_D7 48

#define TFT_BL 38
#define TFT_BACKLIGHT_ON HIGH

// Font da caricare in flash (FONT7 e FONT8 sono i "7 segment" grandi)
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT
