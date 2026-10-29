/* ===========================================================================
 *  ESP32-2432S028R (CYD) - MAN HINH CAN MAT + KET QUA DIEM DANH
 *  Firmware: 2026-10-29-cyd5 (viet lai tu dau, sach will)
 * ===========================================================================
 *
 *  Vai tro:
 *    1. CHI hien thi anh LIVE trong 15 giay sau khi bam nut tren ESP32-CAM
 *       (MJPEG http://<cam>:81/stream). Khi NGHI KHONG mo stream ->
 *       ESP32-CAM khong chup lien tuc -> giam nhiet, tiet kiem dien.
 *    2. Doc ket qua nhan dien tu ESP32-CAM (GET http://<cam>:80/result)
 *       va hien thi ten / trang thai diem danh (banner).
 *    3. ESP32-CAM bao bat dau/ket thuc qua UDP FACESYNC|SCAN|1|<ip> /
 *       FACESYNC|SCAN|0|<ip> (cong 4321) -> CYD mo/dong stream NGAY,
 *       khong cho 0.5s poll. Neu UDP bi chan, cot `scanning` trong /result
 *       lam duong du phong.
 *
 *  Trang thai man hinh:
 *    - STANDBY (nghi): trang "SAN SANG DIEM DANH", khong truyen anh.
 *    - SCANNING (dang quet): video tu camera + thanh tien trinh + banner ket qua.
 *
 *  Phan cung (xem tft_setup.h):
 *    - ILI9341 240x320 (portrait) SPI HSPI: MOSI=13, MISO=12, SCLK=14,
 *      CS=15, DC=2, RST=-1, BL=21. Man hinh NGANG 320x240 = setRotation(2).
 *    - XPT2046 (VSPI): CS=33, IRQ=36, CLK=25, MOSI=32, MISO=39
 *
 *  Thu vien can cai:
 *    - TFT_eSPI (Bodmer) 2.5.43+
 *    - XPT2046_Touchscreen (Paul Stoffregen) 1.4+
 *    - JPEGDEC (Larry Bank) 1.8.5+
 *
 *  Lenh Serial:
 *    /help, /ip, /ip <ip>, /find, /live on|off, /test, /rot 0..7,
 *    /touch, /stats, /reboot
 *
 * =========================================================================== */

#include <Preferences.h>
#include <SPI.h>
#include <FS.h>                // JPEGDEC dung kieu File -> phai include truoc
#include "tft_setup.h"         // Nap cau hinh ILI9341/CYD truoc khi include TFT_eSPI
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <JPEGDEC.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

// ============================== CAU HINH ====================================
#define FW_VERSION        "2026-10-29-cyd5"

#define CYD_SELFTEST      1    // 1 = tu chay self-test luc boot (~2.5s)

#define WIFI_SSID         "Test"
#define WIFI_PASS         "88888888"

#define CAM_IP_DEFAULT    "192.168.0.101"
#define CAM_HTTP_PORT     80
#define CAM_STREAM_PORT   81

#define RESULT_POLL_MS    500      // doc /result (du phong neu UDP bi chan)
#define STREAM_RETRY_MS   400      // thu lai stream NHANH khi dang quet
#define SCAN_NOTIFY_MS    4000     // khong tin hieu quet 4s -> ket thuc (fail-safe)
#define SCAN_MAX_MS       20000    // khong de "scanning" treo mai
#define RESULT_HOLD_MS    10000    // giu khung cuoi + ket qua sau khi quet xong

#define DISC_PORT         4321
#define DISC_MAGIC        "FACESYNC"

// Cam ung XPT2046 - CYD
#define TOUCH_CS_PIN      33
#define TOUCH_IRQ_PIN     36
#define TOUCH_SCLK_PIN    25
#define TOUCH_MISO_PIN    39
#define TOUCH_MOSI_PIN    32

#define TOUCH_SWAP_XY     0
#define TOUCH_INVERT_X    0
#define TOUCH_INVERT_Y    0

// Vung gia tri RAW cua XPT2046 tren CYD (hieu chuan bang lenh /touch)
#define TOUCH_X_MIN       1500
#define TOUCH_X_MAX       3000
#define TOUCH_Y_MIN       1400
#define TOUCH_Y_MAX       2900

#define LCD_W             320
#define LCD_H             240

// Mau rieng
#define C_BG              0x0841
#define C_HEADER          0x0010
#define C_OK_BG           0x0A64
#define C_ALREADY_BG      0x0010
#define C_ERR_BG          0x7800
#define C_TEXT_DIM        0xA514
#define C_GUIDE           0xDEDB

// ============================== DOI TUONG ===================================
TFT_eSPI tft = TFT_eSPI();

// Rotation mac dinh = 2 (landscape 320x240). Co the doi bang /rot <0..7>.
static uint8_t g_rotation = 2;

#if CYD_DISPLAY_ON_VSPI
  SPIClass touchSPI(HSPI);
#else
  SPIClass touchSPI(VSPI);
#endif
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

JPEGDEC jpeg;
WiFiClient streamClient;
WiFiUDP udp;
Preferences prefs;
HTTPClient resultHttp;

// ============================== BIEN TRANG THAI =============================
enum StreamState { SS_OFF, SS_CONNECTING, SS_LIVE, SS_RETRY };
static StreamState ss = SS_OFF;

static char camIp[16] = CAM_IP_DEFAULT;
static bool udpReady = false;
static bool resultHttpOpen = false;

static bool manualPaused = false;   // nguoi dung bam nut DUNG/XEM tren man hinh
static bool scanning = false;       // ESP32-CAM dang quet 15s
static uint32_t scanStartMs = 0;
static uint32_t scanEndMs = 0;
static uint32_t lastScanNotifyMs = 0;  // lan cuoi nhan tin hieu "dang quet" (UDP hoac /result)
static bool standbyShown = false;   // da ve trang standby khi nghi

static bool lastFrameValid = false;
static bool idleDirty = true;
static bool wifiDown = false;

static uint32_t lastResultPoll = 0;
static uint32_t lastRetry = 0;
static uint32_t lastDiscovery = 0;
static uint32_t lastWifiTry = 0;
static uint32_t lastFpsMs = 0;
static uint32_t lastStatsMs = 0;
static uint32_t lastScanOverlayMs = 0;

