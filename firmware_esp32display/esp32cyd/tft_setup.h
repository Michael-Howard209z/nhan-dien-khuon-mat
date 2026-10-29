#ifndef CYD_TFT_SETUP_H
#define CYD_TFT_SETUP_H

// ===========================================================================
//  Cau hinh TFT_eSPI cho ESP32-2432S028 / ESP32-2432S028R (Cheap Yellow Display)
// ===========================================================================
//  QUAN TRONG - DOC KY TRUOC KHI SUA:
//
//   * Panel cua CYD la 240x320 theo CHIEU DOC (native portrait).
//     -> Khung NGANG 320x240 chi dat duoc bang tft.setRotation(1) hoac (3).
//        KHONG dung rotation 0 hay 2: do la portrait 240x320, UI ve theo be
//        ngang 320px se bi CAT chi con ~2/3 man hinh (loi "chi hien 2/3").
//
//   * TUYET DOI KHONG doi TFT_WIDTH=320 / TFT_HEIGHT=240. Panel van la
//     240x320; doi kich thuoc driver se lam SAI cua so dia chi GRAM ->
//     hinh bi lech / chi hien 2/3 man hinh.
//
//   * Neu board la ban 2 cong USB (micro + Type-C) va hinh bi SAI MAU hoac
//     lech, thu doi CYD_PANEL_ST7789 = 1 (xem khoi chon driver phia duoi).
// ===========================================================================

#define USER_SETUP_LOADED

// ---------------------------------------------------------------------------
//  Chon driver panel.
//    0 = ILI9341  (ban 1 cong USB / pho bien nhat)      <-- MAC DINH
//    1 = ST7789V  (mot so ban 2 cong USB dung panel nay)
// ---------------------------------------------------------------------------
#define CYD_PANEL_ST7789  0

#if CYD_PANEL_ST7789
  #define ST7789_DRIVER
  #define TFT_INVERSION_OFF
#else
  #define ILI9341_DRIVER
#endif

// ---------------------------------------------------------------------------
//  Thu tu mau. Neu mau bi HOAN DOI (DO <-> XANH, cyan hien thanh vang) thi
//  doi TFT_RGB <-> TFT_BGR. Panel cua board nay can TFT_RGB (da kiem chung
//  bang anh chup: nen xanh dam hien ra do ~ sai thu tu mau BGR mac dinh).
// ---------------------------------------------------------------------------
#define TFT_RGB_ORDER TFT_RGB

// Native PORTRAIT - KHONG doi thanh 320/240 (xem ghi chu dau file).
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

// ---------------------------------------------------------------------------
//  Chan SPI cho man hinh. XPT2046 (cam ung) nam tren bus SPI rieng.
// ---------------------------------------------------------------------------
#define CYD_DISPLAY_ON_VSPI 0      // 0 = HSPI 13/12/14 (dung cho CYD)

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
  #define TFT_RST  -1              // RST noi vao chan reset ESP32
#endif

#define TFT_CS   15
#define TFT_DC    2
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// ---------------------------------------------------------------------------
//  Font su dung
// ---------------------------------------------------------------------------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ---------------------------------------------------------------------------
//  SPI clock: ILI9341 tren CYD nen chay <= 40MHz. 27MHz on dinh
//  (60MHz tung lam man hinh bi soc/tearing).
// ---------------------------------------------------------------------------
#define SPI_FREQUENCY        27000000
#define SPI_READ_FREQUENCY   10000000
#define SPI_TOUCH_FREQUENCY   2500000

#endif
