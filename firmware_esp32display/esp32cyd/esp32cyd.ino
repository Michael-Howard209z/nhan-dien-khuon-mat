/* ===========================================================================
 *  ESP32-2432S028R (CYD) - MAN HINH CAN MAT + KET QUA DIEM DANH
 * ===========================================================================
 *
 *  Vai tro:
 *    1. Hien thi anh LIVE tu ESP32-CAM (MJPEG http://<cam>:81/stream)
 *       de hoc sinh nhin thay minh va can mat vao khung.
 *    2. Doc ket qua nhan dien tu ESP32-CAM (GET http://<cam>:80/result)
 *       va hien thi ten / trang thai diem danh.
 *    3. TU DONG TAM DUNG stream khi ESP32-CAM dang quet 15 giay
 *       (scanning=1). Ly do: camera chi co fb_count=1, mot client stream
 *       lien tuc co the lay het frame khien dot quet khong lay duoc anh.
 *       Khi tam dung, man hinh giu khung hinh cuoi + hien "DANG QUET".
 *
 *  Phan cung (xem tft_setup.h):
 *    - ILI9341 320x240 SPI (HSPI): MOSI=13, MISO=12, SCLK=14,
 *      CS=15, DC=2, RST=-1, BL=21
 *    - XPT2046 (VSPI): CS=33, IRQ=36, CLK=25, MOSI=32, MISO=39
 *
 *  Thu vien can cai:
 *    - TFT_eSPI (Bodmer)
 *    - XPT2046_Touchscreen (Paul Stoffregen)
 *    - JPEGDEC (Larry Bank)   <-- bat buoc, khong co trong core ESP32
 *
 *  Lenh Serial 115200:
 *    /help                 - danh sach lenh
 *    /ip                   - xem IP camera
 *    /ip 192.168.0.101     - dat IP camera (luu NVS)
 *    /find                 - tim camera qua UDP broadcast 4321
 *    /live                 - bat/tat che do xem truc tiep
 *    /live on | /live off
 *    /stats                - thong so stream / heap / fps
 *    /touch                - bat/tat in toa do cam ung (hieu chuan)
 *    /reboot
 * =========================================================================== */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <FS.h>                // JPEGDEC.h dung kieu File -> phai include truoc
#include <TFT_eSPI.h>          // tft_setup.h trong sketch duoc tu dong include
#include <XPT2046_Touchscreen.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>

// ============================== CAU HINH ====================================
#define FW_VERSION        "2026-10-28-cyd1"

#define WIFI_SSID         "Test"
#define WIFI_PASS         "88888888"

#define CAM_IP_DEFAULT    "192.168.0.101"
#define CAM_HTTP_PORT     80
#define CAM_STREAM_PORT   81

#define RESULT_POLL_MS    700       // doc /result (du de bat scanning som)
#define STREAM_RETRY_MS   2500      // thu lai stream sau loi
#define RESULT_RETRY_MS   1200
#define SCAN_RESUME_MS    1500      // nghi 1.5s sau khi quet xong moi stream lai
#define SCAN_MAX_MS       20000     // fail-safe: khong de "scanning" treo mai

#define DISC_PORT         4321
#define DISC_MAGIC        "FACESYNC"

// Cam ung XPT2046 - CYD
#define TOUCH_CS_PIN      33
#define TOUCH_IRQ_PIN     36
#define TOUCH_SCLK_PIN    25
#define TOUCH_MISO_PIN    39
#define TOUCH_MOSI_PIN    32

// Vung gia tri RAW cua XPT2046 tren CYD (hieu chuan bang lenh /touch)
#define TOUCH_X_MIN       1500
#define TOUCH_X_MAX       3000
#define TOUCH_Y_MIN       1400
#define TOUCH_Y_MAX       2900