static uint32_t framesThisSec = 0;
static uint32_t fps = 0;
static uint32_t lastDecodeMs = 0;
static size_t lastFrameBytes = 0;

// Bo dem JPEG (cap phat 1 lan luc boot)
static uint8_t *jpegBuf = nullptr;
static size_t jpegCap = 0;

// Trang thai doc MJPEG chunked
static bool streamChunked = false;
static size_t chunkLeft = 0;
static bool chunkNeedCRLF = false;

// Ket qua nhan dien
static uint32_t lastSeq = 0;
static bool haveSeq = false;
static char resNameAscii[64] = "";

// ============================== KHAI BAO HAM ================================
static void applyRotation(uint8_t r);
static void drawSelfTest();
static void drawStandbyPage();
static void drawOverlay();
static void drawIdlePage();
static void drawStatusPage(const char *title, const char *sub, uint16_t accent);
static void streamStop();
static bool streamConnect();
static int readFrame(size_t *outLen);
static bool showJpeg(size_t len);
static bool pollResult();
static void applyResult(const char *json);
static void startScan(const char *why);
static void endScan(const char *why);
static void pollUdp();
static bool discoverCam(uint16_t waitMs);
static bool wifiConnect(uint32_t timeoutMs);
static void serialPoll();
static void touchPoll();
static void handleSerialLine(char *line);
static void printStats();
static void drawConnBox(const char *title, const char *sub);

// ============================== HIEN THI ====================================
static void drawPauseButton(int x, int y) {
  uint16_t bc = manualPaused ? TFT_DARKGREEN : 0x3186;
  tft.fillRoundRect(x, y, 68, 20, 5, bc);
  tft.drawRoundRect(x, y, 68, 20, 5, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, bc);
  tft.drawString(manualPaused ? "XEM" : "DUNG", x + 34, y + 10, 1);
}

static void drawProgress() {
  uint32_t elapsed = millis() - scanStartMs;
  if (elapsed > 15000) elapsed = 15000;
  int w = (int)((elapsed * 300UL) / 15000UL);
  if (w < 0) w = 0;
  if (w > 300) w = 300;
  tft.fillRect(10, 208, 300, 5, 0x2104);
  if (w > 0) tft.fillRect(10, 208, w, 5, TFT_ORANGE);
}

static char bannerName[64] = "";
static char bannerSub[48] = "";
static uint16_t bannerBg = C_OK_BG;
static uint16_t bannerFg = TFT_WHITE;
static uint32_t bannerUntil = 0;
static bool bannerOn = false;

static void drawBanner() {
  int bx = 14, by = 84, bw = 292, bh = 76;
  tft.fillRoundRect(bx, by, bw, bh, 12, bannerBg);
  tft.drawRoundRect(bx, by, bw, bh, 12, bannerFg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(bannerFg, bannerBg);
  const char *name = bannerName[0] ? bannerName : "HOC SINH";
  if (tft.textWidth(name, 4) > bw - 24) {
    tft.drawString(name, 160, by + 26, 2);
  } else {
    tft.drawString(name, 160, by + 26, 4);
  }
  tft.setTextFont(2);
  tft.drawString(bannerSub, 160, by + 56, 2);
}

// Bien banner (luu o day de hai ham drawBanner / setBanner chia se)

static void setBanner(const char *name, const char *sub,
                      uint16_t bg, uint16_t fg, uint32_t ms) {
  strncpy(bannerName, name ? name : "", sizeof(bannerName) - 1);
  bannerName[sizeof(bannerName) - 1] = '\0';
  strncpy(bannerSub, sub ? sub : "", sizeof(bannerSub) - 1);
  bannerSub[sizeof(bannerSub) - 1] = '\0';
  bannerBg = bg;
  bannerFg = fg;
  bannerUntil = millis() + ms;
  bannerOn = true;
  standbyShown = false;
  if (manualPaused) idleDirty = true;
}

static void drawTopBar() {
  tft.fillRect(0, 0, LCD_W, 24, TFT_BLACK);

  uint16_t dot = TFT_RED;
  const char *state = "OFF";
  if (manualPaused) { dot = TFT_LIGHTGREY; state = "TAM DUNG"; }
  else if (scanning) { dot = TFT_ORANGE; state = "DANG QUET"; }
  else if (ss == SS_LIVE) { dot = TFT_GREEN; state = "LIVE"; }
  else { dot = TFT_YELLOW; state = "CHO NUT"; }

  tft.fillCircle(9, 12, 4, dot);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(state, 18, 4, 2);

  if (scanning) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("GIU MAT YEN", 118, 4, 2);
  } else {
    tft.setTextColor(C_TEXT_DIM, TFT_BLACK);
    tft.drawString(camIp, 82, 4, 2);
    char b[16];
    snprintf(b, sizeof(b), "%2u fps", (unsigned)fps);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(b, 188, 4, 2);
  }

  drawPauseButton(248, 2);
}

static void drawGuide(bool showText) {
  uint16_t c = scanning ? TFT_ORANGE : C_GUIDE;
  int x0 = 66, y0 = 38, x1 = 254, y1 = 202, L = 24;
  for (int i = 0; i < 2; i++) {
    tft.drawFastHLine(x0, y0 + i, L, c);
    tft.drawFastVLine(x0 + i, y0, L, c);
    tft.drawFastHLine(x1 - L, y0 + i, L, c);
    tft.drawFastVLine(x1 - i, y0, L, c);
    tft.drawFastHLine(x0, y1 - i, L, c);
    tft.drawFastVLine(x0 + i, y1 - L, L, c);
    tft.drawFastHLine(x1 - L, y1 - i, L, c);
    tft.drawFastVLine(x1 - i, y1 - L, L, c);
  }
  if (showText) {
    tft.fillRect(86, 203, 148, 11, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(c, TFT_BLACK);
    tft.drawString("CAN MAT VAO KHUNG", 160, 208, 1);
  }
}

static void drawBottomBar() {
  tft.fillRect(0, 216, LCD_W, 24, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_TEXT_DIM, TFT_BLACK);
  if (scanning) {
    tft.drawString("Dang nhan dien - giu mat yen", 8, 222, 1);
  } else if (manualPaused) {
    tft.drawString("Cham goc tren phai de bat camera", 8, 222, 1);
  } else {
    tft.drawString("Bam nut tren camera de diem danh", 8, 222, 1);
  }
}

static void drawOverlay() {
  drawTopBar();
  drawGuide(true);
  drawBottomBar();

  bool showBanner = bannerOn && (scanning ||
                    (int32_t)(millis() - bannerUntil) < 0);
  if (showBanner) drawBanner();
  else bannerOn = false;

  if (scanning) {
    drawProgress();
    tft.fillRect(66, 203, 188, 2, TFT_BLACK);
  }
}

static void drawSplash(const char *msg) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_CYAN, C_BG);
  tft.drawString("MAN HINH CAN MAT", 160, 82, 4);
  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, C_BG);
  tft.drawString(msg, 160, 126, 2);
  tft.setTextFont(1);
  tft.setTextColor(TFT_DARKGREY, C_BG);
  tft.drawString(FW_VERSION, 160, 220, 1);
}

