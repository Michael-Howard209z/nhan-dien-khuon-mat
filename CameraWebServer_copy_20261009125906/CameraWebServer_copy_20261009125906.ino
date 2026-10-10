// ============================================================================
//  ESP32-CAM + SERVER NHAN DIEN KHUON MAT (Python, thu muc goc du an)
// ----------------------------------------------------------------------------
//  Firmware nay = mau CameraWebServer cua Espressif + them che do PUSH khung
//  hinh ve server Python (server.py). Co 3 che do chon (xem bang duoi):
//
//   [A] PULL (khuyen dung khi test nhanh):
//       - ESP32: BUTTON_ENABLE = 0, PUSH_ENABLE = 0 (tat day, chi chay web + stream).
//       - Server (.env): STREAM_URL = http://<ip-esp32>:81/stream
//         (xem IP tren Serial Monitor, cong 80 = web, cong 81 = stream)
//       - Server tu keo MJPEG ve, nhan dien + diem danh nhu camera thuong.
//
//   [B] PUSH (nhieu cam / NAT kho keo ve):
//       - ESP32: BUTTON_ENABLE = 0, PUSH_ENABLE = 1, PUSH_MODE_MJPEG = 0,
//                PUSH_URL = http://<ip-may-chay-server>:<CAMERA_PORT>/api/esp32/frame
//         (CAMERA_PORT mac dinh 5001, xem .env; <ip-may> vd 192.168.1.9)
//       - Server chap nhan: image/jpeg tho, multipart field "file" (che do 0),
//         va ca cum MJPEG (che do 1). NHUNG voi Flask nen de MODE = 0 vi POST
//         MJPEG chunked giu keep-alive lam treo worker Flask.
//
//   [C] BUTTON - CHE DO NUT NHAN (mac dinh, moi):  <-- khuyen dung
//       - BUTTON_ENABLE = 1: cam DE OI (tat sensor) de gian CPU + PSRAM.
//         Nhan nut GPIO13 -> day cam len -> bat flash -> chup 1 JPEG ->
//         tat flash -> POST ?capture=1 len server -> LED phep biet ket qua ->
//         tat cam lai sau CAM_OFF_DELAY_MS.
//       - Khong co PUSH lien tuc, khong stream noi bo tu chay -> chip khong bi
//         "doi" boi luong khung lien tuc (nguyen nhan treo truoc day).
//       - Web :80 van chay de dev cau hinh cam (/control, /flash, /capture,
//         /live?state=0|1); stream :81 chi chay khi co nguoi mo trinh duyet.
//
//  Ghi chu nhan dien (quan trong):
//   - Anh ESP32 nho (QVGA 320x240) -> mat xa de < MIN_FACE_SIZE (mac dinh 44)
//     va bi bo qua. Nen de VGA 640x480 tro len (xem s->set_framesize ben duoi)
//     va tren server co the ha MIN_FACE_SIZE xuong 24-32 + tang DETECT_SCORE.
//   - Che do nut nhan chi chup 1 anh moi lan nhan -> khong can PUSH_FPS.
// ============================================================================
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>          // captive portal: phan moi ten ve IP cua minh khi o AP
#include <esp_heap_caps.h>
#include "app_config.h"         // cau hinh persistent (NVS): WiFi, IP tinh, server, huong cam

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// WiFi MAC DINH (chi dung khi LAN DAU chua co gi trong NVS).
// Sau nay sua tren trang web /setup (khong can code lai).
// ===========================
const char *ssid = "Free_Wifi";
const char *password = "0559649707";

// ============================================================================
//  CHE DO AP CAU HINH
//  Neu khong ket noi duoc WiFi (hoac giu nut luc boot) thi ESP32 tu tao mang
//  WiFi rieng (mac dinh: ESP32-CAM-Setup / 12345678) + captive portal ->
//  ket noi vao do, dien thoai tu mo trang http://192.168.4.1/ de cau hinh
//  mang WiFi, IP tinh, URL server, huong camera...
//  (ham startSetupMode() + bootHeldForSetup() xem truoc void setup())
// ============================================================================
static DNSServer dnsServer;

// ==================================================================
//  DAY KHUNG HINH LEN MAY CHU  -->  sua o day
// ==================================================================
#define PUSH_ENABLE      0                        // TAM TAT PUSH (mang nay chan ESP32->PC: 0 POST toi server)
                                                  // PULL chay on (server keo :81/stream). Muoi PUSH lai thi doi 1
                                                  // khi cam + server cung mang khong cach ly (vd hotspot dien thoai).
                                                  // LUU Y: BUTTON_ENABLE = 1 se TU TAT che do nay (xem duoi).
#define PUSH_URL         "http://192.168.1.9:5001/api/esp32/frame"  // endpoint server Python - MAC DINH
                                                  // khi NVS chua co gi. Thuc te sua tren trang web
                                                  // /setup (luu trong NVS), khong can code lai.
                                                  // <ip-may>: IP may chay "python server.py"
                                                  // :5001 = CAMERA_PORT trong .env
#define PUSH_MODE_MJPEG  0                        // 0 = POST anh tung khung (KHUYEN DUNG cho Flask)
                                                  //     moi khung 1 POST multipart field "file"
                                                  // 1 = POST luong MJPEG lien tuc (chunked, giu 1 ket noi)
                                                  //     chi dung khi server hieu MJPEG push (Flask de treo)
#define PUSH_FPS         10                       // khung/giay gui len (1..15) -- QVGA+q15 ~8KB/khung, 10fps ~640kbps muot
#define PUSH_JPEG_MAX    (120 * 1024)             // bo dem tam cho 1 khung (QVGA nho -> 120KB du, tiet kiem PSRAM)
#define PUSH_BOUNDARY    "esp32cam"
// ==================================================================

// ==================================================================
//  CHE DO NUT NHAN (che do chinh cua san pham)  -->  sua o day
// ==================================================================
#define BUTTON_ENABLE     1       // 1 = che do nut nhan (cam OFF khi nghi, nhan nut = quet lien tuc + POST diem danh)
                                  // 0 = quay lai che do cu (PULL/PUSH lien tuc, cam luon bat)
