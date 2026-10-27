/* ===========================================================================
 *  ESP32-C3 Super Mini + LCD1602 (PCF8574 I2C) - HIỂN THỊ TÊN HỌC SINH
 * ===========================================================================
 *
 *  Vai trò: Chỉ HIỂN THỊ tên học sinh sau khi ESP32-CAM điểm danh.
 *  Không tự nhận diện — nó đọc kết quả từ ESP32-CAM qua HTTP GET /result.
 *
 *  Kiến trúc:
 *     [ESP32-CAM] --POST ảnh--> [Server Python] nhận diện → lưu kết quả
 *                                          |
 *                                          +-- HTTP GET /result <-- [ESP32-C3 + LCD]
 *
 *  ESP32-CAM đã có endpoint GET /result (firmware 2026-10-27-lcd1+).
 *  ESP32-C3 poll /result mỗi POLL_MS và hiển thị tên trên LCD.
 *
 *  Nối dây I2C (ESP32-C3 Super Mini):
 *     VCC  -> 5V
 *     GND  -> GND
 *     SDA  -> GPIO4
 *     SCL  -> GPIO5
 *
 *  Thư viện cần cài (Library Manager / arduino-cli lib install):
 *     LiquidCrystal_PCF8574 (Mathertel)  -- đã có
 *     (Không cần ArduinoJson — parse JSON đơn giản bằng strstr)
 *
 *  Giao tiếp Serial (115200):
 *     /ip                      -> xem IP ESP32-CAM đang dùng
 *     /ip 192.168.0.101        -> đặt IP ESP32-CAM (lưu NVS)
 *     /find                    -> tìm ESP32-CAM qua UDP broadcast 4321
 *     /help
 * =========================================================================== */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <LiquidCrystal_PCF8574.h>
#include <Wire.h>
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// ============================== CẤU HÌNH ==============================
#define WIFI_SSID       "Test"
#define WIFI_PASS       "88888888"

#define CAM_IP_DEFAULT  "192.168.0.101"   // IP ESP32-CAM (du phong)
#define CAM_PORT        80

#define LCD_SDA_PIN     4
#define LCD_SCL_PIN     5
#define LCD_COLS        16
#define LCD_ROWS        2

#define POLL_MS         1000              // poll /result moi 1s
#define HTTP_TIMEOUT_MS 1500
#define OFFLINE_FAILS   3                 // ~3s khong doc duoc -> offline
#define SHOW_NAME_MS    20000             // hien ten 20s
#define SHOW_MSG_MS     8000              // hien thong bao 8s
#define SCAN_SHOW_MS    4000              // hien "Dang quet..." 4s
#define DISC_QUERY_MS   1500              // cho phan hoi UDP 1.5s

#define DISC_PORT       4321
#define DISC_MAGIC      "FACESYNC"

// ============================== BIẾN ==============================
static Preferences prefs;
static WiFiUDP udp;
static bool udpReady = false;
static LiquidCrystal_PCF8574 *lcd = nullptr;

static char camIp[16] = CAM_IP_DEFAULT;
static bool camOnline = false;
static int  camFails = 0;

static uint32_t lastPoll = 0;
static uint32_t lastDisc = 0;

// Trang thai hien thi LCD
enum ShowKind { K_IDLE = 0, K_SCAN, K_NAME, K_NOMATCH, K_CAMERR, K_NETERR, K_OFFLINE };
static ShowKind showKind = K_IDLE;
static uint32_t showUntil = 0;
static char showName[64] = "";
static char showInfo[24] = "";
static int  scrollPos = 0;
static uint32_t lastScroll = 0;

// Ket qua nhan dien gan nhat (tu ESP32-CAM /result)
static uint32_t lastSeq = 0;
static bool haveSeq = false;

static char drawn[LCD_ROWS][LCD_COLS + 1];

// ============================ HELPER: CHUOI ============================
static void lcdClearCache() { memset(drawn, 0, sizeof(drawn)); }

