/* ===========================================================================
 *  TFT_eSPI setup cho ESP32-2432S028R (Cheap Yellow Display - CYD)
 * ===========================================================================
 *
 *  TFT_eSPI tu dong include file nay vi no nam trong thu muc sketch
 *  (co che "Sketch_with_tft_setup" cua TFT_eSPI).
 *
 *  LUU Y QUAN TRONG - hai bien the day SPI cua CYD:
 *
 *  1) Ban CYD pho bien (mac dinh): man hinh dung HSPI
 *       MOSI=13, MISO=12, SCLK=14, CS=15, DC=2, RST=-1, BL=21
 *     => de CYD_DISPLAY_ON_VSPI = 0
 *
 *  2) Mot so tai lieu/ban variant ghi man hinh dung VSPI 23/19/18:
 *       MOSI=23, MISO=19, SCLK=18, CS=15, DC=2, RST=12, BL=21
 *     => doi CYD_DISPLAY_ON_VSPI = 1
 *     (khi do cam ung se tu chuyen sang HSPI de khong dung chung bus)
 *
 *  Neu man hinh trang / khong hien gi: thu doi CYD_DISPLAY_ON_VSPI,
 *  kiem tra lai day BL=21, va chac chan chi co MOT file setup TFT_eSPI
 *  duoc nap (file nay duoc uu tien hon User_Setup.h).
 * =========================================================================== */

#ifndef CYD_TFT_SETUP_H
#define CYD_TFT_SETUP_H

#define USER_SETUP_LOADED

// --- ILI9341 320x240 (landscape) --------------------------------------------
// CYD duoc chay o che do landscape 320x240 (rotation 1). TFT_eSPI tu hoan doi
// chieu rong/cao sau setRotation(1), nen o day khai bao duoi dang portrait
// de TFT_eSPI khoi tao driver dung, sau do setRotation(1) se danh cho landscape.
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// 0 = HSPI (ban CYD pho bien), 1 = VSPI (variant 23/19/18)
#define CYD_DISPLAY_ON_VSPI 0

#if CYD_DISPLAY_ON_VSPI
  // Variant: man hinh tren VSPI
  #define TFT_MISO 19
  #define TFT_MOSI 23
  #define TFT_SCLK 18
  #define TFT_RST  12
#else
  // Ban CYD pho bien: man hinh tren HSPI
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

// Font can dung (font 4 = ten hoc sinh, font 2 = thong tin)
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// SPI clock: 40 MHz mac dinh, tang len 60 MHz de tang FPS hien thi.
// ILI9341 tren CYD thong thuong hoat din 60-80 MHz. Neu man hinh xoi/nhieu
// sau khi tang, giam lai 40000000.
#define SPI_FREQUENCY       60000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// Neu mau bi sai (do <-> xanh): them dong nay
// #define TFT_RGB_ORDER TFT_BGR

#endif  // CYD_TFT_SETUP_H