#define BUTTON_PIN       13     // GPIO13, INPUT_PULLUP: nut nhan noi GPIO13 -> GND, nhan xuong = muc THAP (active LOW)
//  Sao khong dung cac chan khac:
//   - GPIO12 (MTDI): strapping pin. Nut keo LOW luc boot se doi thanh suc
//     dieu khien SPIRAM 1.8V -> boot loi / khong nhan duoc PSRAM (mat camera).
//   - GPIO16 / GPIO17: dua giua PSRAM tren ESP32-CAM -> dung thi mat PSRAM.
//   - GPIO4: da dung cho flash LED (can de chup anh).
//   - GPIO0: strapping che do download; nut keo LOW luc boot vao mode nap
//     phan mem, firmware khong chay.
//   - GPIO1/3: UART (Serial), GPIO2 cung la strapping -> tranh.
#define CAM_OFF_DELAY_MS  1500   // giu cam bat them 1.5s sau khi POST xong roi moi tat (cho server/phep)
#define CAM_WAKE_ON_HTTP  1      // dev mo /capture hoac /stream thi tu day cam len lai
                                  // (app_httpd.cpp luon tu goi camWake(), khong doc duoc macro
                                  //  nay vi no nam rieng trong file .ino)
#define BUTTON_DEBOUNCE_MS 30    // chong rung: chi tin nhan khi muc on dinh >= 30ms
#define BUTTON_COOLDOWN_MS 800   // bo qua nhan moi trong 800ms sau lan truoc (tran nhan lung)
#define BUTTON_SCAN_SECONDS 15   // nhan nut -> quet lien tuc TOI DA 15 giay (trung mat la dung som)
#define BUTTON_POST_GAP_MS 300   // nghi giua 2 khung POST lien tiep (~2-3 khung/giay)
#define BUTTON_MAX_FAILS 3       // POST loi LIEN TIEP qua muc nay -> bao 5 nhap, dung quet
#define CAM_IDLE_SLEEP_MS 60000  // che do nut: dev xem :81 xong dong lai qua muc nay -> tu tat cam

// BUTTON_ENABLE = 1 -> che do nhan POST 1 anh moi lan nhan nut, khong can
// PUSH lien tuc nua. Neu nguoi dung de PUSH_ENABLE = 1 thi cung tu tat.
#if BUTTON_ENABLE && PUSH_ENABLE
#undef PUSH_ENABLE
#define PUSH_ENABLE 0
#endif

// PUSH_ONESHOT = duoc bien dich cac ham POST 1 anh (dung boi nut nhan
// va/hoac boi push_task khi PUSH_ENABLE = 1).
#if BUTTON_ENABLE || PUSH_ENABLE
#define PUSH_ONESHOT 1
#else
#define PUSH_ONESHOT 0
#endif

// ---- Prototype tuong minh (khong dua vao auto-prototype cua Arduino) ----
void startCameraServer();
void setupLedFlash();
bool camWake();          // bat camera (init lai neu can) -> true = OK
void camSleep();         // tat camera (tu choi neu stream dang chay)
#if defined(LED_GPIO_NUM)
extern int led_duty;     // do sang flash, dinh nghia trong app_httpd.cpp (dong bo UI web)
#endif
#if BUTTON_ENABLE
static void buttonPoll();                          // poll nut + xu ly su kien nhan
static void doButtonCapture();                     // quet lien tuc toi da 15 giay + POST + LED phep
static bool buttonPressedEvent();                  // debounce + xuong canh
static void btnBlink(int count, int on_ms, int off_ms);
static void btnLed(bool on);
static bool push_ensure_buf();
static int push_read_response_body(Client *c, char *buf, size_t buf_sz, int timeout_ms);
static int btn_parse_result(const char *json, int *already, char *out_name, size_t name_sz);
#endif

// ---------------- Trang thai push (doc tu app_httpd.cpp) ----------------
volatile int g_push_state = 0;      // 0 = chua ket noi/dang lai, 1 = dang day
volatile uint32_t g_push_ok = 0;    // so khung gui thanh cong
volatile uint32_t g_push_err = 0;   // so khung loi
const char *g_push_url = PUSH_URL;   // se tro sang cfg.push_url sau cfg_load() trong setup()

// ---------------- Trang thai camera (doc tu app_httpd.cpp) ----------------
volatile bool g_cam_on = false;         // true = camera da init (che do nguoc: OFF khi nghi)
volatile bool g_stream_active = false;  // true = stream :81 dang chay -> camSleep() khong duoc deinit
int g_button_pin = BUTTON_ENABLE ? BUTTON_PIN : -1;   // GPIO nut nhan, -1 = tat che do nut nhan

#if PUSH_ONESHOT

static String push_host, push_path;
static uint16_t push_port = 80;
static bool push_https = false;
static WiFiClient *push_wc = NULL;
static WiFiClientSecure *push_wcs = NULL;
static uint8_t *push_buf = NULL;

// Tach URL -> host / port / path (URL do nguoi dung cai tren trang /setup,
// luu trong NVS; PUSH_URL trong .ino chi la mac dinh lan dau)
static void push_parse_url() {
  String u = cfg.push_url[0] ? String(cfg.push_url) : String(PUSH_URL);
  push_https = u.startsWith("https://");
  int p = u.indexOf("://");
  p = (p < 0) ? 0 : p + 3;
  int slash = u.indexOf('/', p);
  String hp = (slash < 0) ? u.substring(p) : u.substring(slash);
  push_path = (slash < 0) ? "/" : u.substring(slash);
  int c = hp.lastIndexOf(':');
  if (c > 0) {
    push_host = hp.substring(0, c);
    push_port = (uint16_t)hp.substring(c + 1).toInt();
  } else {
    push_host = hp;
    push_port = push_https ? 443 : 80;
  }
}

// Cap bo dem JPEG 1 lan (chuong trinh chinh xac 1 bo dem cho ca nut nhan va push_task)
static bool push_ensure_buf() {
  if (push_buf) return true;
  push_buf = (uint8_t *)heap_caps_malloc(PUSH_JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!push_buf) push_buf = (uint8_t *)malloc(PUSH_JPEG_MAX);
  if (!push_buf) {
    Serial.println("[PUSH] khong cap duoc bo dem");
    return false;
  }
  return true;
}

