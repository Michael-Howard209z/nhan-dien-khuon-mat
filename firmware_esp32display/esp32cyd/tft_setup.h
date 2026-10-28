#ifndef CYD_TFT_SETUP_H
#define CYD_TFT_SETUP_H

#define USER_SETUP_LOADED

// Native landscape 320x240 — KHONG xoay (rotation 0) de tranh tearing
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define CYD_DISPLAY_ON_VSPI 0

#if CYD_DISPLAY_ON_VSPI
  #define TFT_MISO 19
  #define TFT_MOSI 23
  #define TFT_SCLK 18
  #define TFT_RST  12
#else
  #define USE_HSPI_PORT
  #define TFT_MISO 12
  #define TFT_MOSI 13
  #define TFT_SCLK 14
  #define TFT_RST  -1
#endif

#define TFT_CS   15
#define TFT_DC    2
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       60000000
#define SPI_READ_FREQUENCY  10000000
#define SPI_TOUCH_FREQUENCY  2500000

#endif