static void lcdLine(uint8_t row, const char *s) {
  if (row >= LCD_ROWS || !lcd) return;
  char buf[LCD_COLS + 1];
  size_t n = strlen(s ? s : "");
  if (n > LCD_COLS) n = LCD_COLS;
  memcpy(buf, s, n);
  for (size_t i = n; i < LCD_COLS; i++) buf[i] = ' ';
  buf[LCD_COLS] = '\0';
  if (strncmp(drawn[row], buf, LCD_COLS) == 0) return;
  strncpy(drawn[row], buf, LCD_COLS);
  drawn[row][LCD_COLS] = '\0';
  lcd->setCursor(0, row);
  lcd->print(buf);
}

static void lcdRender() {
  if (!lcd) return;
  switch (showKind) {
    case K_OFFLINE:
      lcdLine(0, "Mat ket noi cam");
      lcdLine(1, camIp);
      break;
    case K_IDLE:
      lcdLine(0, "San sang diem danh");
      lcdLine(1, "Bam nut tren cam");
      break;
    case K_SCAN:
      lcdLine(0, "Dang quet...");
      lcdLine(1, "Giu mat truoc cam");
      break;
    case K_NOMATCH:
      lcdLine(0, "Khong nhan ra");
      lcdLine(1, "Thu lai nhe");
      break;
    case K_CAMERR:
      lcdLine(0, "Loi camera");
      lcdLine(1, "Kiem tra cam/nguon");
      break;
    case K_NETERR:
      lcdLine(0, "Loi mang/server");
      lcdLine(1, "Kiem tra WiFi cam");
      break;
    case K_NAME: {
      if (strlen(showName) > 0) {
        if (strlen(showName) > LCD_COLS) {
          int total = (int)strlen(showName) + 3;
          char buf[LCD_COLS + 1];
          for (int i = 0; i < LCD_COLS; i++) {
            int idx = (scrollPos + i) % total;
            buf[i] = (idx < (int)strlen(showName)) ? showName[idx] : ' ';
          }
          buf[LCD_COLS] = '\0';
          lcd->setCursor(0, 0);
          lcd->print(buf);
        } else {
          lcdLine(0, showName);
        }
      } else {
        lcdLine(0, "Khong ten");
      }
      lcdLine(1, showInfo);
      break;
    }
  }
}

// UTF-8 -> codepoint (tra ve so byte da doc)
static int utf8Next(const char *s, unsigned *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0) {
    if (((unsigned char)s[1] & 0xC0) != 0x80) { *cp = c; return 1; }
    *cp = ((unsigned)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    if (((unsigned char)s[1] & 0xC0) != 0x80 || ((unsigned char)s[2] & 0xC0) != 0x80) { *cp = c; return 1; }
    *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)((unsigned char)s[1] & 0x3F) << 6) |
          ((unsigned char)s[2] & 0x3F);
    return 3;
  }
  *cp = c;
  return 1;
}

static char asciiOf(unsigned cp) {
  if (cp < 0x80) return (char)cp;
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
    case 0x0128: return 'I'; case 0x0129: return 'i';
    case 0x0168: return 'U'; case 0x0169: return 'u';
    case 0x01A0: return 'O'; case 0x01A1: return 'o';
    case 0x01AF: return 'U'; case 0x01B0: return 'u';
    default: return '?';
  }
}

static void vnToAscii(const char *src, char *dst, size_t dst_sz) {
  size_t k = 0;
  const char *p = src ? src : "";
  while (*p && k + 1 < dst_sz) {
    unsigned cp;
    int adv = utf8Next(p, &cp);
    p += adv;
    char a = asciiOf(cp);
    if (a == 0) continue;                  // dau to hop -> bo
    if (k && dst[k - 1] == ' ' && a == ' ') continue; // gop khoang trang
    dst[k++] = a;
  }
  dst[k] = '\0';
  size_t start = 0;
  while (dst[start] == ' ') start++;
  if (start > 0) memmove(dst, dst + start, k - start + 1);
  size_t len = strlen(dst);
  while (len > 0 && dst[len - 1] == ' ') { dst[len - 1] = '\0'; len--; }
}