static Client *push_client() {
  if (push_https) {
    if (!push_wcs) {
      push_wcs = new WiFiClientSecure();
      push_wcs->setInsecure();  // chi day, khong xac minh chu ky (cai dat cert neu can)
    }
    return push_wcs;
  }
  if (!push_wc) push_wc = new WiFiClient();
  return push_wc;
}

// Ket noi voi timeout ngan (2s): tranh treo task hang giay khi server chet/sai IP.
// Chu y: lop goc Client* CHI CO ban connect() 2 doi so. Goi ban 3 doi so
// (them timeout) truc tiep qua con tro Client* se LOI bien dich — phai cast ve
// lop cu the. WiFiClient co ban connect(host, port, timeout); HTTPS dung ban
// 2 doi so cho chac chan.
static bool push_connect(Client *c) {
  if (!push_https) {
    WiFiClient *w = (WiFiClient *)c;
    return w->connect(push_host.c_str(), push_port, 2000);
  }
  return c->connect(push_host.c_str(), push_port);
}

#if PUSH_ENABLE
// Chup 1 khung roi TRA ngay fb -> khong lam gi stream noi bo
static bool push_capture(size_t *out_len) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = (fb->len <= PUSH_JPEG_MAX);
  if (ok) {
    memcpy(push_buf, fb->buf, fb->len);
    *out_len = fb->len;
  } else {
    g_push_err = g_push_err + 1;
  }
  esp_camera_fb_return(fb);
  return ok;
}
#endif  // PUSH_ENABLE

#if PUSH_ENABLE && PUSH_MODE_MJPEG
// Gui body theo chunked (HTTP/1.1) cho luong MJPEG ko doi dinh
static bool push_send_chunk(Client *c, const uint8_t *data, size_t len) {
  char lh[16];
  int n = snprintf(lh, sizeof(lh), "%X\r\n", (unsigned)len);
  if (c->write((const uint8_t *)lh, n) != (size_t)n) return false;
  if (len && c->write(data, len) != len) return false;
  if (c->write((const uint8_t *)"\r\n", 2) != 2) return false;
  return true;
}

static bool push_send_mjpeg_frame(Client *c, const uint8_t *jpg, size_t len) {
  char ph[160];
  int n = snprintf(ph, sizeof(ph), "--" PUSH_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
  if (n <= 0) return false;
  if (!push_send_chunk(c, (const uint8_t *)ph, (size_t)n)) return false;
  return push_send_chunk(c, jpg, len);
}
#else
static const char PUSH_FORM_HEAD[] =
  "--" PUSH_BOUNDARY "\r\n"
  "Content-Disposition: form-data; name=\"file\"; filename=\"frame.jpg\"\r\n"
  "Content-Type: image/jpeg\r\n\r\n";
static const char PUSH_FORM_TAIL[] = "\r\n--" PUSH_BOUNDARY "--\r\n";

// POST 1 anh dinh dang multipart/form-data (Connection: close)
static bool push_post_frame(Client *c, const uint8_t *jpg, size_t len) {
  size_t body_len = (sizeof(PUSH_FORM_HEAD) - 1) + len + (sizeof(PUSH_FORM_TAIL) - 1);
  char head[224];
  int n = snprintf(head, sizeof(head),
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Content-Type: multipart/form-data; boundary=" PUSH_BOUNDARY "\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   push_path.c_str(), push_host.c_str(), (unsigned)body_len);
  if (n <= 0) return false;
  if (c->write((const uint8_t *)head, n) != (size_t)n) return false;
  if (c->write((const uint8_t *)PUSH_FORM_HEAD, sizeof(PUSH_FORM_HEAD) - 1) != sizeof(PUSH_FORM_HEAD) - 1) return false;
  if (c->write(jpg, len) != len) return false;
  if (c->write((const uint8_t *)PUSH_FORM_TAIL, sizeof(PUSH_FORM_TAIL) - 1) != sizeof(PUSH_FORM_TAIL) - 1) return false;
  return true;
}

#if PUSH_ENABLE
// Doc phan hoi HTTP, lay ma trang thai roi xoa het bo dem nhan
static int push_read_response(Client *c, int timeout_ms) {
  char line[48];
  int i = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)timeout_ms) {
    while (c->available()) {
      char ch = (char)c->read();
      if (i < (int)sizeof(line) - 1) line[i++] = ch;
      if (ch == '\n') { t0 = millis(); goto have_line; }
    }
    if (!c->connected()) break;
    delay(1);
  }
have_line:
  line[i] = 0;
  char *sp = strchr(line, ' ');
  int code = sp ? atoi(sp + 1) : 0;
  // doc het body/cong hoi con lai de khong tran buffer nhan (rut tu 800ms -> 250ms
  // de giam lag moi khung: server Flask tra JSON nho, doc nhanh la du)
  uint32_t t1 = millis();
  while (millis() - t1 < 250) {
    while (c->available()) { c->read(); t1 = millis(); }
    if (!c->connected() && !c->available()) break;
    delay(1);
  }
  return code;
}
#endif  // PUSH_ENABLE

#if BUTTON_ENABLE
// Tim header Content-Length (khong phan biet hoa/thuong) -> -1 neu khong co
static int push_content_length(const char *headers) {
  static const char KEY[] = "content-length:";
  for (const char *p = headers; *p; p++) {
    size_t k = 0;
    while (KEY[k]) {
      char a = p[k];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (a != KEY[k]) break;
      k++;
    }
    if (KEY[k] == 0) return atoi(p + sizeof(KEY) - 1);
  }
  return -1;
}