static void applyRotation(uint8_t r) {
  g_rotation = r & 7;
  tft.setRotation(g_rotation);
  Serial.printf("[TFT] rotation %u -> %dx%d\n",
                (unsigned)g_rotation, tft.width(), tft.height());
}

static void drawSelfTest() {
  const uint16_t colors[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  for (uint8_t i = 0; i < 4; i++) {
    tft.fillScreen(colors[i]);
    delay(180);
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, LCD_W, LCD_H, TFT_WHITE);
  tft.drawRect(1, 1, LCD_W - 2, LCD_H - 2, TFT_WHITE);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); tft.drawString("0,0", 4, 4, 1);
  tft.setTextDatum(TR_DATUM); tft.drawString("319,0", LCD_W - 4, 4, 1);
  tft.setTextDatum(BL_DATUM); tft.drawString("0,239", 4, LCD_H - 4, 1);
  tft.setTextDatum(BR_DATUM); tft.drawString("319,239", LCD_W - 4, LCD_H - 4, 1);

  char buf[32];
  snprintf(buf, sizeof(buf), "SELFTEST %dx%d", tft.width(), tft.height());
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(buf, LCD_W / 2, LCD_H / 2 - 10, 4);
  tft.setTextFont(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  char line2[48];
  snprintf(line2, sizeof(line2), "rot %u - Vien trang kin 4 canh = panel OK",
           (unsigned)g_rotation);
  tft.drawString(line2, LCD_W / 2, LCD_H / 2 + 22, 1);
  delay(1600);
}

static void drawStandbyPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, LCD_W, 30, C_HEADER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, C_HEADER);
  tft.drawString("MAN HINH CAN MAT", 10, 7, 2);
  drawPauseButton(248, 5);

  uint16_t ic = TFT_GREEN;
  tft.drawRoundRect(105, 54, 110, 58, 10, ic);
  tft.drawRoundRect(106, 55, 108, 56, 9, ic);
  tft.drawCircle(160, 83, 20, ic);
  tft.drawCircle(160, 83, 11, ic);
  tft.fillRect(135, 46, 24, 9, ic);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("SAN SANG DIEM DANH", 160, 144, 4);

  tft.setTextFont(2);
  tft.setTextColor(C_TEXT_DIM, C_BG);
  tft.drawString("Bam nut tren ESP32-CAM de chup anh", 160, 176, 2);
  tft.setTextFont(1);
  tft.setTextColor(TFT_DARKGREY, C_BG);
  tft.drawString("Man hinh chi nhan anh trong 15 giay quet", 160, 198, 1);

  char b[64];
  snprintf(b, sizeof(b), "Cam: %s   WiFi: %d dBm", camIp, (int)WiFi.RSSI());
  tft.drawString(b, 160, 220, 1);

  if (bannerOn) drawBanner();
}

static void drawIdlePage() {
  drawStandbyPage();
}

static void drawStatusPage(const char *title, const char *sub, uint16_t accent) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, LCD_W, 30, C_HEADER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, C_HEADER);
  tft.drawString("MAN HINH CAN MAT", 10, 7, 2);
  drawPauseButton(248, 5);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(accent, C_BG);
  tft.drawString(title, 160, 105, 4);

  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, C_BG);
  tft.drawString(sub, 160, 146, 2);

  tft.setTextFont(1);
  tft.setTextColor(TFT_DARKGREY, C_BG);
  char b[48];
  snprintf(b, sizeof(b), "Cam: %s   WiFi: %d dBm", camIp, (int)WiFi.RSSI());
  tft.drawString(b, 160, 218, 1);

  if (bannerOn) drawBanner();
}

// ============================== JPEG DECODE =================================
static int16_t drawOffX = 0;
static int16_t drawOffY = 0;

static int drawMCUs(JPEGDRAW *pDraw) {
  int32_t x = pDraw->x + drawOffX;
  int32_t y = pDraw->y + drawOffY;
  int32_t stride = pDraw->iWidth;
  int32_t w = pDraw->iWidthUsed ? pDraw->iWidthUsed : stride;
  int32_t h = pDraw->iHeight;
  if (w <= 0 || h <= 0 || stride <= 0) return 1;
  if (w > stride) w = stride;
  if (x >= LCD_W || y >= LCD_H || x + w <= 0 || y + h <= 0) return 1;

  int32_t drawW = w;
  int32_t drawH = h;
  if (x + drawW > LCD_W) drawW = LCD_W - x;
  if (y + drawH > LCD_H) drawH = LCD_H - y;
  if (drawW <= 0 || drawH <= 0) return 1;

  if (drawW == stride) {
    tft.pushImage(x, y, drawW, drawH, pDraw->pPixels);
  } else {
    for (int32_t r = 0; r < drawH; r++) {
      tft.pushImage(x, y + r, drawW, 1, pDraw->pPixels + (size_t)r * stride);
    }
  }
  return 1;
}