// ============================ JSON parse (strstr, khong can thu vien) =====
static bool jsonStrField(const char *json, const char *key, char *out, size_t out_sz) {
  if (out && out_sz) out[0] = '\0';
  if (!json || !key || !out || !out_sz) return false;
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t k = 0;
  while (*p && *p != '"' && k < out_sz - 1) {
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

static bool jsonBoolField(const char *json, const char *key, bool def) {
  if (!json || !key) return def;
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p) return def;
  p += strlen(pat);
  while (*p == ' ') p++;
  return (strncmp(p, "true", 4) == 0);
}

// Ap dung JSON tu /result. Tra ve true neu doc duoc.
static bool applyResult(const char *json) {
  int seq = jsonIntField(json, "seq", -1);
  if (seq < 0) return true;
  bool isNew = !haveSeq || (uint32_t)seq != lastSeq;
  if (!isNew) return true;

  lastSeq = (uint32_t)seq;
  haveSeq = true;

  char status[16] = "";
  jsonStrField(json, "status", status, sizeof(status));
  bool scanning = jsonBoolField(json, "scanning", false);

  if (!strcmp(status, "ok") || !strcmp(status, "already")) {
    char raw[64] = "", cls[24] = "";
    jsonStrField(json, "full_name", raw, sizeof(raw));
    jsonStrField(json, "class_name", cls, sizeof(cls));
    vnToAscii(raw, showName, sizeof(showName));
    if (jsonBoolField(json, "already", false))
      snprintf(showInfo, sizeof(showInfo), "Da diem danh");
    else if (strlen(cls) > 0)
      snprintf(showInfo, sizeof(showInfo), "Lop %s", cls);
    else
      snprintf(showInfo, sizeof(showInfo), "Diem danh OK");
    setKind(K_NAME, SHOW_NAME_MS);
    Serial.printf("[LCD] SEQ %u: %s | %s\n", lastSeq, showName, showInfo);
  } else if (!strcmp(status, "nomatch")) {
    setKind(K_NOMATCH, SHOW_MSG_MS);
  } else if (!strcmp(status, "camerr")) {
    setKind(K_CAMERR, SHOW_MSG_MS);
  } else if (!strcmp(status, "neterr")) {
    setKind(K_NETERR, SHOW_MSG_MS);
  } else if (!strcmp(status, "scanning") || scanning) {
    setKind(K_SCAN, SCAN_SHOW_MS);
  }
  return true;
}

static void setKind(ShowKind k, uint32_t durMs) {
  showKind = k;
  showUntil = millis() + durMs;
  if (k == K_NAME) { scrollPos = 0; lastScroll = millis(); }
  lcdClearCache();
}

// ============================ WIFI ============================
static bool wifiConnect(uint32_t timeout_ms) {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[WiFi] Dang ket noi '%s' ... ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] KHONG ket noi duoc");
    lcdLine(0, "WiFi LOI!");
    lcdLine(1, "Kiem tra pass");
    return false;
  }
  Serial.printf("[WiFi] OK - IP: %s\n", WiFi.localIP().toString().c_str());
  char buf[24];
  snprintf(buf, sizeof(buf), "IP %s", WiFi.localIP().toString().c_str());
  lcdLine(0, "WiFi OK");
  lcdLine(1, buf);
  delay(600);
  return true;
}