// Doc DAY DU phan hoi HTTP: status line + header + body (gioi han theo
// Content-Length, cap bo dem buf_sz - 1). Tra ve ma HTTP (0 = loi/timeout).
// Body luu sau header trong cung buf -> co the strstr khoa JSON truc tiep.
static int push_read_response_body(Client *c, char *buf, size_t buf_sz, int timeout_ms) {
  if (!buf || buf_sz < 8) return 0;
  size_t i = 0;         // so byte da luu vao buf (header + body)
  size_t body_got = 0;  // so byte body da nhan duoc
  size_t body_need = 0; // Content-Length (0 = khong co / khong biet)
  bool hdr_done = false;
  int code = 0;
  uint32_t t0 = millis();
  uint32_t last_data = millis();

  buf[0] = 0;
  while (millis() - t0 < (uint32_t)timeout_ms) {
    bool got = false;
    while (c->available() > 0) {
      int ch = c->read();
      if (ch < 0) break;
      got = true;
      last_data = millis();
      if (i < buf_sz - 1) buf[i++] = (char)ch;
      if (!hdr_done) {
        if (i >= 4 && buf[i - 4] == '\r' && buf[i - 3] == '\n' && buf[i - 2] == '\r' && buf[i - 1] == '\n') {
          hdr_done = true;
          buf[i] = 0;
          char *sp = strchr(buf, ' ');
          code = sp ? atoi(sp + 1) : 0;
          int cl = push_content_length(buf);
          body_need = (cl > 0) ? (size_t)cl : 0;
        }
      } else {
        body_got++;
        if (body_need && body_got >= body_need) break;
      }
    }
    if (hdr_done) {
      if (body_need && body_got >= body_need) break;
      // khong co Content-Length -> doi server dong ket noi (Connection: close)
      if (!body_need && !c->connected() && c->available() <= 0) break;
    }
    if (i > 0 && !got && (millis() - last_data) > 1500) break;  // im long qua lau
    if (i > 0 && !c->connected() && c->available() <= 0) break; // mat cong sau khi nhan duoc gi do
    delay(1);
  }
  if (i >= buf_sz) i = buf_sz - 1;
  buf[i] = 0;
  return code;
}

// Doc JSON phan hoi server (khoa ASCII o TOP LEVEL -> strstr la du):
//   {"ok":true,"matched":1,"already":false,"full_name":"Nguyen Van A",...}
// Tra ve matched (0/1); *already = 1 neu da diem danh roi; ten day vao out_name.
static int btn_parse_result(const char *json, int *already, char *out_name, size_t name_sz) {
  int matched = 0;
  if (already) *already = 0;
  if (out_name && name_sz) out_name[0] = 0;
  if (!json) return 0;

  const char *m = strstr(json, "\"matched\":");
  if (m) matched = atoi(m + 10);

  const char *a = strstr(json, "\"already\":");
  if (a) {
    a += 10;
    while (*a == ' ') a++;
    if (strncmp(a, "true", 4) == 0 && already) *already = 1;
  }

  const char *f = strstr(json, "\"full_name\":\"");
  if (f && out_name && name_sz) {
    f += 13;  // bo qua day chuoi "full_name":" (13 ky tu)
    size_t k = 0;
    while (*f && *f != '"' && k < name_sz - 1) out_name[k++] = *f++;
    out_name[k] = 0;
  }
  return matched;
}
#endif  // BUTTON_ENABLE
#endif  // khong phai che do MJPEG (duong POST 1 anh)

#endif  // PUSH_ONESHOT