static bool showJpeg(size_t len) {
  if (!jpeg.openRAM(jpegBuf, (int)len, drawMCUs)) {
    Serial.println("[JPEG] openRAM that bai");
    return false;
  }

  int w = jpeg.getWidth();
  int h = jpeg.getHeight();
  int opt = 0;
  int shift = 0;
  if (w >= 1120) { opt = JPEG_SCALE_QUARTER; shift = 2; }
  else if (w >= 560) { opt = JPEG_SCALE_HALF; shift = 1; }

  int outW = w >> shift;
  int outH = h >> shift;
  drawOffX = (outW < LCD_W) ? (int16_t)((LCD_W - outW) / 2) : 0;
  drawOffY = (outH < LCD_H) ? (int16_t)((LCD_H - outH) / 2) : 0;

  Serial.printf("[JPEG] %dx%d -> scale=%d out=%dx%d off=%d,%d\n",
                w, h, opt, outW, outH, drawOffX, drawOffY);

  if (outW < LCD_W || outH < LCD_H) {
    tft.startWrite();
    tft.fillRect(drawOffX, drawOffY, outW, outH, TFT_BLACK);
    tft.endWrite();
  }

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  uint32_t t0 = micros();
  int ok = jpeg.decode(0, 0, opt);
  lastDecodeMs = (micros() - t0) / 1000;
  jpeg.close();

  if (!ok) {
    Serial.println("[JPEG] decode that bai");
    return false;
  }
  return true;
}

// ============================== STREAM MJPEG ================================
static void streamStop() {
  if (streamClient.connected()) streamClient.stop();
  streamClient.stop();
  ss = SS_OFF;
  streamChunked = false;
  chunkLeft = 0;
  chunkNeedCRLF = false;
}

static int streamReadByte(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (true) {
    int n = streamClient.available();
    if (n > 0) return streamClient.read();
    if (!streamClient.connected()) return -1;
    if (millis() - t0 > timeoutMs) return -2;
    delay(1);
  }
}

static int streamBodyByte(uint32_t timeoutMs) {
  if (!streamChunked) return streamReadByte(timeoutMs);

  for (;;) {
    if (chunkLeft == 0) {
      if (chunkNeedCRLF) {
        int a = streamReadByte(timeoutMs);
        int b = streamReadByte(timeoutMs);
        if (a < 0 || b < 0) return -1;
        chunkNeedCRLF = false;
      }
      char line[24];
      int n = 0;
      for (;;) {
        int c = streamReadByte(timeoutMs);
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && n < (int)sizeof(line) - 1) line[n++] = (char)c;
      }
      line[n] = '\0';
      if (n == 0) continue;
      char *end = nullptr;
      unsigned long sz = strtoul(line, &end, 16);
      if (end == line) return -1;
      if (sz == 0) return -1;
      chunkLeft = (size_t)sz;
    }
    int b = streamReadByte(timeoutMs);
    if (b < 0) return -1;
    if (--chunkLeft == 0) chunkNeedCRLF = true;
    return b;
  }
}

static bool streamConnect() {
  streamStop();
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.printf("[CAM] Ket noi stream %s:%d ...\n", camIp, CAM_STREAM_PORT);
  IPAddress addr;
  if (!addr.fromString(camIp)) {
    Serial.printf("[CAM] IP khong hop le: %s\n", camIp);
    return false;
  }
  if (!streamClient.connect(addr, CAM_STREAM_PORT, 1500)) {
    Serial.println("[CAM] TCP connect that bai");
    return false;
  }
  streamClient.setNoDelay(true);
  streamClient.setTimeout(2000);

  streamClient.print("GET /stream HTTP/1.1\r\nHost: ");
  streamClient.print(camIp);
  streamClient.print(":");
  streamClient.print(CAM_STREAM_PORT);
  streamClient.print("\r\nUser-Agent: CYD-FaceSync\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n");

  char hdr[1024];
  size_t hn = 0;
  bool done = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (streamClient.available()) {
      char c = (char)streamClient.read();
      if (hn < sizeof(hdr) - 1) hdr[hn++] = c;
      if (hn >= 4 &&
          hdr[hn - 4] == '\r' && hdr[hn - 3] == '\n' &&
          hdr[hn - 2] == '\r' && hdr[hn - 1] == '\n') {
        done = true;
        break;
      }
    }
    if (done) break;
    if (!streamClient.connected()) break;
    delay(1);
  }
  hdr[hn] = '\0';

  if (!done || !strstr(hdr, " 200 ")) {
    Serial.printf("[CAM] Header loi (done=%d): %.80s\n", (int)done, hdr);
    streamClient.stop();
    return false;
  }

  streamChunked = (strstr(hdr, "chunked") != NULL) ||
                  (strstr(hdr, "Chunked") != NULL);
  chunkLeft = 0;
  chunkNeedCRLF = false;
  Serial.printf("[CAM] Stream OK (%s)\n", streamChunked ? "chunked" : "raw");
  return true;
}

static int readFrame(size_t *outLen) {
  size_t n = 0;
  bool in = false;
  bool overflow = false;
  int prev = -1;
  uint32_t t0 = millis();

  while (millis() - t0 < 5000) {
    int b = streamBodyByte(3000);
    if (b < 0) return -1;

    if (!in) {
      if (prev == 0xFF && b == 0xD8) {          // SOI
        in = true;
        n = 0;
        overflow = false;
        if (jpegCap >= 2) {
          jpegBuf[n++] = 0xFF;
          jpegBuf[n++] = 0xD8;
        } else {
          overflow = true;
        }
      }
      prev = b;
      continue;
    }

    if (prev == 0xFF && b == 0xD8) {          // SOI moi truoc EOI -> bo cu, bat dau lai
      n = 0;
      overflow = false;
      if (jpegCap >= 2) {
        jpegBuf[n++] = 0xFF;
        jpegBuf[n++] = 0xD8;
      } else {
        overflow = true;
      }
      prev = b;
      continue;
    }

    if (n < jpegCap) jpegBuf[n++] = (uint8_t)b;
    else overflow = true;

    if (prev == 0xFF && b == 0xD9) {          // EOI
      if (overflow) {
        Serial.printf("[CAM] Bo frame > %u bytes\n", (unsigned)jpegCap);
        return 0;
      }
      *outLen = n;
      return 1;
    }
    prev = b;
  }
  return -1;
}

// ============================== HTTP / RESULT ===============================
static void resultHttpClose() {
  if (resultHttpOpen) {
    resultHttp.end();
    resultHttpOpen = false;
  }
}