// Chinh huong truc cam ung. CYD co nhieu bien the day cam ung khac nhau:
//   - TOUCH_SWAP_XY  1: doi vai tro X/Y (cam ung bi xoay 90 do so voi man hinh)
//   - TOUCH_INVERT_X 1: dao dau truc X
//   - TOUCH_INVERT_Y 1: dao dau truc Y
// Dung /touch de xem RAW va toa do da map khi cham 4 goc, roi chon to hop dung.
#define TOUCH_SWAP_XY     0
#define TOUCH_INVERT_X    0
#define TOUCH_INVERT_Y    0

#define LCD_W             320
#define LCD_H             240

// Mau rieng
#define C_BG              0x0841   // xanh den rat dam
#define C_HEADER          0x0010   // navy dam
#define C_OK_BG           0x0A64   // xanh la dam
#define C_ALREADY_BG      0x0010   // navy
#define C_ERR_BG          0x7800   // maroon
#define C_TEXT_DIM        0xA514
#define C_GUIDE           0xDEDB

// ============================== DOI TUONG ===================================
TFT_eSPI tft = TFT_eSPI();

#if CYD_DISPLAY_ON_VSPI
  SPIClass touchSPI(HSPI);     // man hinh da chiem VSPI -> cam ung dung HSPI
#else
  SPIClass touchSPI(VSPI);     // man hinh HSPI -> cam ung dung VSPI
#endif
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

JPEGDEC jpeg;
WiFiClient streamClient;
WiFiUDP udp;
Preferences prefs;
HTTPClient resultHttp;

// ============================== BIEN ========================================
enum StreamState { SS_OFF, SS_CONNECTING, SS_LIVE, SS_RETRY };
static StreamState ss = SS_RETRY;

static char camIp[16] = CAM_IP_DEFAULT;
static bool udpReady = false;
static bool resultHttpOpen = false;

static bool manualPaused = false;    // nguoi dung bam DUNG
static bool scanning = false;        // ESP32-CAM dang quet 15s
static bool scanHold = false;        // dang tam dung stream de nhường frame
static uint32_t scanStartMs = 0;
static uint32_t scanEndMs = 0;

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

// Banner
static bool bannerOn = false;
static uint32_t bannerUntil = 0;
static char bannerName[48] = "";
static char bannerSub[48] = "";
static uint16_t bannerBg = C_OK_BG;
static uint16_t bannerFg = TFT_WHITE;

static bool touchDebug = false;

// ============================== TIEN ICH CHUOI ==============================
static void lcdClearCacheUnused() {}

static int utf8Next(const char *s, unsigned *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0) {
    if (((unsigned char)s[1] & 0xC0) != 0x80) { *cp = c; return 1; }
    *cp = ((unsigned)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    if (((unsigned char)s[1] & 0xC0) != 0x80 ||
        ((unsigned char)s[2] & 0xC0) != 0x80) { *cp = c; return 1; }
    *cp = ((unsigned)(c & 0x0F) << 12) |
          ((unsigned)((unsigned char)s[1] & 0x3F) << 6) |
          ((unsigned char)s[2] & 0x3F);
    return 3;
  }
  *cp = c;
  return 1;
}

static char asciiOf(unsigned cp) {
  if (cp < 0x80) return (char)cp;
  if (cp >= 0x0300 && cp <= 0x036F) return 0;   // dau to hop -> bo
  // Vietnamese Extended Additional U+1EA0..U+1EF9
  if (cp >= 0x1EA0 && cp <= 0x1EF9) {
    char base;
    if (cp <= 0x1EB7) base = 'A';
    else if (cp <= 0x1EC7) base = 'E';
    else if (cp <= 0x1ECB) base = 'I';
    else if (cp <= 0x1EE3) base = 'O';
    else if (cp <= 0x1EF1) base = 'U';
    else base = 'Y';
    return (cp & 1) ? (char)(base + 32) : base;
  }
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
  if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
  if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
  if (cp >= 0x00D2 && cp <= 0x00D6) return 'O';
  if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
  if (cp == 0x00DD) return 'Y';
  if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
  if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
  if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
  if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';
  if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
  if (cp == 0x00FD || cp == 0x00FF) return 'y';
  switch (cp) {
    case 0x0102: return 'A'; case 0x0103: return 'a';
    case 0x0110: return 'D'; case 0x0111: return 'd';
    case 0x00D0: return 'D'; case 0x00F0: return 'd';
    case 0x0128: return 'I'; case 0x0129: return 'i';
    case 0x0168: return 'U'; case 0x0169: return 'u';
    case 0x01A0: return 'O'; case 0x01A1: return 'o';
    case 0x01AF: return 'U'; case 0x01B0: return 'u';
    default: return '?';
  }
}