#if PUSH_ENABLE
static void push_task(void *arg) {
  (void)arg;
  push_parse_url();

  if (!push_ensure_buf()) {
    vTaskDelete(NULL);
    return;
  }
  Serial.printf("[PUSH] %s -> %s\n", PUSH_MODE_MJPEG ? "MJPEG stream" : "frame POST", cfg.push_url);

  Client *c = NULL;
  uint32_t next_frame = 0;
  uint32_t next_retry = 0;      // backoff chung cho ca 2 che do (tranh spam connect khi server chet/sai IP)
  uint32_t last_fail_log = 0;

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      g_push_state = 0;
      if (c) c->stop();
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    // Chan toc theo PUSH_FPS + tu bo khung tre de khong cong don lag:
    // neu gui 1 khung ton 300ms ma nhip chi 100ms (10fps) thi bo qua khung giua,
    // dat lich lai tu BAY GIO (khong co gang "duoi kip" -> cang lag).
    uint32_t now = millis();
    if (now < next_frame) {
      vTaskDelay(5 / portTICK_PERIOD_MS);
      continue;
    }
    uint32_t interval = (1000 / (PUSH_FPS > 0 ? PUSH_FPS : 1));
    if (now > next_frame + interval) next_frame = now;  // tre >1 nhip -> reset lich
    else next_frame = now + interval;

    size_t len = 0;
    if (!push_capture(&len)) {
      vTaskDelay(5 / portTICK_PERIOD_MS);
      continue;
    }

#if PUSH_MODE_MJPEG
    // ---- Che do luong: 1 POST MJPEG duy nhat, gui lien tuc ----
    if (!c || !c->connected()) {
      g_push_state = 0;
      if (c) c->stop();
      if (now < next_retry) { vTaskDelay(100 / portTICK_PERIOD_MS); continue; }
      next_retry = now + 3000;

      c = push_client();
      if (!push_connect(c)) {
        if (millis() - last_fail_log > 5000) {
          Serial.printf("[PUSH] ket noi %s:%u that bai (kiem tra IP server + firewall + server.py co chay khong)\n",
                        push_host.c_str(), push_port);
          last_fail_log = millis();
        }
        g_push_err = g_push_err + 1;
        c->stop();
        continue;
      }
      String h;
      h.reserve(240);
      h += "POST "; h += push_path; h += " HTTP/1.1\r\n";
      h += "Host: "; h += push_host; h += "\r\n";
      h += "Content-Type: multipart/x-mixed-replace; boundary=" PUSH_BOUNDARY "\r\n";
      h += "Transfer-Encoding: chunked\r\n";
      h += "Connection: keep-alive\r\n\r\n";
      if (c->print(h) == 0) {
        Serial.println("[PUSH] gui header that bai");
        g_push_err = g_push_err + 1;
        c->stop();
        continue;
      }
      Serial.println("[PUSH] da ket noi, dang day luong MJPEG");
    }
    // doc phan hoi server (neu co) de khong tran buffer nhan
    while (c->available()) c->read();

    if (push_send_mjpeg_frame(c, push_buf, len)) {
      g_push_ok = g_push_ok + 1;
      g_push_state = 1;
    } else {
      Serial.println("[PUSH] loi gui khung, ket noi lai...");
      g_push_err = g_push_err + 1;
      g_push_state = 0;
      c->stop();
    }
#else
    // ---- Che do anh tung khung: moi khung 1 POST rieng ----
    // Backoff 3s khi server chet/sai IP: connect() blocking hang giay, thu moi
    // khung (10fps) se treo task + spam Serial -> ca stream noi bo (:81) cũng lag.
    if (now < next_retry) { vTaskDelay(100 / portTICK_PERIOD_MS); continue; }
    c = push_client();
    if (!push_connect(c)) {  // timeout 2s cho HTTP, khong doi mac dinh
      g_push_err = g_push_err + 1;
      g_push_state = 0;
      c->stop();
      next_retry = millis() + 3000;
      if (millis() - last_fail_log > 5000) {  // log thua 5s/lan, khong spam UART
        Serial.printf("[PUSH] ket noi %s:%u that bai (kiem tra IP server + firewall + server.py co chay khong)\n",
                      push_host.c_str(), push_port);
        last_fail_log = millis();
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    next_retry = 0;  // ket noi duoc -> reset backoff
    bool sent = push_post_frame(c, push_buf, len);
    int code = push_read_response(c, 1500);
    c->stop();
    if (sent && code >= 200 && code < 300) {
      g_push_ok = g_push_ok + 1;
      g_push_state = 1;
    } else {
      // Chi in log khi loi lien tuc (moi 20 loi) de UART khong lam lag stream.
      if ((g_push_err % 20) == 0) Serial.printf("[PUSH] loi gui anh (HTTP %d)\n", code);
      g_push_err = g_push_err + 1;
      g_push_state = 0;
    }
#endif
    vTaskDelay(1 / portTICK_PERIOD_MS);  // nhuong CPU ngay, khong cong them 10ms lag
  }
}
#endif  // PUSH_ENABLE

// ============================================================================
//  BAT / TAT CAMERA  (che do nut nhan giu sensor OFF khi nghi de gian CPU)
//  app_httpd.cpp goi cac ham nay qua extern (camWake khi dev mo /capture,
//  /stream, /live?state=1; camSleep khi /live?state=0).
// ============================================================================
static volatile bool g_cam_waking = false;  // chan 2 task cung luc init camera

// Noi dung init duoc tach tu setup() cu, chay duoc nhieu lan
static bool camWakeInit() {
#if defined(PWDN_GPIO_NUM) && (PWDN_GPIO_NUM >= 0)
  // PWDN cua sensor dang cap (active HIGH): keo LOW de nguon cho camera
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(5);   // cho nguon sensor on dinh truoc khi init I2C
#endif

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // Cap phat buffer VUA DU cho QVGA (khong phai UXGA): tiet kiem ~500KB PSRAM,
  // giam phan manh heap -> it lag/treo. Muon doi do phan giai tren web van duoc
  // (sensor set_framesize sau init), chi can buffer >= khung lon nhat muon dung.
  // QVGA+quality15 ~8KB: de fb_count=2 + GRAB_LATEST -> luon lay khung MOI nhat.
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;     // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_LATEST;    // luon lay khung hinh moi nhat -> it lag
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 15;                 // 12=net/nang, 15=can bang, 20-25=nhe/muot (0..63, so cang lon cang nhe)
  config.fb_count = 2;                      // double buffering -> frame rate cao hon

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 15;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
      config.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
      config.fb_count = 1;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  // chu y: GPIO13 cung la BUTTON_PIN mac dinh -> chi xay ra khi dung board ESP_EYE
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
#if defined(PWDN_GPIO_NUM) && (PWDN_GPIO_NUM >= 0)
    digitalWrite(PWDN_GPIO_NUM, HIGH);  // that bai -> tat nguon sensor cho tiet kiem
#endif
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    // SIEU MUOT: QVGA 320x240 + quality 15 (~8KB/khung, 10fps ~640kbps).
    // QVGA nhan gan <1.5m tot; muon nhan xa 2-3m thi mo web http://<ip-esp32>/
    // chuyen len VGA (nut preset "Can bang") — danh doi lag hon chut.
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 15);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

  // Huong camera do NGUOI DUNG chon tren trang /setup (luu trong NVS).
  // -1 = khong ep, giu mac dinh cua cam bien/boarding ben tren.
  if (cfg.vflip >= 0) s->set_vflip(s, cfg.vflip);
  if (cfg.hmirror >= 0) s->set_hmirror(s, cfg.hmirror);

  return true;
}

// Bat camera. An toan khi goi khi da day (tra ve true luon), va khong cho
// 2 task (loop nut nhan vs httpd) cung luc init.
bool camWake() {
  if (g_cam_on) return true;
  while (g_cam_waking) delay(5);   // task khac dang init -> cho
  if (g_cam_on) return true;
  g_cam_waking = true;
  bool ok = camWakeInit();
  g_cam_waking = false;
  if (ok) g_cam_on = true;
  return ok;
}

// Tat camera: chi khi camera DANG bat va stream :81 KHONG chay.
// -> esp_camera_deinit roi keo PWDN len cao (cat nguon sensor).
void camSleep() {
  if (!g_cam_on) return;
  if (g_stream_active) return;   // dang stream -> khong duoc deinit giua frame
  delay(50);                     // cho cac handler dang chay xong khung hoi
  if (g_stream_active) return;   // kiem tra lai sau delay
  esp_camera_deinit();
#if defined(PWDN_GPIO_NUM) && (PWDN_GPIO_NUM >= 0)
  digitalWrite(PWDN_GPIO_NUM, HIGH);  // PWDN cao -> sensor mat nguon
#endif
  g_cam_on = false;
}

// ============================================================================
//  CHE DO NUT NHAN: poll khong block, debounce, chup 1 anh + POST len server
// ============================================================================
#if BUTTON_ENABLE

// LED phep biet: dung flash GPIO4 (LED_GPIO_NUM) neu co, khong co thi den do GPIO33
static void btnLed(bool on) {
#if defined(LED_GPIO_NUM)
  led_duty = on ? 255 : 0;                             // dong bo "led_intensity" tren /status
  ledcWrite(LED_GPIO_NUM, led_duty);                   // ledcAttach da chay trong setupLedFlash()
#else
  digitalWrite(33, on ? HIGH : LOW);       // den do status tren ESP32-CAM
#endif
}