static bool pollResult() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (!resultHttpOpen) {
    resultHttp.setConnectTimeout(700);
    resultHttp.setTimeout(1100);
    resultHttp.setReuse(true);
    if (!resultHttp.begin(String(camIp), CAM_HTTP_PORT, "/result")) {
      return false;
    }
    resultHttpOpen = true;
  }

  int code = resultHttp.GET();
  if (code == 200) {
    String body = resultHttp.getString();
    if (body.length() > 0 && body.length() < 2048) {
      applyResult(body.c_str());
      return true;
    }
  } else if (code <= 0) {
    resultHttpClose();
  }
  return false;
}

// Helpers JSON ngan (tranh dang String de tiet kiem heap)
static int jsonIntField(const char *json, const char *key, int defVal) {
  char keybuf[32];
  snprintf(keybuf, sizeof(keybuf), "\"%s\":", key);
  const char *p = strstr(json, keybuf);
  if (!p) return defVal;
  p += strlen(keybuf);
  while (*p == ' ') p++;
  int sign = 1, val = 0;
  if (*p == '-') { sign = -1; p++; }
  while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
  return sign * val;
}

static void jsonStrField(const char *json, const char *key, char *out, size_t outSz) {
  out[0] = '\0';
  if (outSz == 0) return;
  char keybuf[32];
  snprintf(keybuf, sizeof(keybuf), "\"%s\":\"", key);
  const char *p = strstr(json, keybuf);
  if (!p) return;
  p += strlen(keybuf);
  size_t i = 0;
  while (*p && *p != '"' && i < outSz - 1) {
    out[i++] = *p++;
  }
  out[i] = '\0';
}

static void startScan(const char *why) {
  if (scanning) return;
  scanning = true;
  scanStartMs = millis();
  scanEndMs = 0;
  lastScanNotifyMs = scanStartMs;
  standbyShown = false;
  lastFrameValid = false;
  lastScanOverlayMs = 0;
  lastRetry = 0;
  Serial.printf("[CAM] Bat dau quet (%s) -> MO stream :81\n", why);
}

static void endScan(const char *why) {
  if (!scanning) return;
  scanning = false;
  scanEndMs = millis();
  Serial.printf("[CAM] Ket thuc quet (%s) -> DONG stream de ha nhiet\n", why);
  streamStop();
  ss = SS_OFF;
}

static void applyResult(const char *json) {
  bool scan = jsonIntField(json, "scanning", 0) != 0;
  if (scan) {
    lastScanNotifyMs = millis();
    if (!scanning) startScan("result");
  } else if (scanning) {
    // Bo qua ket qua /result cu (gui truoc khi bam nut). Chi ket thuc neu da
    // lau khong con tin hieu quet nao (UDP SCAN|0 se ket thuc ngay lap tuc).
    if (lastScanNotifyMs == 0 || millis() - lastScanNotifyMs > 1200) {
      endScan("result");
    }
  }

  int seq = jsonIntField(json, "seq", -1);
  if (seq >= 0) {
    if (haveSeq && (uint32_t)seq == lastSeq) return;
    bool first = !haveSeq;
    lastSeq = (uint32_t)seq;
    haveSeq = true;
    if (first) return;
  }

  char status[16] = "";
  jsonStrField(json, "status", status, sizeof(status));
  bool already = jsonIntField(json, "already", 0) != 0;

  if (!strcmp(status, "ok") || !strcmp(status, "already") || already) {
    char raw[64] = "", cls[24] = "", clsAscii[24] = "";
    jsonStrField(json, "full_name", raw, sizeof(raw));
    if (!raw[0]) jsonStrField(json, "name", raw, sizeof(raw));
    jsonStrField(json, "class", cls, sizeof(cls));

    // Chuyen tieng Viet co dau thanh khong dau de hien thi tren man hinh
    char resAscii[64] = "", clsA[24] = "";
    // vnToAscii neu co -> goi o day; hien tam bo qua de giam code, hien raw
    strncpy(resAscii, raw, sizeof(resAscii) - 1);
    resAscii[sizeof(resAscii) - 1] = '\0';
    strncpy(clsA, cls, sizeof(clsA) - 1);
    clsA[sizeof(clsA) - 1] = '\0';

    char sub[48];
    bool isAlready = already || !strcmp(status, "already");

    if (isAlready && bannerOn && bannerBg == C_OK_BG &&
        !strcmp(bannerName, resAscii)) {
      return;   // giu banner THANH CONG, khong ha cap
    }

    if (isAlready) {
      snprintf(sub, sizeof(sub), "DA DIEM DANH");
    } else if (clsA[0]) {
      snprintf(sub, sizeof(sub), "LOP %s - DIEM DANH OK", clsA);
    } else {
      snprintf(sub, sizeof(sub), "DIEM DANH OK");
    }
    setBanner(resAscii, sub,
              isAlready ? C_ALREADY_BG : C_OK_BG,
              isAlready ? TFT_CYAN : TFT_WHITE, 8000);
    Serial.printf("[RES] %s | %s\n", resAscii, sub);
  } else if (!strcmp(status, "nomatch")) {
    setBanner("KHONG NHAN RA", "THU LAI - GIU MAT TRONG KHUNG",
              C_ERR_BG, TFT_WHITE, 5000);
    Serial.println("[RES] Khong khop");
  } else if (!strcmp(status, "camerr")) {
    setBanner("LOI CAMERA", "KIEM TRA CAM / NGUON",
              C_ERR_BG, TFT_YELLOW, 5000);
    Serial.println("[RES] Loi camera");
  } else if (!strcmp(status, "neterr")) {
    setBanner("LOI MANG", "KIEM TRA SERVER / WIFI",
              C_ERR_BG, TFT_YELLOW, 5000);
    Serial.println("[RES] Loi mang/server");
  }
}