static bool isIp4Lan(const char *s) {
  if (!s || !*s) return false;
  int o[4] = {0,0,0,0}, nOct = 0, cur = 0, digits = 0;
  for (const char *q = s; *q && nOct < 4; q++) {
    if (*q >= '0' && *q <= '9') { cur = cur * 10 + (*q - '0'); if (++digits > 3 || cur > 255) return false; }
    else if (*q == '.') { if (!digits) return false; o[nOct++] = cur; cur = 0; digits = 0; }
    else break;
  }
  if (!digits) return false;
  o[nOct++] = cur;
  if (nOct != 4) return false;
  if (o[0] == 127 || o[0] == 0 || o[0] >= 224) return false;
  return (o[0] == 10) || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

// ============================ DISCOVERY UDP ============================
static void udpSend(const char *payload) {
  if (!udpReady || WiFi.status() != WL_CONNECTED) return;
  udp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
  udp.print(payload);
  udp.endPacket();
}

static bool discoverCam(uint32_t wait_ms) {
  if (!udpReady || WiFi.status() != WL_CONNECTED) return false;
  udpSend(DISC_MAGIC "|CAM?");
  uint32_t t0 = millis();
  while (millis() - t0 < wait_ms) {
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
      size_t kk = 0;
      while (*p && *p != '|' && *p != '\r' && *p != '\n' && kk < sizeof(ip) - 1)
        ip[kk++] = *p++;
      ip[kk] = '\0';
      if (!isIp4Lan(ip)) continue;
      if (strcmp(ip, camIp) != 0) {
        strncpy(camIp, ip, sizeof(camIp) - 1);
        camIp[sizeof(camIp) - 1] = '\0';
        prefs.putString("camip", camIp);
        Serial.printf("[DISC] Tim thay ESP32-CAM: %s (luu NVS)\n", camIp);
      }
      return true;
    }
    delay(20);
  }
  return false;
}

static void listenPassive() {
  if (!udpReady) return;
  int n = udp.parsePacket();
  if (n <= 0) return;
  char buf[128];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';
  const char *p = strstr(buf, DISC_MAGIC "|ESP32|");
  if (!p) return;
  p += strlen(DISC_MAGIC) + 7;
  char ip[16];
  size_t kk = 0;
  while (*p && *p != '|' && *p != '\r' && *p != '\n' && kk < sizeof(ip) - 1)
    ip[kk++] = *p++;
  ip[kk] = '\0';
  if (!isIp4Lan(ip)) return;
  if (strcmp(ip, camIp) != 0) {
    strncpy(camIp, ip, sizeof(camIp) - 1);
    camIp[sizeof(camIp) - 1] = '\0';
    prefs.putString("camip", camIp);
    Serial.printf("[DISC] Cam bao danh -> IP cam: %s\n", camIp);
    haveSeq = false;
  }
}

// ============================ HTTP POLL ============================
static HTTPClient http;
static bool httpOpen = false;

static void httpClose() {
  if (httpOpen) { http.end(); httpOpen = false; }
}

static bool pollCam() {
  if (WiFi.status() != WL_CONNECTED) { httpClose(); return false; }
  if (!httpOpen) {
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(true);
    if (!http.begin(camIp, CAM_PORT, "/result")) return false;
    httpOpen = true;
  }
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    if (body.length() >= 2) ok = applyResult(body.c_str());
  } else if (code <= 0) {
    httpClose();
  }
  return ok;
}

// ============================ SERIAL ============================
static char rxLine[32];
static size_t rxLen = 0;