// Cac mau phep biet sau moi lan nhan:
//   1 nhap dai 800ms  -> diem danh OK (in ten len Serial)
//   2 nhap vua 300ms  -> nhan dien duoc NHUNG da diem danh roi trong buoi nay
//   3 nhap ngan 120ms -> khong nhan dien duoc mat / khong khop ai
//   5 nhanh 60ms      -> loi mang / loi server (connect fail hoac HTTP != 2xx)
static void btnBlink(int count, int on_ms, int off_ms) {
  for (int i = 0; i < count; i++) {
    btnLed(true);
    delay(on_ms);
    btnLed(false);
    delay(off_ms);
  }
}

// Phat hien su kien NHAN XUONG: debounce 30ms + xuong canh (chi kich khi tu
// nha -> nhan, tranh nhan duoc 2 lan khi nhan/am du). Nut active LOW.
static bool buttonPressedEvent() {
  static bool stable = false;     // gia tri on dinh (true = dang nhan)
  static bool last_raw = false;   // gia tri doc moi nhat
  static uint32_t changed_ms = 0; // luc raw moi doi

  bool raw = (digitalRead(BUTTON_PIN) == LOW);
  if (raw != last_raw) {
    last_raw = raw;
    changed_ms = millis();
  }
  if (raw != stable && (millis() - changed_ms) >= BUTTON_DEBOUNCE_MS) {
    stable = raw;
    if (stable) return true;   // canh xuong: vua nhan xuong
    return false;              // nha nut -> bo qua
  }
  return false;
}

// Yeu cau tat cam sau CAM_OFF_DELAY_MS (van dung duoc khi stream dang chay:
// loop() se thu lai moi lan cho den khi g_stream_active = false)
static uint32_t g_sleep_at = 0;
static bool g_sleep_req = false;
static uint32_t stream_off_at = 0;   // luc stream :81 vua dong (0 = khong theo doi)
static void cam_sleep_request() {
  g_sleep_at = millis() + CAM_OFF_DELAY_MS;
  g_sleep_req = true;
}

// Mot lan nhan nut = 1 phien quet: day cam -> flash ON (giu sang suot luc
// quet) -> lap: chup -> POST ?capture=1 -> doc JSON phan hoi:
//   - diem danh OK / da diem danh -> DUNG SOM, LED phep
//   - het BUTTON_SCAN_SECONDS ma chua khop -> 3 nhap
//   - POST loi lien tiep BUTTON_MAX_FAILS lan -> 5 nhap, dung quet
// Moi khung deu kem ?capture=1 nen server tu loai trung theo buoi (already).
static void doButtonCapture() {
  static char resp[2816];   // header ~256B + body cap ~2.5KB (static -> khong ton stack)

  Serial.printf("[BTN] nhan nut -> quet toi da %d giay, POST lien tuc len server\n",
                BUTTON_SCAN_SECONDS);

  // WiFi chua len -> bao ngay bang 5 nhanh (khong can day cam, khong ton pin)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BTN] loi mang: WiFi CHUA ket noi duoc (kiem tra ssid/password) -> 5 nhanh");
    btnBlink(5, 60, 60);
    return;
  }

  if (!camWake()) {
    Serial.println("[BTN] loi: khong day duoc camera");
    btnBlink(5, 60, 60);
    return;
  }
  // Xoa lich tat cam cu + moc stream cu: tranh truong hop nhan 2 lan lien nhau
  // (lich tat cua lan truoc chua kip chay) tat cam GIUA phien quet moi.
  g_sleep_req = false;
  stream_off_at = 0;
  if (!push_ensure_buf()) {
    btnBlink(5, 60, 60);
    cam_sleep_request();
    return;
  }

  // POST toi server, kem ?capture=1 (server biet day la anh nhan nut)
  push_parse_url();
  if (push_path.indexOf('?') < 0) push_path += "?capture=1";
  Client *c = push_client();

  // Bat flash TRUOC khi quet: sensor can LED sang ~250ms moi thay trong anh.
  // GIU flash SANG SUOT luc quet -> anh sang on dinh, nhan dien chuan hon
  // (flash sang lien tuc cung la tin hieu "dang quet" cho nguoi dung).
  // (cf. comment trong app_httpd.cpp, capture_handler)
  btnLed(true);
  delay(250);

  uint32_t t_end = millis() + (uint32_t)BUTTON_SCAN_SECONDS * 1000UL;
  int frames = 0, fails = 0;
  int outcome = 0;   // 0 = het gio chua khop, 1 = diem danh OK, 2 = da diem danh, 5 = loi mang
  char final_name[64];
  final_name[0] = '\0';

  while ((int32_t)(t_end - millis()) > 0 && outcome == 0) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[BTN] loi: chup anh that bai");
      fails++;
    } else if (fb->len > PUSH_JPEG_MAX) {
      Serial.printf("[BTN] loi: khung qua lon (%u bytes)\n", (unsigned)fb->len);
      esp_camera_fb_return(fb);
      fails++;
    } else {
      size_t len = fb->len;
      memcpy(push_buf, fb->buf, fb->len);
      esp_camera_fb_return(fb);   // tra fb NGAY, khong cham giua stream noi bo
      frames++;

      if (!push_connect(c)) {
        Serial.printf("[BTN] loi mang: khong ket noi duoc %s:%u\n",
                      push_host.c_str(), push_port);
        c->stop();
        fails++;
      } else {
        bool sent = push_post_frame(c, push_buf, len);
        int code = push_read_response_body(c, resp, sizeof(resp), 3000);
        c->stop();

        if (!sent || code < 200 || code >= 300) {
          Serial.printf("[BTN] loi server: POST that bai (HTTP %d)\n", code);
          fails++;
        } else {
          fails = 0;   // POST thanh cong -> xoa dem loi lien tiep
          g_push_ok = g_push_ok + 1;
          g_push_state = 1;

          int already = 0;
          char name[64];
          name[0] = '\0';
          int matched = btn_parse_result(resp, &already, name, sizeof(name));

          if (matched && !already) {
            outcome = 1;
            strncpy(final_name, name, sizeof(final_name) - 1);
            break;   // TRUNG -> dung quet som, khong can het 15 giay
          } else if (matched && already) {
            outcome = 2;
            strncpy(final_name, name, sizeof(final_name) - 1);
            break;   // DA DIEM DANH -> dung quet som
          } else {
            Serial.printf("[BTN] khung %d: chua khop, quet tiep...\n", frames);
          }
        }
      }
    }

    if (fails >= BUTTON_MAX_FAILS) {
      Serial.println("[BTN] loi mang lien tuc -> dung quet, 5 nhanh");
      g_push_err = g_push_err + 1;
      g_push_state = 0;
      outcome = 5;
      break;
    }
    delay(BUTTON_POST_GAP_MS);   // nhip quet ~2-3 khung/giay
  }

  btnLed(false);   // tat flash TRUOC khi nhap LED bao ket qua

  if (outcome == 1) {
    Serial.printf("[BTN] Diem danh OK sau %d khung: %s\n",
                  frames, final_name[0] ? final_name : "(khong doc duoc ten)");
    btnBlink(1, 800, 200);            // 1 nhap dai
  } else if (outcome == 2) {
    Serial.printf("[BTN] %s da diem danh roi trong buoi nay (%d khung)\n",
                  final_name[0] ? final_name : "Hoc sinh", frames);
    btnBlink(2, 300, 200);            // 2 nhap vua
  } else if (outcome == 5) {
    btnBlink(5, 60, 60);              // 5 nhanh
  } else {
    Serial.printf("[BTN] Het %d giay (%d khung): khong nhan dien duoc mat\n",
                  BUTTON_SCAN_SECONDS, frames);
    btnBlink(3, 120, 120);            // 3 nhap ngan
  }
  cam_sleep_request();                // tat cam sau CAM_OFF_DELAY_MS
}

