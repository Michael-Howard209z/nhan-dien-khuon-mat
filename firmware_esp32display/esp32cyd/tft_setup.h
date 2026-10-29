#ifndef CYD_TFT_SETUP_H
#define CYD_TFT_SETUP_H

#define USER_SETUP_LOADED

// CYD 2.8" = ILI9341 240x320 PORTRAIT.
// Man hinh NGANG 320x240 dat duoc bang tft.setRotation(1) trong sketch.
// KHONG doi TFT_WIDTH=320/TFT_HEIGHT=240: panel van la 240x320, doi kich
// thuoc driver se sai cua so dia chi -> chi ve duoc 2/3 man hinh.
#define ILI9341_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 240

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

#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  10000000
// Ghi chu: 60MHz tung lam man hinh bi soc/tearing (commit f5c7c52 ghi "20MHz"
// nhung thuc te de 60MHz). ILI9341 chi nen chay <=40MHz tren CYD.
// Muon thu nhanh hon: 40000000. Neu van soc -> 20000000.
#define SPI_TOUCH_FREQUENCY  2500000

#endif