static void vnToAscii(const char *src, char *dst, size_t dstSz) {
  if (!dst || dstSz == 0) return;
  size_t k = 0;
  const char *p = src ? src : "";
  while (*p && k + 1 < dstSz) {
    unsigned cp;
    p += utf8Next(p, &cp);
    char a = asciiOf(cp);
    if (a == 0) continue;
    if (k && dst[k - 1] == ' ' && a == ' ') continue;
    dst[k++] = a;
  }
  dst[k] = '\0';
  size_t start = 0;
  while (dst[start] == ' ') start++;
  if (start > 0) memmove(dst, dst + start, k - start + 1);
  size_t len = strlen(dst);
  while (len > 0 && dst[len - 1] == ' ') { dst[len - 1] = '\0'; len--; }
}

// ============================== JSON (strstr) ===============================
static bool jsonStrField(const char *json, const char *key, char *out, size_t outSz) {
  if (out && outSz) out[0] = '\0';
  if (!json || !key || !out || !outSz) return false;
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t k = 0;
  while (*p && *p != '"' && k + 1 < outSz) {
    if (*p == '\\' && (*(p + 1) == '"' || *(p + 1) == '\\')) p++;
    out[k++] = *p++;
  }
  out[k] = '\0';
  return true;
}

static int jsonIntField(const char *json, const char *key, int def) {
  if (!json || !key) return def;
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p) return def;
  return atoi(p + strlen(pat));
}