// ============================== UDP DISCOVERY ===============================
static bool isIp4Lan(const char *ip) {
  if (!ip || !*ip) return false;
  int o[4] = {0, 0, 0, 0};
  int n = 0, cur = 0, digits = 0;
  for (const char *q = ip; *q && n < 4; q++) {
    char c = *q;
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      digits++;
      if (cur > 255 || digits > 3) return false;
    } else if (c == '.') {
      if (digits == 0) return false;
      o[n++] = cur; cur = 0; digits = 0;
    } else break;
  }
  if (digits == 0) return false;
  o[n++] = cur;
  if (n != 4) return false;
  if (o[0] == 127 || o[0] == 0 || o[0] >= 224) return false;
  return (o[0] == 10) || (o[0] == 192 && o[1] == 168) ||
         (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

static void pollUdp() {
  if (!udpReady || WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < 8; i++) {
    int n = udp.parsePacket();
    if (n <= 0) break;
    char buf[128];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = '\0';

    const char *p = strstr(buf, DISC_MAGIC "|SCAN|");
    if (!p) continue;
    p += strlen(DISC_MAGIC) + 6;          // bo "FACESYNC|SCAN|"
    bool on = (*p == '1');

    IPAddress from = udp.remoteIP();
    char fip[16];
    snprintf(fip, sizeof(fip), "%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
    if (!isIp4Lan(fip)) continue;

    if (strcmp(fip, camIp) != 0) {
      strncpy(camIp, fip, sizeof(camIp) - 1);
      camIp[sizeof(camIp) - 1] = '\0';
      prefs.putString("camip", camIp);
      resultHttpClose();
      streamStop();
      Serial.printf("[DISC] SCAN tu IP moi -> cap nhat camera: %s\n", camIp);
    }

    lastScanNotifyMs = millis();
    if (on) {
      if (!scanning) startScan("udp");
    } else if (scanning) {
      endScan("udp");
    }
  }
}

static bool discoverCam(uint16_t waitMs) {
  if (!udpReady || WiFi.status() != WL_CONNECTED) return false;

  udp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
  udp.print(DISC_MAGIC "|CAM?");
  udp.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    int n = udp.parsePacket();
    if (n > 0) {
      char buf[128];
      int len = udp.read(buf, sizeof(buf) - 1);
      if (len <= 0) continue;
      buf[len] = '\0';
      const char *p = strstr(buf, DISC_MAGIC "|CAM|");
      if (!p) continue;
      p += strlen(DISC_MAGIC) + 5;
      char ip[16];
      size_t k = 0;
      while (*p && *p != '|' && *p != '\r' && *p != '\n' && k < sizeof(ip) - 1) {
        ip[k++] = *p++;
      }
      ip[k] = '\0';
      if (!isIp4Lan(ip)) continue;
      if (strcmp(ip, camIp) != 0) {
        strncpy(camIp, ip, sizeof(camIp) - 1);
        camIp[sizeof(camIp) - 1] = '\0';
        prefs.putString("camip", camIp);
        resultHttpClose();
        Serial.printf("[DISC] Tim thay ESP32-CAM: %s\n", camIp);
      }
      return true;
    }
    delay(10);
  }
  Serial.println("[DISC] Khong thay ESP32-CAM tra loi");
  return false;
}

// ============================== WIFI ========================================
static bool wifiConnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("cyd-face");
  Serial.printf("[WiFi] Ket noi '%s' ...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] That bai");
    return false;
  }
  Serial.printf("[WiFi] OK - IP %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// ============================== SERIAL / LENH ===============================
static char rxLine[48];
static size_t rxLen = 0;

static void printStats() {
  Serial.printf("[STATS] fw=%s ip=%s wifi=%d dBm heap=%u\n",
                FW_VERSION, camIp, (int)WiFi.RSSI(), ESP.getFreeHeap());
  Serial.printf("[STATS] stream=%d fps=%u frame=%u B decode=%u ms\n",
                (int)ss, (unsigned)fps, (unsigned)lastFrameBytes,
                (unsigned)lastDecodeMs);
  Serial.printf("[STATS] scanning=%d paused=%d jpegCap=%u\n",
                (int)scanning, (int)manualPaused, (unsigned)jpegCap);
}

static void handleSerialLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  if (!strcmp(line, "/help") || !strcmp(line, "help")) {
    Serial.println("Lenh:");
    Serial.println("  /ip                 xem IP camera");
    Serial.println("  /ip <ip>            dat IP camera (luu NVS)");
    Serial.println("  /find               tim camera qua UDP 4321");
    Serial.println("  /live on|off        bat/tat xem truc tiep");
    Serial.println("  /test               chay lai self-test man hinh");
    Serial.println("  /rot 0..7           doi rotation (mac dinh 2 = landscape 320x240)");
    Serial.println("  /touch              bat/tat in toa do cam ung");
    Serial.println("  /stats              thong so stream / heap / fps");
    Serial.println("  /reboot             khoi dong lai");
    return;
  }

  if (!strcmp(line, "/ip")) {
    Serial.printf("[CFG] IP cam: %s\n", camIp);
    return;
  }

  if (strncmp(line, "/ip ", 4) == 0) {
    char *ip = line + 4;
    while (*ip == ' ') ip++;
    if (isIp4Lan(ip)) {
      strncpy(camIp, ip, sizeof(camIp) - 1);
      camIp[sizeof(camIp) - 1] = '\0';
      prefs.putString("camip", camIp);
      resultHttpClose();
      streamStop();
      Serial.printf("[CFG] Dat IP cam: %s\n", camIp);
    } else {
      Serial.println("[CFG] IP khong hop le");
    }
    return;
  }

  if (!strcmp(line, "/find")) {
    Serial.println("[DISC] Tim camera qua UDP...");
    if (udpReady) discoverCam(1500);
    return;
  }

  if (!strcmp(line, "/test")) {
    Serial.println("[TFT] Chay self-test man hinh...");
    drawSelfTest();
    lastFrameValid = false;
    if (!scanning) {
      drawStandbyPage();
      standbyShown = true;
    }
    return;
  }

  if (strncmp(line, "/rot", 4) == 0 &&
      (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
    int r = atoi(line + 4);
    applyRotation((uint8_t)r);
    tft.fillScreen(TFT_BLACK);
    lastFrameValid = false;
    if (!scanning) {
      char b[40];
      snprintf(b, sizeof(b), "rot %u: %dx%d", (unsigned)g_rotation,
               tft.width(), tft.height());
      drawStatusPage("XOAY MAN HINH", b, TFT_CYAN);
    }
    return;
  }

  if (!strncmp(line, "/live", 5)) {
    char *arg = line + 5;
    while (*arg == ' ') arg++;
    if (!strcmp(arg, "on")) {
      manualPaused = false;
      idleDirty = true;
      standbyShown = false;
      lastRetry = 0;
      Serial.println("[UI] Bat lai xem truc tiep");
    } else if (!strcmp(arg, "off")) {
      manualPaused = true;
      streamStop();
      ss = SS_OFF;
      idleDirty = true;
      Serial.println("[UI] Tam dung xem truc tiep");
    } else {
      Serial.println("[UI] Dung: /live on|off");
    }
    return;
  }

  if (!strcmp(line, "/touch")) {
    Serial.println("[TOUCH] Debug cam ung da bat/tat");
    return;
  }

  if (!strcmp(line, "/stats")) {
    printStats();
    return;
  }

  if (!strcmp(line, "/reboot")) {
    Serial.println("[SYS] Reboot...");
    delay(100);
    ESP.restart();
    return;
  }

  Serial.printf("[LENH] Khong ro: %s\n", line);
}

static void serialPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxLine[rxLen] = '\0';
        handleSerialLine(rxLine);
        rxLen = 0;
      }
    } else if (rxLen < sizeof(rxLine) - 1) {
      rxLine[rxLen++] = c;
    }
  }
}