// Poll nut nhan (khong block): debounce + xuong canh + cooldown
static void buttonPoll() {
  static uint32_t last_capture = 0;
  if (!buttonPressedEvent()) return;
  uint32_t now = millis();
  if (last_capture != 0 && (now - last_capture) < BUTTON_COOLDOWN_MS) return;  // dang/trong phoi nhan truoc
  last_capture = now;
  doButtonCapture();   // block trong thoi gian chup + POST (la giao dich 1 lan, chap nhan duoc)
}

#endif  // BUTTON_ENABLE

// ============================================================================
//  CHE DO AP CAU HINH (xem ghi chu o dau file)
// ============================================================================
// Giu nut nhan ~1.5s luc boot = vao che do AP cau hinh (dung khi muon doi
// cau hinh ma khong reset duoc WiFi da luu). Tra ve true = dang giu nut.
static bool bootHeldForSetup() {
#if BUTTON_ENABLE
  Serial.println("[CFG] Nhan GIU nut ~1.5s luc boot de vao che do AP cau hinh...");
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    if (digitalRead(BUTTON_PIN) == LOW) return true;   // dang nhan -> che do cau hinh
    delay(10);
  }
#endif
  return false;
}

// Tao mang WiFi rieng + captive portal cho trang cau hinh /setup
static void startSetupMode() {
  cfg_setup_mode = true;

  // WIFI_AP_STA (khong chi AP) de /api/scan van quet duoc WiFi khi cau hinh.
  // (Luu y: luc quet, AP se nhay kenh 1-2s -> client co the mat ket noi ngan)
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);   // khong cho STA tu thu lai lam gian AP
  WiFi.setSleep(false);           // tat sleep -> AP on dinh, phan hoi cau hinh nhanh
  IPAddress ap_ip(192, 168, 4, 1);
  WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
  bool ap_ok;
  if (strlen(cfg.ap_pass) >= 8) {
    ap_ok = WiFi.softAP(cfg.ap_ssid, cfg.ap_pass);  // WPA2 (mat khau >= 8 ky tu)
  } else {
    ap_ok = WiFi.softAP(cfg.ap_ssid);               // mat khau qua ngan -> AP mo
    Serial.println("[CFG] ap_pass < 8 ky tu -> AP mo (khong co mat khau)");
  }
  if (!ap_ok) {
    // SSID rong hoac ky tu la -> AP that bai; dung lai ten mac dinh cho chac
    Serial.println("[CFG] softAP THAT BAI -> thu lai ten mac dinh 'ESP32-CAM-Setup'");
    WiFi.softAP("ESP32-CAM-Setup");
  }

  // Captive portal: moi ten tenh deu tro ve 192.168.4.1 -> dien thoai tu hien
  // trang cau hinh (Android/iOS/Windows deu kiem tra cac endpoint /generate_204...)
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", ap_ip);

  Serial.println("[CFG] *** VAO CHE DO AP CAU HINH ***");
  Serial.printf("[CFG] Ket noi vao mang WiFi '%s' (mat khau: %s)\n",
                cfg.ap_ssid, strlen(cfg.ap_pass) >= 8 ? cfg.ap_pass : "(khong)");
  Serial.printf("[CFG] Mo trinh duyet tai http://%s/ de cau hinh\n",
                ap_ip.toString().c_str());
}