static bool isIp4Lan(const char *s) {
  if (!s || !*s) return false;
  int o[4] = {0, 0, 0, 0}, nOct = 0, cur = 0, digits = 0;
  for (const char *q = s; *q && nOct < 4; q++) {
    if (*q >= '0' && *q <= '9') {
      cur = cur * 10 + (*q - '0');
      if (++digits > 3 || cur > 255) return false;
    } else if (*q == '.') {
      if (!digits) return false;
      o[nOct++] = cur; cur = 0; digits = 0;
    } else break;
  }
  if (!digits) return false;
  o[nOct++] = cur;
  if (nOct != 4) return false;
  if (o[0] == 127 || o[0] == 0 || o[0] >= 224) return false;
  return (o[0] == 10) || (o[0] == 192 && o[1] == 168) ||
         (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

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

static void drawTopBar() {
  uint16_t bg = TFT_BLACK;
  tft.fillRect(0, 0, LCD_W, 24, bg);

  uint16_t dot = TFT_RED;
  const char *state = "OFF";
  if (manualPaused) { dot = TFT_LIGHTGREY; state = "TAM DUNG"; }
  else if (scanHold) { dot = TFT_ORANGE; state = "DANG QUET"; }
  else if (ss == SS_LIVE) { dot = TFT_GREEN; state = "LIVE"; }
  else { dot = TFT_YELLOW; state = "KET NOI"; }

  tft.fillCircle(9, 12, 4, dot);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(state, 18, 4, 2);

  if (scanHold) {
    tft.setTextColor(TFT_ORANGE, bg);
    tft.drawString("GIU MAT YEN", 82, 4, 2);
  } else {
    tft.setTextColor(C_TEXT_DIM, bg);
    tft.drawString(camIp, 82, 4, 2);
    char b[16];
    snprintf(b, sizeof(b), "%2u fps", (unsigned)fps);
    tft.setTextColor(TFT_CYAN, bg);
    tft.drawString(b, 188, 4, 2);
  }

  drawPauseButton(248, 2);
}

static void drawGuide(bool showText) {
  uint16_t c = scanHold ? TFT_ORANGE : C_GUIDE;
  int x0 = 66, y0 = 38, x1 = 254, y1 = 202, L = 24;
  // 2px day
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
  if (scanHold) {
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

  bool showBanner = bannerOn && (scanHold ||
                    (int32_t)(millis() - bannerUntil) < 0);
  if (showBanner) drawBanner();
  else if (!scanHold) bannerOn = false;

  if (scanHold) {
    drawProgress();
    // progress bar nam tren dong chu huong dan -> ve lai nen
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

static void drawIdlePage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, LCD_W, 30, C_HEADER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, C_HEADER);
  tft.drawString("MAN HINH CAN MAT", 10, 7, 2);
  drawPauseButton(248, 5);

  // Bieu tuong camera
  uint16_t ic = TFT_CYAN;
  tft.drawRoundRect(105, 66, 110, 58, 10, ic);
  tft.drawRoundRect(106, 67, 108, 56, 9, ic);
  tft.drawCircle(160, 95, 20, ic);
  tft.drawCircle(160, 95, 11, ic);
  tft.fillRect(135, 58, 24, 9, ic);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("CAMERA DANG TAT", 160, 156, 4);

  tft.setTextFont(2);
  tft.setTextColor(C_TEXT_DIM, C_BG);
  tft.drawString("Cham goc tren phai de bat camera", 160, 188, 2);

  tft.setTextFont(1);
  tft.setTextColor(TFT_DARKGREY, C_BG);
  char b[64];
  snprintf(b, sizeof(b), "Cam: %s   WiFi: %d dBm", camIp, (int)WiFi.RSSI());
  tft.drawString(b, 160, 220, 1);

  if (bannerOn) drawBanner();
}

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
  if (manualPaused) idleDirty = true;
}

// ============================== KET QUA =====================================
static void applyResult(const char *json) {
  bool scan = jsonIntField(json, "scanning", 0) != 0;
  if (scan && !scanning) {
    scanning = true;
    scanStartMs = millis();
    scanEndMs = 0;
    Serial.println("[CAM] Bat dau quet 15s -> tam dung stream");
  } else if (!scan && scanning) {
    scanning = false;
    scanEndMs = millis();
    Serial.println("[CAM] Ket thuc quet -> stream lai sau 1.5s");
  }

  int seq = jsonIntField(json, "seq", -1);
  if (seq >= 0) {
    if (haveSeq && (uint32_t)seq == lastSeq) return;
    // Lan doc dau tien sau khi boot: chi ghi nho seq, KHONG hien ket qua cu
    // (tranh man hinh hien ten cua dot diem danh truoc do).
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
    vnToAscii(raw, resNameAscii, sizeof(resNameAscii));
    vnToAscii(cls, clsAscii, sizeof(clsAscii));

    char sub[48];
    bool isAlready = already || !strcmp(status, "already");

    // Trong cung 1 dot quet, khung dau tra "ok" roi cac khung sau tra
    // "already" cho cung nguoi -> giu nguyen banner THANH CONG, khong ha cap.
    if (isAlready && bannerOn && bannerBg == C_OK_BG &&
        !strcmp(bannerName, resNameAscii)) {
      return;
    }

    if (isAlready) {
      snprintf(sub, sizeof(sub), "DA DIEM DANH");
    } else if (clsAscii[0]) {
      snprintf(sub, sizeof(sub), "LOP %s - DIEM DANH OK", clsAscii);
    } else {
      snprintf(sub, sizeof(sub), "DIEM DANH OK");
    }
    setBanner(resNameAscii, sub,
              isAlready ? C_ALREADY_BG : C_OK_BG,
              isAlready ? TFT_CYAN : TFT_WHITE, 8000);
    Serial.printf("[RES] %s | %s\n", resNameAscii, sub);
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

// ============================== KET NOI =====================================
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

// Doc 1 byte cua body HTTP (da bo chunk framing neu co)
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
      if (n == 0) continue;   // bo dong trang
      char *end = nullptr;
      unsigned long sz = strtoul(line, &end, 16);
      if (end == line) return -1;
      if (sz == 0) return -1;  // ket thuc stream
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
  if (!streamClient.connect(camIp, CAM_STREAM_PORT, 2500)) {
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

// 1 = co frame, 0 = frame qua lon (bo), -1 = loi stream
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

    if (n < jpegCap) jpegBuf[n++] = (uint8_t)b;
    else overflow = true;

    if (prev == 0xFF && b == 0xD9) {            // EOI
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

// ============================== JPEG DECODE ================================
static int16_t drawOffX = 0;
static int16_t drawOffY = 0;

static int drawMCUs(JPEGDRAW *pDraw) {
  int32_t x = pDraw->x + drawOffX;
  int32_t y = pDraw->y + drawOffY;
  int32_t stride = pDraw->iWidth;                  // so pixel that moi hang trong buffer
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
    // Block bi cat o mep phai (iWidthUsed < iWidth): buffer van co stride =
    // iWidth nen phai ve tung hang, khong the push ca khoi lien mach.
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

  // LOG chuan doan: xem chinh xac kich thuc JPEG camera gui ve
  Serial.printf("[JPEG] %dx%d -> scale=%d out=%dx%d off=%d,%d\n",
                w, h, opt, outW, outH, drawOffX, drawOffY);

  // Neu anh nho hon man hinh (320x240), to den vung con lai de khong con
  // hien anh cu o phia sau -> khac biet "hien mot phan" vs "full man hinh".
  if (outW < LCD_W || outH < LCD_H) {
    tft.startWrite();
    tft.fillRect(drawOffX, drawOffY, outW, outH, TFT_BLACK);
    tft.endWrite();
  }

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

  // Toan bo bo decode duoc chap trong MOT transaction SPI de giam overhead
  // mo/dong bus cho tung block MCU -> tang FPS.
  tft.startWrite();
  uint32_t t0 = micros();
  int ok = jpeg.decode(0, 0, opt);
  lastDecodeMs = (micros() - t0) / 1000;
  tft.endWrite();
  jpeg.close();

  if (!ok) {
    Serial.println("[JPEG] decode that bai");
    return false;
  }
  return true;
}

// ============================== HTTP / RESULT ==============================
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

// ============================== DISCOVERY ==================================
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

// ============================== WIFI ======================================
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
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

// ============================== TOUCH ======================================
static void touchPoll() {
  static uint32_t lastTap = 0;
  if (!touch.touched()) return;

  uint16_t rx, ry;
  uint8_t rz;
  touch.readData(&rx, &ry, &rz);
  if (rz == 0) return;

  // setRotation(1) cua XPT2046 da tra raw theo truc vat ly, nen o day chi
  // con doi truc / dao dau theo tung bien the board (xem TOUCH_* o dau file).
  uint16_t mx = rx;
  uint16_t my = ry;
#if TOUCH_SWAP_XY
  uint16_t tmp = mx;
  mx = my;
  my = tmp;
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

  if (touchDebug) {
    Serial.printf("[TOUCH] raw X=%u Y=%u Z=%u -> %ld,%ld\n",
                  rx, ry, rz, x, y);
  }

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
      Serial.println("[UI] Bat lai xem truc tiep");
    }
    if (manualPaused) drawIdlePage();
    return;
  }
  lastTap = now;
}

// ============================== SERIAL =====================================
static char rxLine[48];
static size_t rxLen = 0;

static void printStats() {
  Serial.printf("[STATS] fw=%s ip=%s wifi=%d dBm heap=%u maxblk=%u\n",
                FW_VERSION, camIp, (int)WiFi.RSSI(), ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.printf("[STATS] stream=%d chunked=%d fps=%u frame=%u B decode=%u ms\n",
                (int)ss, (int)streamChunked, (unsigned)fps,
                (unsigned)lastFrameBytes, (unsigned)lastDecodeMs);
  Serial.printf("[STATS] scanning=%d scanHold=%d paused=%d jpegCap=%u\n",
                (int)scanning, (int)scanHold, (int)manualPaused,
                (unsigned)jpegCap);
}

static void handleSerialLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  if (!strcmp(line, "/help") || !strcmp(line, "help")) {
    Serial.println("Lenh:");
    Serial.println("  /ip                 xem IP camera");
    Serial.println("  /ip 192.168.0.101   dat IP camera (luu NVS)");
    Serial.println("  /find               tim ESP32-CAM qua UDP 4321");
    Serial.println("  /live [on|off]      bat/tat xem truc tiep");
    Serial.println("  /stats              thong so");
    Serial.println("  /touch              bat/tat debug toa do cam ung");
    Serial.println("  /reboot");
    return;
  }
  if (!strcmp(line, "/find")) {
    Serial.println("[DISC] Dang tim ESP32-CAM...");
    if (discoverCam(1200)) Serial.printf("[DISC] cam: %s\n", camIp);
    return;
  }
  if (!strcmp(line, "/stats")) { printStats(); return; }
  if (!strcmp(line, "/touch")) {
    touchDebug = !touchDebug;
    Serial.printf("[TOUCH] Debug %s\n", touchDebug ? "ON" : "OFF");
    return;
  }
  if (!strcmp(line, "/reboot")) {
    Serial.println("[SYS] Reboot...");
    delay(200);
    ESP.restart();
  }
  if (!strncmp(line, "/live", 5)) {
    const char *arg = line + 5;
    while (*arg == ' ') arg++;
    if (!*arg) manualPaused = !manualPaused;
    else if (!strcmp(arg, "on")) manualPaused = false;
    else if (!strcmp(arg, "off")) manualPaused = true;
    else { Serial.println("[CFG] Dung: /live on | /live off"); return; }
    idleDirty = true;
    if (manualPaused) { streamStop(); ss = SS_OFF; }
    else lastRetry = 0;
    Serial.printf("[UI] Xem truc tiep: %s\n", manualPaused ? "OFF" : "ON");
    return;
  }
  if (!strncmp(line, "/ip", 3)) {
    const char *arg = line + 3;
    while (*arg == ' ') arg++;
    if (!*arg) {
      Serial.printf("[CFG] IP cam: %s, stream %s\n", camIp,
                    ss == SS_LIVE ? "LIVE" : "OFF");
      return;
    }
    if (!isIp4Lan(arg)) {
      Serial.println("[CFG] IP khong hop le (chi 10.x / 172.16-31.x / 192.168.x)");
      return;
    }
    strncpy(camIp, arg, sizeof(camIp) - 1);
    camIp[sizeof(camIp) - 1] = '\0';
    prefs.putString("camip", camIp);
    resultHttpClose();
    streamStop();
    ss = SS_RETRY;
    lastRetry = 0;
    lastFrameValid = false;
    Serial.printf("[CFG] Da luu IP cam: %s\n", camIp);
    return;
  }
  Serial.printf("[CFG] Khong hieu '%s' - go /help\n", line);
}

static void serialPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxLine[rxLen] = '\0';
      handleSerialLine(rxLine);
      rxLen = 0;
      continue;
    }
    if (rxLen < sizeof(rxLine) - 1) rxLine[rxLen++] = c;
  }
}

// ============================== CAP PHAT RAM ==============================
static bool allocJpegBuffer() {
  const size_t caps[] = {
    112 * 1024, 96 * 1024, 80 * 1024,
    64 * 1024, 48 * 1024, 32 * 1024
  };
  for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
    jpegBuf = (uint8_t *)malloc(caps[i]);
    if (jpegBuf) {
      jpegCap = caps[i];
      Serial.printf("[RAM] JPEG buffer: %u bytes (heap %u, maxblk %u)\n",
                    (unsigned)jpegCap, ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      return true;
    }
    Serial.printf("[RAM] Khong cap duoc %u bytes, thu nho...\n",
                  (unsigned)caps[i]);
  }
  jpegCap = 0;
  Serial.println("[RAM] THAT BAI: khong cap duoc bo dem JPEG");
  return false;
}

// ============================== SETUP ======================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== CYD ESP32-2432S028R - MAN HINH CAN MAT ===");
  Serial.printf("=== Firmware %s ===\n", FW_VERSION);

  allocJpegBuffer();

  // Backlight truoc khi init de tranh nhay trang
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(1);                      // landscape 320x240
  tft.setSwapBytes(true);                  // JPEGDEC xuat uint16 RGB565 chuan (little-endian)
  tft.fillScreen(TFT_BLACK);               // dam bao toan bo man hinh duoc xoa mau den
  tft.setTextSize(1);
  Serial.printf("[TFT] init %dx%d -> rotation1 width=%d height=%d\n",
                TFT_WIDTH, TFT_HEIGHT, tft.width(), tft.height());

  // Cam ung tren bus SPI rieng
  touchSPI.begin(TOUCH_SCLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSPI);
  touch.setRotation(1);
  Serial.println("[TOUCH] XPT2046 san sang");

  drawSplash("Dang khoi dong...");

  // NVS
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
    drawStatusPage("DANG KET NOI CAMERA", camIp, TFT_YELLOW);
  }

  lastResultPoll = millis();
  lastFpsMs = millis();
  lastStatsMs = millis();
  lastRetry = 0;
  lastDiscovery = millis();
  Serial.println("[SYS] San sang. Go /help de xem lenh.");
}