static void handleSerialLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  if (!strcmp(line, "/help") || !strcmp(line, "help")) {
    Serial.println("Lenh: /ip                  -> xem IP cam + trang thai");
    Serial.println("      /ip 192.168.0.101    -> dat IP cam (luu NVS)");
    Serial.println("      /find                -> tim cam qua UDP ngay");
    Serial.println("      /help                -> sua lenh");
    return;
  }
  if (!strcmp(line, "/find")) {
    Serial.println("[DISC] Dang tim ESP32-CAM...");
    if (discoverCam(DISC_QUERY_MS)) Serial.printf("[DISC] cam: %s\n", camIp);
    else Serial.println("[DISC] Khong tim thay (cung WiFi? cam bat? UDP 4321?)");
    return;
  }
  if (!strncmp(line, "/ip", 3)) {
    const char *arg = line + 3;
    while (*arg == ' ') arg++;
    if (!*arg) {
      Serial.printf("[CFG] IP cam: %s, cam %s\n", camIp, camOnline ? "ONLINE" : "OFFLINE");
      return;
    }
    if (!isIp4Lan(arg)) { Serial.println("[CFG] IP khong hop le"); return; }
    strncpy(camIp, arg, sizeof(camIp) - 1);
    camIp[sizeof(camIp) - 1] = '\0';
    prefs.putString("camip", camIp);
    httpClose();
    haveSeq = false;
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

// ============================ SETUP / LOOP ============================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== MAN LCD DIEM DANH (ESP32-C3 + LCD1602 PCF8574) ===");

  // LCD
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setClock(100000);
  uint8_t lcdAddr = 0;
  const uint8_t addrs[2] = {0x27, 0x3F};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(addrs[i]);
    if (Wire.endTransmission() == 0) { lcdAddr = addrs[i]; break; }
  }
  if (!lcdAddr) {
    Serial.println("[LCD] KHONG thay PCF8574 (0x27/0x3F) - kiem tra day I2C");
    lcdAddr = 0x27;
  }
  Serial.printf("[LCD] I2C addr 0x%02X\n", lcdAddr);
  lcd = new LiquidCrystal_PCF8574(lcdAddr);
  lcd->begin(LCD_COLS, LCD_ROWS, Wire);
  lcd->setBacklight(255);
  lcd->clear();
  lcdClearCache();
  lcdLine(0, "Man diem danh");
  lcdLine(1, "Dang khoi dong...");

  // NVS: load IP cam da luu
  prefs.begin("lcd", false);
  String saved = prefs.getString("camip", "");
  if (saved.length() && isIp4Lan(saved.c_str())) {
    strncpy(camIp, saved.c_str(), sizeof(camIp) - 1);
    camIp[sizeof(camIp) - 1] = '\0';
  }
  Serial.printf("[CFG] IP cam: %s (mac du %s)\n", camIp, CAM_IP_DEFAULT);

  // WiFi
  if (!wifiConnect(20000)) {
    lcdClearCache();
    setKind(K_OFFLINE, 0);
    return;
  }
  udpReady = (udp.begin(DISC_PORT) == 1);
  if (!udpReady) Serial.println("[DISC] Khong mo duoc UDP 4321");

  // Thu IP da co
  if (!pollCam()) {
    Serial.println("[CAM] Chua doc duoc /result -> tim qua UDP...");
    lcdLine(0, "Dang tim camera");
    lcdLine(1, camIp);
    if (discoverCam(DISC_QUERY_MS)) pollCam();
  }

  lcdClearCache();
  setKind(K_IDLE, 0);
  lastPoll = millis();
  lastDisc = millis();
  Serial.println("[SYS] San sang. Go /help de xem lenh.");
}

void loop() {
  serialPoll();
  listenPassive();
  if (WiFi.status() != WL_CONNECTED) camOnline = false;

  uint32_t now = millis();

  // 1) Poll /result tu ESP32-CAM
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    bool ok = pollCam();
    if (ok) {
      camFails = 0;
      if (!camOnline) {
        camOnline = true;
        haveSeq = false;
        Serial.println("[CAM] ONLINE");
      }
      if (showKind == K_OFFLINE) setKind(K_IDLE, 0);
    } else {
      if (camFails < 255) camFails++;
      if (camFails == OFFLINE_FAILS && camOnline) {
        camOnline = false;
        haveSeq = false;
        Serial.println("[CAM] OFFLINE (khong doc duoc /result)");
      }
      if (camFails >= OFFLINE_FAILS && showKind != K_OFFLINE && showKind != K_NAME) {
        setKind(K_OFFLINE, 0);
      }
    }
  }

  // 2) Neu mat cam -> thinh thoang hoi lai
  if (!camOnline && WiFi.status() == WL_CONNECTED && now - lastDisc > 8000) {
    lastDisc = now;
    discoverCam(400);
  }

  // 3) Het thoi gian hien thi -> quay ve man idle
  if (showKind != K_IDLE && showKind != K_OFFLINE && showKind != K_SCAN && showUntil && now > showUntil) {
    setKind(K_IDLE, 0);
  }

  // 4) Chuoi chay cho ten dai
  if (showKind == K_NAME && strlen(showName) > LCD_COLS && now - lastScroll >= 450) {
    lastScroll = now;
    int total = (int)strlen(showName) + 3;
    scrollPos = (scrollPos + 1) % total;
  }

  lcdRender();
  delay(20);
}