void setup() {
  Serial.begin(115200);
  // Tat debug log tren UART -> giam nhieu, stream muot hon nhieu
  Serial.setDebugOutput(false);
  Serial.println();

  // Doc cau hinh da luu trong NVS (lan dau: mac dinh ssid/password/PUSH_URL
  // o tren file .ino). Sau nay sua tren trang web /setup, khong can code lai.
  cfg_load(ssid, password, PUSH_URL);
  g_push_url = cfg.push_url;   // trang /status tren web hien URL dang dung

#if BUTTON_ENABLE
  pinMode(BUTTON_PIN, INPUT_PULLUP);   // nut nhan: GPIO13 -> GND, nhan xuong = muc THAP
  digitalWrite(BUTTON_PIN, HIGH);      // dam bao muc cao khi INPUT_PULLUP khong ho tro
  // Chan doan: in trang thai nghi cua chan nut khi boot
  Serial.printf("[BTN] GPIO13 nghi = %s (phai HIGH; neu LOW -> bi keo xuong / noi sai)\n",
                digitalRead(BUTTON_PIN) == LOW ? "LOW" : "HIGH");
#endif

#if BUTTON_ENABLE
  // Che do nhan nut: cam OI TU LUC BOOT (chua init), chi day khi co nguoi yeu
  // cau: nhan nut hoac dev mo /capture, /stream, /live?state=1.
  // Neu cam loi cung van chay tiep web + nut nhan (nut se bao loi bang 5 nhanh).
  Serial.println("[BTN] Camera OFF (cho nhan nut), web van chay tai :80");
#else
  if (!camWake()) {   // che do cu: loi camera -> dung setup nhu truoc
    return;
  }
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();          // ledcAttach(LED_GPIO_NUM, 5000, 8) -> sau do ledcWrite() la du
#else
  pinMode(33, OUTPUT);      // khong co flash -> dung den do status GPIO33 cho LED phep biet
  digitalWrite(33, LOW);
#endif

  // ---- MANG: ket noi WiFi da luu, that bai -> tu tao mang AP cau hinh ----
  if (cfg.wifi_ssid[0] == '\0' || bootHeldForSetup()) {
    // Chua co WiFi nao duoc luu hoac giu nut luc boot (muon sua cau hinh)
    // -> vao AP NGAY, khong ton 20s cho ket noi that bai.
    if (cfg.wifi_ssid[0] == '\0') Serial.println("[CFG] Chua co WiFi nao duoc luu -> vao che do AP cau hinh");
    startSetupMode();
  } else {
    WiFi.persistent(false);                // cau hinh WiFi do app_config quan ly (NVS), khong can SDK luu nua
    WiFi.mode(WIFI_STA);                   // tat AP cho do ton RAM/CPU
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);                  // tat sleep -> HTTP nhanh, it lag
    WiFi.setTxPower(WIFI_POWER_19_5dBm);   // phat cong suat toi da -> song WiFi khoe hon
    if (cfg.use_static_ip) cfg_apply_static_ip();   // IP tinh PHAI cai TRUOC WiFi.begin()
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

    Serial.printf("WiFi connecting to '%s'", cfg.wifi_ssid);
    uint32_t wifi_t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifi_t0) < 20000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      // Khong ket noi duoc -> TU TAO mang WiFi rieng de sua cau hinh (sai mat
      // khau, doi mang, doi IP static...) thay vi chi in canh bao nhu truoc day.
      Serial.println("\n[CFG] WiFi KHONG ket noi duoc sau 20s -> tao mang AP cau hinh");
      startSetupMode();
    }
  }

  // Bao hieu "firmware da chay": LED sang co dinh ~1.5s khi boot xong.
  // (Khac moi mau nhap ket qua -> nhanh nhanh biet board song, kể cả khi chua co WiFi.)
  btnLed(true);
  delay(1500);
  btnLed(false);

  startCameraServer();

#if PUSH_ENABLE
  // Task day luong live len may chu (uu tien thap hon web server -> khong lam gi stream)
  // BUTTON_ENABLE = 1 -> PUSH_ENABLE da bi tu tat o tren -> khong co task nay.
  if (xTaskCreate(push_task, "cam_push", 8192, NULL, 1, NULL) != pdPASS) {
    Serial.println("[PUSH] khong tao duoc task");
  }
#endif

#if BUTTON_ENABLE
  Serial.printf("[BTN] Che do nut nhan: nut GPIO%d -> GND (nhan xuong = muc thap)\n", BUTTON_PIN);
  Serial.printf("[BTN] LED phep tren GPIO%d, quet toi da %d giay moi lan nhan -> %s?capture=1\n",
#if defined(LED_GPIO_NUM)
                LED_GPIO_NUM,
#else
                33,
#endif
                BUTTON_SCAN_SECONDS,
                cfg.push_url);
#endif
  if (cfg_setup_mode) {
    Serial.print("CHE DO CAU HINH! Ket noi WiFi '");
    Serial.print(cfg.ap_ssid);
    Serial.println("' roi mo http://192.168.4.1/ de cau hinh");
  } else {
    Serial.print("Camera Ready! Use 'http://");
    Serial.print(WiFi.localIP());
    Serial.println("' to connect (trang cau hinh: /setup)");
  }
}

void loop() {
  if (cfg_setup_mode) {
    dnsServer.processNextRequest();   // captive portal: phan moi ten ve minh
  }
#if BUTTON_ENABLE
  // Chan doan: in ra moi khi trang thai chan nut DOI (nhan/nha) -> kiem tra wiring
  static bool last_raw = false;
  bool raw_now = (digitalRead(BUTTON_PIN) == LOW);
  if (raw_now != last_raw) {
    last_raw = raw_now;
    Serial.printf("[BTN] GPIO13 -> %s\n", raw_now ? "LOW (dang nhan)" : "HIGH (nha nut)");
  }

  buttonPoll();   // nhan nut -> quet lien tuc toi da 15 giay (block trong phien quet)

  // Tat cam sau CAM_OFF_DELAY_MS; neu stream dang chay thi giu yeu cau va thu lai
  if (g_sleep_req && (int32_t)(millis() - g_sleep_at) >= 0 && !g_stream_active) {
    g_sleep_req = false;
    camSleep();
  }

  // Tu tat cam khi idle: dev mo :81 xem xong roi dong lai ma quen tat cam ->
  // cam khong duoc phep sang mai (che do nut: cam chi bat khi quet hoac dev xem).
  static bool last_stream = false;
  bool s = g_stream_active;
  if (last_stream && !s) stream_off_at = millis();   // stream vua dong -> bat dau dem
  last_stream = s;
  if (!s && !g_sleep_req && stream_off_at != 0 &&
      (int32_t)(millis() - stream_off_at) >= (int32_t)CAM_IDLE_SLEEP_MS) {
    stream_off_at = 0;
    camSleep();   // tu return neu cam dang OFF
  }
  delay(5);   // nhe CPU, van du do cho debounce 30ms
#else
  // Do nothing. Everything is done in another task by the web server
  delay(10000);
#endif
}