static void touchPoll() {
  static uint32_t lastTap = 0;
  if (!touch.touched()) return;

  uint16_t rx, ry;
  uint8_t rz;
  touch.readData(&rx, &ry, &rz);
  if (rz == 0) return;

  uint16_t mx = rx;
  uint16_t my = ry;
#if TOUCH_SWAP_XY
  uint16_t tmp = mx; mx = my; my = tmp;
#endif
  long x = map((long)mx, TOUCH_X_MIN, TOUCH_X_MAX, 0, LCD_W - 1);
  long y = map((long)my, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, LCD_H - 1);
#if TOUCH_INVERT_X
  x = (LCD_W - 1) - x;
#endif
#if TOUCH_INVERT_Y
  y = (LCD_H - 1) - y;
#endif
  x = constrain(x, 0, LCD_W - 1);
  y = constrain(y, 0, LCD_H - 1);

  uint32_t now = millis();
  if (now - lastTap < 350) return;

  // Nut DUNG/XEM o goc tren phai
  if (x >= 240 && y <= 30) {
    lastTap = now;
    manualPaused = !manualPaused;
    idleDirty = true;
    if (manualPaused) {
      streamStop();
      ss = SS_OFF;
      Serial.println("[UI] Tam dung xem truc tiep");
    } else {
      lastRetry = 0;
      standbyShown = false;
      Serial.println("[UI] Bat lai xem truc tiep");
    }
    if (manualPaused) drawIdlePage();
    return;
  }
  lastTap = now;
}

// ============================== SETUP =======================================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== CYD ESP32-2432S028R - MAN HINH CAN MAT ===");
  Serial.printf("=== Firmware %s ===\n", FW_VERSION);

  // Cap bo dem JPEG
  jpegBuf = (uint8_t *)heap_caps_malloc(112 * 1024,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpegBuf) jpegBuf = (uint8_t *)malloc(112 * 1024);
  jpegCap = jpegBuf ? 112 * 1024 : 0;
  Serial.printf("[RAM] JPEG buffer: %u bytes\n", (unsigned)jpegCap);

  // Backlight truoc khi init de tranh nhay trang
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setSwapBytes(true);                  // JPEGDEC xuat RGB565 little-endian
  applyRotation(2);                        // mac dinh /rot 2 (landscape 320x240)

  // Kiem tra THAT sau khi xoay: phai landscape (rong > cao). Neu khong,
  // thu 0..3 va chon cai landscape dau tien.
  if (tft.width() <= tft.height()) {
    Serial.println("[TFT] CANH BAO: rotation 2 khong ra landscape -> thu 0..3");
    for (uint8_t r = 0; r < 4; r++) {
      tft.setRotation(r);
      Serial.printf("[TFT]   thu rot %u: %dx%d\n", (unsigned)r,
                    tft.width(), tft.height());
      if (tft.width() > tft.height()) { g_rotation = r; break; }
    }
    tft.setRotation(g_rotation);
    Serial.printf("[TFT] Chon rotation %u -> %dx%d\n",
                  (unsigned)g_rotation, tft.width(), tft.height());
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  Serial.printf("[TFT] driver ILI9341 native %dx%d -> rotation%u %dx%d (UI %dx%d)\n",
                TFT_WIDTH, TFT_HEIGHT, (unsigned)g_rotation,
                tft.width(), tft.height(), LCD_W, LCD_H);
  if (tft.width() != LCD_W || tft.height() != LCD_H) {
    Serial.println("[TFT] LOI: kich thuoc sau xoay KHAC 320x240 -> man hinh se bi cat!");
  }

#if CYD_SELFTEST
  Serial.println("[TFT] Self-test panel (4 mau + vien + 4 goc)...");
  drawSelfTest();
  tft.fillScreen(TFT_BLACK);
#endif

  // Cam ung tren bus SPI rieng
  touchSPI.begin(TOUCH_SCLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSPI);
  touch.setRotation(1);
  Serial.println("[TOUCH] XPT2046 san sang");

  drawSplash("Dang khoi dong...");

  // NVS: lay IP camera da luu
  prefs.begin("cyd", false);
  String saved = prefs.getString("camip", "");
  if (saved.length() && isIp4Lan(saved.c_str())) {
    strncpy(camIp, saved.c_str(), sizeof(camIp) - 1);
    camIp[sizeof(camIp) - 1] = '\0';
  }
  Serial.printf("[CFG] IP cam: %s (mac dinh %s)\n", camIp, CAM_IP_DEFAULT);

  drawSplash("Dang ket noi WiFi...");
  if (!wifiConnect(20000)) {
    wifiDown = true;
    drawStatusPage("LOI WIFI", "Kiem tra SSID / mat khau", TFT_RED);
  } else {
    udpReady = (udp.begin(DISC_PORT) == 1);
    if (!udpReady) Serial.println("[DISC] Khong mo duoc UDP 4321");

    // Thu tim camera neu IP mac dinh khong dung
    if (!pollResult()) {
      Serial.println("[CAM] Chua doc duoc /result -> discovery");
      if (udpReady) discoverCam(800);
    }

    if (scanning) {
      drawStatusPage("DANG QUET", "Dang lay hinh tu camera...", TFT_ORANGE);
    } else {
      drawStandbyPage();   // nghi: khong truyen anh
      standbyShown = true;
    }
  }

  lastResultPoll = millis();
  lastFpsMs = millis();
  lastStatsMs = millis();
  lastRetry = 0;
  lastDiscovery = millis();
  Serial.println("[SYS] San sang. Go /help de xem lenh.");
}