// ============================== LOOP ======================================
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
    drawStatusPage("DANG KET NOI CAMERA", camIp, TFT_YELLOW);
  }

  // ---------------- Doc ket qua nhan dien ----------------
  if (now - lastResultPoll >= RESULT_POLL_MS) {
    lastResultPoll = now;
    pollResult();
  }

  // ---------------- Fail-safe: scanning treo ----------------
  if (scanHold && now - scanStartMs > SCAN_MAX_MS) {
    Serial.println("[CAM] scanning qua lau -> tu bo qua");
    scanning = false;
    scanEndMs = now;
  }

  // ---------------- Bat dau / ket thuc tam dung khi quet ----------------
  if (scanning && !scanHold) {
    scanHold = true;
    streamStop();
    ss = SS_OFF;
    Serial.println("[CAM] Tam dung stream de nhuong frame cho dot quet");
    if (!lastFrameValid) tft.fillScreen(TFT_BLACK);
    drawOverlay();
  } else if (!scanning && scanHold) {
    if (scanEndMs == 0) scanEndMs = now;
    if (now - scanEndMs > SCAN_RESUME_MS) {
      scanHold = false;
      // KHONG xoa banner o day: banner tu het theo bannerUntil (5-8s) va van
      // duoc ve de len video khi stream chay lai.
      lastRetry = 0;
      Serial.println("[CAM] Quet xong -> bat lai stream");
    }
  }

  // ---------------- Banner het han tren trang idle ----------------
  if (manualPaused && bannerOn && (int32_t)(now - bannerUntil) >= 0) {
    bannerOn = false;
    idleDirty = true;
  }

  // ---------------- Stream / decode ----------------
  bool wantStream = !manualPaused && !scanHold;

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
    } else if (scanHold) {
      // Giu khung hinh cuoi; cap nhat thanh tien trinh
      if (now - lastScanOverlayMs > 250) {
        lastScanOverlayMs = now;
        drawOverlay();
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
        else drawStatusPage("MAT KET NOI CAM", camIp, TFT_RED);
      }
      // r == 0: frame qua lon, da bo -> doc tiep frame sau
    } else if (ss == SS_RETRY || ss == SS_OFF) {
      if (now - lastRetry >= STREAM_RETRY_MS) {
        lastRetry = now;
        ss = SS_CONNECTING;
        if (!lastFrameValid) {
          drawStatusPage("DANG KET NOI CAM", camIp, TFT_YELLOW);
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
