// ============================================================================
//  CAM STRIP TEST - ban chan doan toi gian (tam thoi, khong phai firmware chinh)
// ----------------------------------------------------------------------------
//  Muc dich: phan dinh firmware chinh (.ino) vs phan cung/nguon, bang cach
//  chay cau hinh Y HET ban minimal da chung minh chay duoc (VGA/q12/fb1,
//  WiFi mac dinh, khong webserver/NVS/vflip/flash/LEDC), chi them nut nhan
//  + POST anh tho (raw JPEG) len server + LED do bao ket qua.
//
//  Cach dung:
//   1. Arduino IDE: File -> New, dan NGUYEN FILE nay, Board "AI Thinker
//      ESP32-CAM", partition nhu firmware chinh, sua ssid/password/
//      SERVER_URL neu khac, Upload.
//   2. Mo Serial 115200. Noi GPIO13 -> GND (nut nhan) de chup 3 khung POST.
//   3. Dan output cho dev.
//
//  Doc ket qua:
//   - "POST 200 ... matched=1"  -> phan cung/nguon OK, benh nam o firmware
//     chinh (webserver/sleep/vflip/PWDN...), dev se bisect tu day.
//   - "fb NULL" mai            -> benh nam ngoai code (nguon day dan/cam),
//     du firmware chinh co sua gi cung vay.
// ============================================================================
#include "esp_camera.h"
#include <WiFi.h>

// ---- Chan AI Thinker (giong ban minimal) ----
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

#define BUTTON_PIN 13       // nut nhan: GPIO13 -> GND (INPUT_PULLUP)
#define RED_LED    33       // den do bao ket qua (tranh GPIO4 flash)

const char *ssid = "Free_Wifi";
const char *password = "0559649707";
// Doi IP neu may server khac 192.168.1.9 (giua nguyen ?capture=1)
const char *SERVER_URL = "http://192.168.1.9:5001/api/esp32/frame?capture=1";

static void redBlink(int count, int on_ms, int off_ms) {
  for (int i = 0; i < count; i++) {
    digitalWrite(RED_LED, HIGH);
    delay(on_ms);
    digitalWrite(RED_LED, LOW);
    delay(off_ms);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, LOW);

  WiFi.mode(WIFI_STA);   // co y de mac dinh (khong setSleep/setTxPower)
  WiFi.begin(ssid, password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(500);
  Serial.printf("WiFi: %s IP: %s\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "FAIL",
                WiFi.localIP().toString().c_str());

  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  esp_err_t err = esp_camera_init(&c);
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("init: %s PID: 0x%04x PSRAM: %s\n",
                err == ESP_OK ? "OK" : "FAIL",
                s ? s->id.PID : 0, psramFound() ? "co" : "KHONG");
  Serial.println("San sang. Noi GPIO13 -> GND de chup 3 khung POST len server.");
}

static bool last_btn = false;

// POST anh tho (raw JPEG) + doc matched/already tu JSON (khong can thu vien)
static void post_frame(uint8_t *jpg, size_t len) {
  // tach host/port/path tu SERVER_URL (chi ho tro http://host:port/duong-dan)
  String u = String(SERVER_URL);
  int p = u.indexOf("://");
  p = (p < 0) ? 0 : p + 3;
  int slash = u.indexOf('/', p);
  String hp = (slash < 0) ? u.substring(p) : u.substring(p, slash);
  String path = (slash < 0) ? "/" : u.substring(slash);
  int c = hp.lastIndexOf(':');
  String host = (c > 0) ? hp.substring(0, c) : hp;
  uint16_t port = (c > 0) ? (uint16_t)hp.substring(c + 1).toInt() : 80;
  Serial.printf("POST http://%s:%u%s (%u bytes)... ", host.c_str(), port,
                path.c_str(), (unsigned)len);

  WiFiClient cli;
  if (!cli.connect(host.c_str(), port, 2000)) {
    Serial.println("FAIL connect");
    redBlink(5, 60, 60);
    return;
  }
  cli.printf("POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: image/jpeg\r\n"
             "Content-Length: %u\r\nConnection: close\r\n\r\n",
             path.c_str(), host.c_str(), (unsigned)len);
  if (cli.write(jpg, len) != len) {
    Serial.println("FAIL send");
    cli.stop();
    redBlink(5, 60, 60);
    return;
  }
  // doc TOAN BO phan hoi den khi server dong ket noi (toi da 3KB/6s),
  // roi tach body sau \r\n\r\n
  String resp = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 6000 && resp.length() < 3072) {
    while (cli.available() && resp.length() < 3072) resp += (char)cli.read();
    if (!cli.connected() && !cli.available()) break;
    delay(5);
  }
  cli.stop();
  int code = 0, sp1 = resp.indexOf(' ');
  if (sp1 > 0) code = resp.substring(sp1 + 1).toInt();
  String body = resp;
  int hb = resp.indexOf("\r\n\r\n");
  if (hb >= 0) body = resp.substring(hb + 4);
  if (code < 200 || code >= 300) {
    Serial.printf("HTTP %d\n", code);
    redBlink(5, 60, 60);
    return;
  }
  int matched = (body.indexOf("\"matched\":1") >= 0
                 || body.indexOf("\"matched\": 1") >= 0) ? 1 : 0;
  int already = (body.indexOf("\"already\":true") >= 0
                 || body.indexOf("\"already\": true") >= 0) ? 1 : 0;
  Serial.printf("HTTP OK matched=%d already=%d\n", matched, already);
  if (matched && !already) redBlink(1, 800, 200);
  else if (matched && already) redBlink(2, 300, 200);
  else redBlink(3, 120, 120);
}

void loop() {
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !last_btn) {
    Serial.println("[STRIP] nhan nut -> chup 3 khung POST");
    for (int i = 0; i < 3; i++) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        Serial.printf("[STRIP] khung %d: fb NULL\n", i + 1);
        redBlink(5, 60, 60);
      } else {
        Serial.printf("[STRIP] khung %d: len=%u -> ", i + 1, (unsigned)fb->len);
        post_frame(fb->buf, fb->len);
        esp_camera_fb_return(fb);
      }
      delay(500);
    }
  }
  last_btn = pressed;
  delay(20);
}