// ============================== LOOP ========================================
void loop() {
  uint32_t now = millis();
  serialPoll();
  touchPoll();

  // ---------------- WiFi ----------------
  if (WiFi.status() != WL_CONNECTED) {
    if (ss != SS_OFF) streamStop();
    if (!wifiDown) {
      wifiDown = true;
      resultHttpClose();
      drawStatusPage("MAT WIFI", "Dang ket noi lai...", TFT_RED);
      Serial.println("[WiFi] Mat ket noi");
    }
    if (now - lastWifiTry > 8000) {
      lastWifiTry = now;
      Serial.println("[WiFi] Thu ket noi lai...");
      if (!wifiConnect(12000)) WiFi.disconnect();
    }
    delay(20);
    return;
  }
  if (wifiDown) {
    wifiDown = false;
    resultHttpClose();
    lastRetry = 0;
    lastFrameValid = false;
    Serial.println("[WiFi] Da ket noi lai");
    if (scanning) {
      drawStatusPage("DANG QUET", "Dang lay hinh tu camera...", TFT_ORANGE);
    } else {
      drawStandbyPage();
      standbyShown = true;
    }
  }

  // ---------------- Thong bao quet tu ESP32-CAM (UDP) ----------------
  pollUdp();

  // ---------------- Doc ket qua nhan dien ----------------
  if (now - lastResultPoll >= RESULT_POLL_MS) {
    lastResultPoll = now;
    pollResult();
  }

  // ---------------- Fail-safe: mat tin hieu "dang quet" ----------------
  if (scanning) {
    uint32_t last = lastScanNotifyMs ? lastScanNotifyMs : scanStartMs;
    if ((now - last > SCAN_NOTIFY_MS) || (now - scanStartMs > SCAN_MAX_MS)) {
      endScan("timeout");
    }
  }

  // ---------------- Stream / decode ----------------
  // CHI truyen anh khi camera dang quet (bam nut). Khi nghI dong stream de
  // ESP32-CAM khong chup lien tuc -> giam nhiet, tiet kiem dien.
  bool wantStream = scanning && !manualPaused;

  if (!wantStream) {
    if (ss == SS_LIVE || ss == SS_CONNECTING) {
      streamStop();
      ss = SS_OFF;
    }
    if (manualPaused) {
      if (idleDirty) {
        drawIdlePage();
        idleDirty = false;
      }
    } else {
      // Vua quet xong: giu khung cuoi + ket qua trong RESULT_HOLD_MS,
      // sau do ve trang cho. Banner duoc ve lai de len khung dong bang.
      bool holding = scanEndMs && (now - scanEndMs < RESULT_HOLD_MS);
      bool bannerActive = bannerOn && (int32_t)(now - bannerUntil) < 0;
      if (bannerActive) {
        if (now - lastScanOverlayMs > 250) {
          lastScanOverlayMs = now;
          drawOverlay();
        }
      } else if (!holding && !standbyShown) {
        bannerOn = false;
        drawStandbyPage();
        standbyShown = true;
        lastFrameValid = false;
      }
    }
  } else {
    if (ss == SS_LIVE) {
      size_t len = 0;
      int r = readFrame(&len);
      if (r == 1) {
        lastFrameBytes = len;
        if (showJpeg(len)) {
          framesThisSec++;
          lastFrameValid = true;
          drawOverlay();
        }
      } else if (r < 0) {
        Serial.println("[CAM] Stream loi -> ket noi lai");
        streamStop();
        ss = SS_RETRY;
        lastRetry = now;
        if (lastFrameValid) drawConnBox("MAT KET NOI", "Dang thu lai...");
        else drawStatusPage("DANG QUET", "Dang lay hinh tu camera...", TFT_ORANGE);
      }
      // r == 0: frame qua lon, da bo -> doc tiep frame sau
    } else if (ss == SS_RETRY || ss == SS_OFF) {
      if (now - lastRetry >= STREAM_RETRY_MS) {
        lastRetry = now;
        ss = SS_CONNECTING;
        if (!lastFrameValid) {
          drawStatusPage("DANG QUET", "Dang lay hinh tu camera...", TFT_ORANGE);
        }
        if (streamConnect()) {
          ss = SS_LIVE;
          lastDiscovery = now;
        } else {
          ss = SS_RETRY;
          if (!lastFrameValid) {
            drawStatusPage("KHONG THAY CAMERA", "Kiem tra nguon / WiFi cua cam", TFT_RED);
          }
          if (now - lastDiscovery > 12000) {
            lastDiscovery = now;
            if (udpReady) discoverCam(700);
          }
        }
      }
    }
  }

  // ---------------- FPS ----------------
  if (now - lastFpsMs >= 1000) {
    fps = framesThisSec;
    framesThisSec = 0;
    lastFpsMs = now;
  }

  // ---------------- Log dinh ky ----------------
  if (now - lastStatsMs >= 15000) {
    lastStatsMs = now;
    Serial.printf("[STATS] %u fps, frame %u B, decode %u ms, heap %u, ss=%d\n",
                  (unsigned)fps, (unsigned)lastFrameBytes,
                  (unsigned)lastDecodeMs, ESP.getFreeHeap(), (int)ss);
  }

  delay(1);
}

// Ham duoc goi o tren (drawOverlay su dung drawConnBox) - dinh nghia o day
static void drawConnBox(const char *title, const char *sub) {
  tft.fillRoundRect(46, 92, 228, 62, 10, TFT_BLACK);
  tft.drawRoundRect(46, 92, 228, 62, 10, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(title, 160, 112, 2);
  tft.setTextFont(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(sub, 160, 137, 1);
}
