//  ESP32-CAM + SERVER NHAN DIEN KHUON MAT (Python, thu muc goc du an)
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
//   [C] BUTTON - CHE DO NUT NHAN (mac dinh, moi):  <-- khuyen dung
//       - Khi o che do cho (idle): ESP32-CAM va server KHONG truyen hinh anh.
//         Chi gui heartbeat nhe (GET /api/esp32/heartbeat) moi 3s de biet nhau
//         dang hoat dong (online).
//       - Khi nhan nut (GPIO13): ESP32-CAM truyen hinh anh LIEN TUC trong 15s
//         den server de nhan dien khuon mat.
//         ("truyen lien tuc" = lay tung khung tu camera roi POST len server
//         ~2-3 lan/giay. ESP32-CAM khong co "ong truyen thang" sensor -> server;
//         POST MJPEG lien tuc da thu truoc do va bi tat vi treo worker Flask.)
//       - Camera duoc init NGAY LUC NHAN NUT (khong init tai boot), dung cau
//         hinh da chung minh chay duoc: VGA / quality 12 / fb_count 1 /
//         GRAB_WHEN_EMPTY (giong ban CamButtonStrip). KHONG bat log driver
//         camera VERBOSE (truoc do gay "cam_hal: FB-OVF" + fb_get NULL vi
//         nghet UART 115200).
//       - Server thuc hien nhan dien va ghi nhan diem danh DUNG 1 LAN trong
//         dot 15s do (loai bo trung lap giua cac khung hinh trong cung phien).
//       - Ket thuc 15s: den LED bao ket qua (1 nhap dai = diem danh OK,
//         2 nhap vua = da diem danh, 3 nhap ngan = khong khop, 4 = loi cam, 5 = loi mang),
//         sau do tro ve che do cho khong truyen anh.
#include "esp_camera.h"
#include "esp_log.h"             // esp_log_level_set: mo log driver camera khi chan doan fb NULL
#include <WiFi.h>
#include <esp_system.h>          // esp_reset_reason: phat hien board bi SAP NGUON (brownout)
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>          // captive portal: phan moi ten ve IP cua minh khi o AP
#include <esp_heap_caps.h>
#include "driver/ledc.h"       // dat XCLK TRUOC khi tha PWDN (xem camStartXclkEarly)
#include "driver/gpio.h"       // gpio_get_level: do truc tiep VSYNC/HREF/PCLK tai /camdiag
#include "app_config.h"         // cau hinh persistent (NVS): WiFi, IP tinh, server, huong cam
#include <WiFiUdp.h>            // phat hien server qua UDP broadcast (khong can IP server)
// Select camera model in board_config.h

#include "board_config.h"

// WiFi MAC DINH (chi dung khi LAN DAU chua co gi trong NVS).
// Sau nay sua tren trang web /setup (khong can code lai).

const char *ssid = "Test";
const char *password = "88888888";
//  CHE DO AP CAU HINH
//  Neu khong ket noi duoc WiFi (hoac giu nut luc boot) thi ESP32 tu tao mang
//  WiFi rieng (mac dinh: ESP32-CAM-Setup / 12345678) + captive portal ->
//  ket noi vao do, dien thoai tu mo trang http://192.168.4.1/ de cau hinh
//  mang WiFi, IP tinh, URL server, huong camera...
//  (ham startSetupMode() + bootHeldForSetup() xem truoc void setup())
static DNSServer dnsServer;
//  DAY KHUNG HINH LEN MAY CHU  -->  sua o day
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
#define PUSH_JPEG_MAX    (120 * 1024)             // bo dem tam cho 1 khung (VGA q12 ~15-30KB -> 120KB du, tiet kiem PSRAM)
#define PUSH_BOUNDARY    "esp32cam"
//  CHE DO NUT NHAN (che do chinh cua san pham)  -->  sua o day

#define BUTTON_ENABLE     1       // 1 = che do nut nhan (cam OFF khi nghi, nhan nut = truyen anh 15s)
                                  // 0 = quay lai che do cu (PULL/PUSH lien tuc, cam luon bat)
#define FW_VERSION "2026-10-26-fix1"  // in ra Serial luc boot de biet board dang chay ban nao
// --- Tu tim IP server qua UDP broadcast (xem them o phan DISC duoi day) ---
//  1 = MOI (khuyen dung): server phat "FACESYNC|SERVER|http://ip:5001" ra mang,
//      ESP32 tu doi URL khi server doi IP. Khong anh huong gi den camera.
//  0 = TAT (chi de so sanh chan doan): neu nghi code moi gay loi camera thi tat
//      di, nap lai va bam nut. Camera van NULL thi LOI KHONG PHAI o discovery.
#define DISCOVERY_ENABLE  1
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
#define BUTTON_SCAN_SECONDS 15   // nhan nut -> truyen anh lien tuc 15 giay de nhan dien (ghi nhan 1 lan)
#define BUTTON_POST_GAP_MS 300   // nghi giua 2 khung POST lien tiep (~2-3 khung/giay)
#define BUTTON_MAX_FAILS 3       // POST loi LIEN TIEP qua muc nay -> dung quet
#define BUTTON_SCAN_FLASH 0      // 1 = giu flash sang suot luc quet; 0 = tat flash (an toan nguon yeu)
#define HEARTBEAT_INTERVAL_MS 3000 // Gui heartbeat nhe (khong truyen anh) moi 3 giay khi cho

// ---- CAU HINH DRIVER CAMERA (DUNG HET CHO BAN MINIMAL DA CHAY DUOC) ----
//  CamButtonStrip (VGA + q12 + fb_count 1 + GRAB_WHEN_EMPTY, khong log verbose)
//  chay hoan toan tot tren CUNG board nay. Firmware chinh truoc do dung
//  fb_count 2 + GRAB_LATEST + log VERBOSE -> "cam_hal: FB-OVF" + fb_get NULL
//  (nguyen nhan: log driver camera nghet UART 115200 -> task camera khong tra
//  buffer kip). Ve lai dung cau hinh da chung minh.
#define CAM_FB_COUNT      1     // 1 buffer: du khi chi 1 nguoi doc (chup nut) moi luc
#define CAM_GRAB_LATEST   0     // 0 = CAMERA_GRAB_WHEN_EMPTY (giong minimal da chay duoc)

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

// Prototype tuong minh (khong dua vao auto-prototype cua Arduino)
void startCameraServer();
void setupLedFlash();
bool camWake();          // bat camera (init lai neu can) -> true = OK
void camSleep();  // GIU NGUYEN camera (khong deinit - xem dinh nghia duoi)
static int camWarmup();  // bo 3 khung dau sau init + in ket qua ra Serial (doan)
#if defined(LED_GPIO_NUM)
extern int led_duty;     // do sang flash, dinh nghia trong app_httpd.cpp (dong bo UI web)
#endif
#if BUTTON_ENABLE
static void buttonPoll();                          // poll nut + xu ly su kien nhan
static void doButtonCapture();                     // truyen anh lien tuc 15 giay + POST + LED phep
static bool buttonPressedEvent();                  // debounce + xuong canh
static void btnBlink(int count, int on_ms, int off_ms);
static void btnLed(bool on);
static bool push_ensure_buf();
static int push_read_response_body(Client *c, char *buf, size_t buf_sz, int timeout_ms);
static int btn_parse_result(const char *json, int *already, int *faces, int *ids,
                            char *out_name, size_t name_sz);
#endif
static void heartbeatPoll();                       // duy tri ket noi (khong truyen anh) khi cho

static void serialPoll();   // lenh Serial (/url, /help): doi URL server khong can nap lai

//Tu tim IP server qua UDP broadcast 
static uint32_t g_last_hb_ok_ms = 0;   // lan heartbeat thanh cong gan nhat
                                       // (dung cho ca discovery va chan doan)
#if DISCOVERY_ENABLE
//  Boi vi: IP may chay server co the doi (DHCP), luc do ESP32 "bi mat" server
//  vi url cu trong NVS tro toi IP da khong con. Cach khac phuc:
//   + Server (Python) moi 5s gui 1 goi UDP broadcast "FACESYNC|SERVER|http://ip:5001"
//     ra cong 4321 (di ra ngoai -> khong bi Windows Firewall chan).
//   + ESP32 lang nghe; neu hien dang OFFLINE roi nghe thay url khac thi doi sang url
//   + ESP32 cung gui "FACESYNC|DISCOVER" khi offline (server tra loi truc tiep) va
//     "FACESYNC|ESP32|<ip>|<mode>" de server biet ip camera (de nghi uu tien: rule
//     cho phep UDP 4321 den may chay server).
#define DISC_PORT 4321
#define DISC_MAGIC "FACESYNC"
#define DISC_MIN_GAP_MS 5000    // it nhat 5s moi lan gui DISCOVER khi offline
static WiFiUDP g_discUdp;
static bool g_discOn = false;
static uint32_t g_discLastSend = 0;   // lan GUI di (DISCOVER/ANNOUNCE) gan nhat
static uint32_t g_discLastInfo = 0;   // lan gui "minh la ESP32 nao" cho server

// Trang thai nay duoc KHAI BAO duoi (phan bien toan cuc) - o day chi la khai
// bao truoc (forward declaration) vi ham discovery dung chung.
extern volatile bool g_server_online;
extern volatile bool g_is_scanning;
extern volatile uint32_t g_last_heartbeat_ms;

static const char *discModeName() {
#if BUTTON_ENABLE
  return "button";
#elif PUSH_ENABLE
  return "push";
#else
  return "pull";
#endif
}

static void discSend(const char *payload) {
  if (!g_discOn || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (g_discLastSend && (now - g_discLastSend) < DISC_MIN_GAP_MS) return;  // han che do nhieu
  g_discLastSend = now;
  if (g_discUdp.beginPacket("255.255.255.255", DISC_PORT)) {   // broadcast toan mang
    g_discUdp.print(payload);
    g_discUdp.endPacket();
  }
}

// Gui khong ton han che 5s (dung rieng cho luc boot).
static void discSendNow(const char *payload) {
  g_discLastSend = 0;
  discSend(payload);
}

// Server dang bao: "minh la http://<ip>:5001[/api/esp32/frame]" -> neu ESP32 dang
// OFFLINE va url khac url hien tai thi chuyen sang url moi (luu NVS).
// Kiem tra host trong URL co phai dia chi IPv4 LAN hop le khong.
//
// *** HANG PHONG THU QUAN TRONG NHAT CUA DISCOVERY ***
// Log 2026-10-26: server da tung broadcast "http://127.0.0.1:5001/..." va ESP32
// da LUU no vao NVS. 127.0.0.1 la loopback = chinh ESP32 -> POST anh vao chinh
// no -> he thong "ngu" ma heartbeat van bao ONLINE (heartbeat dung path co dinh),
// rat kho phat hien. Chi may ma server tu sua lai bang goi sau do.
//
// Vi vay: TU CHOI moi URL ma host khong phai IP LAN that su, truoc khi cham vao
// NVS. Server co loi hoac co script khac cung broadcast cung khong xong.
//  - 127.x  -> loopback (bui "ngu" ngay lap tuc)
//  - 0.x    -> "khong xac dinh" / loi parse
//  - 169.254-> APIPA (router chua cap IP)
//  - >=224  -> multicast / reserved
//  - khong du 4 octet hoac co chu cai -> khong phai IP
static bool discHostIsLan(const char *url) {
  if (!url || !*url) return false;
  const char *p = strstr(url, "://");
  const char *h = p ? p + 3 : url;
  if (!*h) return false;

  int o[4] = {0, 0, 0, 0};
  int n = 0, cur = 0, digits = 0;
  for (const char *q = h; *q && n < 4; q++) {
    char c = *q;
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      digits++;
      if (cur > 255 || digits > 3) return false;       // octet qua lon / qua nhieu chu so
    } else if (c == '.') {
      if (digits == 0) return false;                   // co "." rong -> sai dinh dang
      o[n++] = cur; cur = 0; digits = 0;
    } else {
      break;   // het host (gap ':' port, '/' path, '?' query...)
    }
  }
  if (digits == 0) return false;                       // thieu octet cuoi
  o[n++] = cur;
  if (n != 4) return false;                            // khong du 4 octet -> khong phai IP

  if (o[0] == 127 || o[0] == 0) return false;          // loopback / "khong xac dinh"
  if (o[0] == 169 && o[1] == 254) return false;        // APIPA
  if (o[0] >= 224) return false;                       // multicast / reserved
  return (o[0] == 10)
      || (o[0] == 192 && o[1] == 168)
      || (o[0] == 172 && o[1] >= 16 && o[1] <= 31);    // chi d LAN RFC1918
}

static void discOnServerUrl(const char *url) {
  if (!url || !*url) return;

  // KIEM TRA TRUOC CAU HINH - chi nhan URL co host la IP LAN.
  if (!discHostIsLan(url)) {
    Serial.printf("[DISC] BO QUA URL khong hop le (khong phai IP LAN): %s\n", url);
    return;
  }

  // CHAN DOAN: server co the chi gui "http://ip:port" (ban server cu / server
  // khac). Neu bo nguyen vao NVS thi se MAT duong dan "/api/esp32/frame" ->
  // POST anh loi 404 ma heartbeat van ONLINE (vi heartbeat dung path co dinh)
  // -> rat kho phat hien. Vao day: neu thieu path thi them vao.
  char fixed[96];
  const char *p = strstr(url, "://");           // bo qua "http://"
  const char *path = p ? strchr(p + 3, '/') : strchr(url, '/');
  if (!path || path[1] == '\0') {               // khong co path (hoac chi "/")
    size_t n = strlen(url);
    while (n > 0 && url[n - 1] == '/') n--;      // bo dau "/" cuoi neu co
    snprintf(fixed, sizeof(fixed), "%.*s/api/esp32/frame", (int)n, url);
    Serial.printf("[DISC] Server gui thieu duong dan -> bo sung: %s\n", fixed);
    url = fixed;
  }

  if (strcmp(url, cfg.push_url) == 0) return;         // khong doi gi

  // Chi doi khi dang mat ket noi (khong lam gian doan phien dang chay tot).
  // "Chua tung online" (g_last_heartbeat_ms==0) van cho phep -> chay tot tu boot.
  //
  // NGOAI LE QUAN TRONG: neu url HIEN TAI dang thieu duong dan API thi no VAN
  // "online" (heartbeat dung path co dinh) nhung POST anh se loi 404 -> phai
  // cho phep doi NGAY, khong doi qua trang thai online.
  {
    const char *cur = cfg.push_url;
    const char *cp = strstr(cur, "://");
    const char *cpath = cp ? strchr(cp + 3, '/') : strchr(cur, '/');
    bool cur_path_ok = (cpath && cpath[1] != '\0');
    if (g_last_heartbeat_ms != 0 && g_server_online && cur_path_ok) {
      Serial.printf("[DISC] Nghe server %s nhung dang online -> giu %s\n", url, cur);
      return;
    }
  }

  Serial.printf("[DISC] Tim thay server moi: %s (cu: %s)\n", url,
                cfg.push_url[0] ? cfg.push_url : "(mac dinh)");
  strncpy(cfg.push_url, url, sizeof(cfg.push_url) - 1);
  cfg.push_url[sizeof(cfg.push_url) - 1] = '\0';
  cfg_save();                                            // luu NVS -> boot lai van dung
  push_parse_url();                                      // nhan lai host/port/path moi
  if (g_server_online) g_server_online = false;          // cho heartbeat thu lai ngay
}

// Doc goi tin server gui (khong chan): "FACESYNC|SERVER|http://..."
static void discRx() {
  for (int i = 0; i < 4; i++) {                          // toi da 4 goi moi lan poll
    int n = g_discUdp.parsePacket();
    if (n <= 0) break;
    char buf[128];
    int len = g_discUdp.read(buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = '\0';
    if (strncmp(buf, DISC_MAGIC "|", strlen(DISC_MAGIC) + 1) != 0) continue;
    const char *body = buf + strlen(DISC_MAGIC) + 1;
    if (strncmp(body, "SERVER|", 7) == 0) discOnServerUrl(body + 7);
  }
}

static void discPoll() {
  if (!g_discOn || WiFi.status() != WL_CONNECTED) return;

  // 1) Nhan tin server (neu co)
  discRx();

  // 2) Van de: dang OFFLINE qua lau -> hoi toan mang "ai la server?"
  //    (cai nay chi can khi server chua broadcast duoc, vi vay nen co 2 huong).
  uint32_t now = millis();
  if (!g_server_online && !g_is_scanning &&
      (g_last_hb_ok_ms == 0 || (now - g_last_hb_ok_ms) > 10000)) {
    char msg[96];
    snprintf(msg, sizeof(msg), DISC_MAGIC "|DISCOVER|%s", discModeName());
    discSend(msg);
  }
}

// Luc BOOT (WiFi vua len): hoi toan mang NGAY vai lan, moi lan nghe ~700ms.
// Neu server phan hoi thi URL dung tu dau, khong can cho 5-15s nhu khi rung.
// Tra ve true = da nghe thay server (url da duoc discOnServerUrl cap nhat).
static bool discBootBurst() {
  if (!g_discOn || WiFi.status() != WL_CONNECTED) return false;
  char msg[96];
  snprintf(msg, sizeof(msg), DISC_MAGIC "|DISCOVER|%s", discModeName());
  for (int i = 0; i < 3; i++) {
    discSendNow(msg);
    char before[sizeof(cfg.push_url)];
    strncpy(before, cfg.push_url, sizeof(before) - 1);   // ban sao DE SO SANH
    before[sizeof(before) - 1] = '\0';
    uint32_t t0 = millis();
    while (millis() - t0 < 700) {        // cho server tra loi 700ms
      discRx();
      if (strcmp(cfg.push_url, before) != 0) return true;   // da doi URL
      delay(5);
    }
  }
  return false;
}

// Bao voi server "minh la ESP32 <ip> o che do <mode>" -> server biet ip camera
// ngay ca khi ESP32 CHUA gui duoc HTTP (vi sai url server). Gui 15s/lan.
static void discInfoPoll() {
  if (!g_discOn || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (g_discLastInfo && (now - g_discLastInfo) < 15000) return;
  g_discLastInfo = now;
  char msg[80];
  snprintf(msg, sizeof(msg), DISC_MAGIC "|ESP32|%s|%s",
           WiFi.localIP().toString().c_str(), discModeName());
  discSend(msg);   // van ton trong DISC_MIN_GAP_MS -> khong gay nheu
}

// Mo cong UDP phat hien (goi sau khi WiFi da ket noi).
static void discBegin() {
  if (cfg_setup_mode) return;   // AP cau hinh thi khong phat hien gi
  g_discOn = (g_discUdp.begin(DISC_PORT) == 1);
  Serial.printf("[DISC] UDP port %d: %s (server broadcast -> ESP32 tu doi URL)\n",
                DISC_PORT, g_discOn ? "san sang" : "KHONG mo duoc");
}
#else  // !DISCOVERY_ENABLE - khong co code discovery (chi de chan doan)
static void discPoll() {}
static void discInfoPoll() {}
static void discBegin() {}
static bool discBootBurst() { return false; }
#endif  // DISCOVERY_ENABLE

//Trang thai push va heartbeat 
volatile int g_push_state = 0;      // 0 = chua ket noi/dang lai, 1 = dang day
volatile uint32_t g_push_ok = 0;    // so khung gui thanh cong
volatile uint32_t g_push_err = 0;   // so khung loi
const char *g_push_url = PUSH_URL;   // se tro sang cfg.push_url sau cfg_load() trong setup()
volatile bool g_server_online = false; // true = server Python dang phan hoi heartbeat (online)
volatile bool g_is_scanning = false;   // true = dang trong dot truyen anh 15 giay
volatile uint32_t g_last_heartbeat_ms = 0;   // khong static: ham discovery doc duoc
//Trang thai camera (doc tu app_httpd.cpp) 
volatile bool g_cam_on = false;         // true = camera da init (che do nguoc: OFF khi nghi)
volatile bool g_stream_active = false;  // true = stream :81 dang chay (chi de hien thi, camSleep hien la no-op)
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
  String hp = (slash < 0) ? u.substring(p) : u.substring(p, slash);
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
// (them timeout) truc tiep qua con tro Client* se LOI bien dich â€” phai cast ve
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
static bool push_post_frame(Client *c, const uint8_t *jpg, size_t len, const char *custom_path = NULL) {
  size_t body_len = (sizeof(PUSH_FORM_HEAD) - 1) + len + (sizeof(PUSH_FORM_TAIL) - 1);
  const char *p_path = custom_path ? custom_path : push_path.c_str();
  char head[256];
  int n = snprintf(head, sizeof(head),
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Content-Type: multipart/form-data; boundary=" PUSH_BOUNDARY "\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   p_path, push_host.c_str(), (unsigned)body_len);
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
//   {"ok":true,"matched":1,"already":false,"full_name":"Nguyen Van A",
//    "count":1,"ids":5,...}
// Tra ve matched (0/1); *already = 1 neu da diem danh roi; *faces = so mat
// thay trong khung (count); *ids = so nguoi da dang ky mau (-1 neu thieu key);
// ten day vao out_name. Con tro NULL duoc bo qua (tuong thich server cu).
static int btn_parse_result(const char *json, int *already, int *faces, int *ids,
                            char *out_name, size_t name_sz) {
  int matched = 0;
  if (already) *already = 0;
  if (faces) *faces = 0;
  if (ids) *ids = -1;
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

  const char *c = strstr(json, "\"count\":");
  if (c && faces) *faces = atoi(c + 8);   // atoi tu bo khoang trang

  const char *g = strstr(json, "\"ids\":");
  if (g && ids) *ids = atoi(g + 6);

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
    // khung (10fps) se treo task + spam Serial -> ca stream noi bo (:81) cÅ©ng lag.
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

//  BAT / TAT CAMERA  (che do nut nhan giu sensor OFF khi nghi de gian CPU)
//  app_httpd.cpp goi cac ham nay qua extern (camWake khi dev mo /capture,
//  /stream, /live?state=1; camSleep khi /live?state=0).

static volatile bool g_cam_waking = false;  // chan 2 task cung luc init camera

// ============================================================================
//  BAT XCLK TRUOC KHI THA PWDN (chu ky dung cua cam bien anh)
//
//  Doan nay sua NGUYEN NHAN GOC sau rat nhieu lan doan (2026-10-19..26):
//  "doc duoc sensor PID 0x5640 nhung khong xuat frame nao" (fb_get NULL 4000ms).
//
//  XCLK (chan GPIO0 tren AI-Thinker) duoc esp_camera_init() bat TUONG UNG TRONG
//  luc no chay. Neu ta cat/cap nguon sensor (PWDN) TRUOC luc do thi sensor len
//  nguon trong khi KHONG CO DONG HO:
//    - SCCB/I2C van doc duoc PID (OV5640 co dong ho noi bo cho bus)   -> PID OK
//    - nhung PLL cua sensor khong khoa duoc tu XVCLK -> khong co xuat anh
//      -> esp_camera_fb_get() het timeout 4000ms -> NULL
//  Do chinh la tai sao "nguon tot, PID dung, van khong co khung".
//
//  Cach dung: dat XCLK TRUOC (dung chinh LEDC_TIMER_0 + LEDC_CHANNEL_0 ma
//  esp_camera_init se dung, 20MHz, duty 1/1bit = 50%) -> roi moi tha PWDN.
//  Luc esp_camera_init() chay no se cau hinh lai LEDC voi CUNG tham so
//  -> khong co xung dong, khong anh huong gi.
//
//  Chu ky dung: XCLK on dinh  -> PWDN HIGH (tat) -> PWDN LOW (sensor len
//  nguon DA CO DONG HO) -> cho on dinh -> init I2C/cau hinh.
// ============================================================================
static void camStartXclkEarly() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_1_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 20000000;                 // 20MHz - giong camera_config_t.xclk_freq_hz
  t.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&t) != ESP_OK) {
    Serial.println("[CAM] loi cau hinh LEDC timer cho XCLK som");
    return;
  }
  ledc_channel_config_t c = {};
  c.gpio_num = XCLK_GPIO_NUM;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.intr_type = LEDC_INTR_DISABLE;      // KHONG bat ngat - esp_camera_init se quan ly
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 1;                           // 50% khi LEDC_TIMER_1_BIT (giong driver)
  c.hpoint = 0;
  if (ledc_channel_config(&c) != ESP_OK) {
    Serial.println("[CAM] loi cau hinh LEDC channel cho XCLK som");
    return;
  }
  Serial.printf("[CAM] da bat XCLK som (%u Hz, chay TRUOC khi tha PWDN)\n",
                (unsigned)20000000);
}

// Noi dung init duoc tach tu setup() cu, chay duoc nhieu lan
static bool camWakeInit() {
#if defined(PWDN_GPIO_NUM) && (PWDN_GPIO_NUM >= 0)
  camStartXclkEarly();    // BAT DONG HO TRUOC - khong de sensor len nguon khong XCLK
  // CAP NGUON LAI SENSOR (PWDN) 2 LAN TRUOC MOI LAN INIT - khong chi dat LOW.
  //
  // NGUYEN NHAN GOC (chung minh bang log Serial 2026-10-19/20):
  //  Luc COLD BOOT, chan GPIO32 (PWDN) dang o trang thai input/Hi-Z cua luc
  //  reset chip -> sensor tu len nguon trong khi XCLK (GPIO0) chua on dinh
  //  -> OV5640 bi "TREO": doc duoc PID qua I2C (0x5640) nhung khong xuat
  //  frame -> fb_get() NULL 3/3.
  //  Ban cu chi `digitalWrite(PWDN, LOW)` (vi rang sensor da TAT) -> khong
  //  thuc su cap lai -> loi xay ra lai moi lan reboot.
  //
  //  TAI SAO LA 2 LAN? Nhan tu chinh log Serial (2026-10-20):
  //   - luc init dau tien (CHI 1 chuoi PWDN)  -> 0/3 khung (NULL)
  //   - lan /cam2 sau do (2 chuoi PWDN lien tuc) -> 3/3 khung (143/30/30 ms)
  //  Hieu don gian: sensor luc nay dang trong trang thai "treo" nen can MOT
  //  LAN CAT NGUON NUA de thuc su ve trang thai sach. Them chuoi thu 2 chi ton
  //  them 900ms va chi xay ra DUY NHAT luc driver vua init (khong phai moi frame).
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);   // Chuoi 1: TAT nguon sensor
  delay(600);                          // cho sensor thuc su tat
  digitalWrite(PWDN_GPIO_NUM, LOW);    // MO lai nguon
  delay(300);                          // cho on dinh truoc khi init I2C
  digitalWrite(PWDN_GPIO_NUM, HIGH);   // Chuoi 2: cat nua lan (log chung minh can)
  delay(600);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(300);
#endif

  // *** FIX QUAN TRONG 2026-10-26: ZERO-INIT TOAN BO STRUCT ***
  // Truoc day: `camera_config_t config;` = bien stack KHONG khoi tao.
  // Core 3.3.12 them cac field moi vao cuoi struct: jpeg_buffer_size,
  // sccb_i2c_port, conv_mode. Firmware khong set -> chung chua RAC STACK.
  // Driver cam_config() doc:
  //     if (config->jpeg_buffer_size > 0) recv_size = config->jpeg_buffer_size;
  //     else                              recv_size = w*h/5;
  //     fb_size = recv_size;
  // RAC thuong la dia chi con tro (0x3FFBxxxx ~ 1.07 ty) -> recv_size khong lo
  // -> "frame buffer malloc failed" (da thay trong log). Rac nho -> bo dem qua
  // be -> JPEG tran -> 0/3 am tham. Doi layout stack (them code) -> doi rac ->
  // "luc truoc chay duoc, gio hong". Day chinh la nguyen nhan goc.
  camera_config_t config = {};             // <-- zero-init TAT CA field
  config.jpeg_buffer_size = 0;             // 0 = driver tu tinh w*h/5 (VGA=61440)
  config.sccb_i2c_port = -1;               // dung chan SDA/SCL ben duoi
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
  // Cau hinh VGA + quality 12 da chung minh chay duoc tren OV5640 (ban minimal).
  // fb_count / grab_mode lay tu macro tren dau file: 1 + WHEN_EMPTY (giong ban
  // minimal da chay duoc; khong dung 2/LATEST + log verbose -> FB-OVF).
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;     // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAM_GRAB_LATEST ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;                  // 12=net/khoe, 15=can bang, 20-25=nhe/muot (0..63, so cang lon cang nhe)
  config.fb_count = CAM_FB_COUNT;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 12;
      // Van dung macro (mac dinh 1 + WHEN_EMPTY, giong ban minimal da chay duoc).
      config.fb_count = CAM_FB_COUNT;
      config.grab_mode = CAM_GRAB_LATEST ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
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
  Serial.printf("[CAM] init: VGA/q12/fb_count=%d/grab=%s/PSRAM=%s\n",
                (int)config.fb_count,
                (config.grab_mode == CAMERA_GRAB_LATEST) ? "LATEST" : "WHEN_EMPTY",
                psramFound() ? "co" : "KHONG");
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
  // KHONG goi set_framesize/set_quality sau init: tren OV5640, doi do phan
  // giai sau init lam treo luong khung (fb NULL mai). Dung luon cau hinh
  // luc init (VGA/quality 12, giong ban minimal da chung minh chay).

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

// "Tat" camera: KHONG deinit that.
// Ly do (chung minh bang log Serial): esp_camera_deinit() khong giai phong
// sach heap DMA noi bo -> esp_camera_init() ke tiep chet voi
// "cam_dma_config: frame buffer malloc failed" (cam brick den khi reset chip).
// XAC NHAN LAI 2026-10-20: 2/2 lan deinit() -> init lai de that bai
// (error 0xffffffff) ngay ca khi chi lap MOT lan - khong phai lap nhieu lan.
// Nguoc lai, lan init khong qua deinit (g_cam_on=false) luon thanh cong.
// Vi vay: init 1 LAN duy nhat luc can dau tien, giu nguyen suot uptime.
// Sensor chi chay that khi co fb_get (/quet nut) hoac :81 stream; luc nghi
// tieu thu them ~100mA so voi deinit - chap nhan duoc de doi lay chay on dinh.
// Muon tat hoan toan -> reset board. API giu nguyen de /live va loop khong doi.
void camSleep() {
  return;   // co y khong lam gi
}

//  CHAN DOAN NGUON: ESP32 co the RESET vi SAP NGUON (brownout) khi camera +
//  WiFi dong luc tren nguon yeu. Neu lan boot truoc bi brownout thi day la
//  NGUYEN NHANso 1 giai thich vi sao sensor doc duoc PID nhung khong xuat frame
//  (sensor can dong lon hon khi bat dau stream -> nguon rot -> sensor khoi
//  dong lai khong day du -> khong co khung, trong khi I2C van doc duoc).

static const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "BAT NGUON (nguon moi duoc cam vao)";
    case ESP_RST_EXT:       return "RESET BEN NGOAI (chan RESET)";
    case ESP_RST_SW:        return "RESET PHAN MEM (reboot)";
    case ESP_RST_PANIC:     return "CRASH / PANIC (phan mem roi)";
    case ESP_RST_INT_WDT:   return "WATCHDOG NGAT (treo ngat)";
    case ESP_RST_TASK_WDT:  return "WATCHDOG TASK (treo task)";
    case ESP_RST_WDT:       return "WATCHDOG (treo)";
    case ESP_RST_DEEPSLEEP: return "THUC DAY TU SLEEP";
    case ESP_RST_BROWNOUT:  return "SAP NGUON (BROWNOUT) - NGUON YEU!";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    default:                return "KHONG RO";
  }
}

static void camPowerReport() {
  esp_reset_reason_t r = esp_reset_reason();
  Serial.printf("[PWR] Lan reset truoc: %s\n", resetReasonName());
  if (r == ESP_RST_BROWNOUT) {
    Serial.println("[PWR] *** DAY LA NGUYEN NHAN CHIN: board bi SAp nguon luc truoc. ***");
    Serial.println("[PWR] *** Sensor bi mat dien luc dang hoat dong -> khong xuat frame.   ***");
    Serial.println("[PWR] *** FIX: nguon 5V/2A rieng (khong dung cong USB may tinh),       ***");
    Serial.println("[PWR] *** cap USB ngan + day, hoac them tu 470uF/10V vao 5V-GND.       ***");
  }
  Serial.printf("[PWR] Heap trong: %u byte, PSRAM: %u byte\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

//  NONG MAY: sau khi init, bo 3 khung dau tien va IN ket qua ra Serial.
//  Van de dang gap: esp_camera_init() OK (doc duoc PID sensor) NHUNG
//  esp_camera_fb_get() luon tra NULL -> khong co khung nao de gui len server.
//  Ham nay phan biet ro 2 truong hop:
//    -3/3 NULL -> sensor KHONG XUAT FRAME (XCLK/LEDC bi chiem, nguon yeu,
//                 loi cap nguon PWDN) -> phai sua phan cung, khong phai loi luong diem danh.
//    - co frame -> camera tot, loi o noi khac.
static int camWarmup() {
  int ok = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t t0 = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    uint32_t dt = millis() - t0;
    bool got = (fb != NULL);
    if (got) {
      ok++;
      esp_camera_fb_return(fb);
    }
    Serial.printf("[CAM] nong may khung %d: %s (doi %lu ms)\n", i + 1,
                  got ? "OK" : "NULL - khong co frame", (unsigned long)dt);
    delay(60);
  }
  Serial.printf("[CAM] nong may xong: %d/3 khung co frame\n", ok);
  return ok;
}


//  TU PHUC HOI: luc boot, OV5640 thuong bi "TREO" (doc duoc PID nhung khong xuat
//  frame).Nguyen nhan: sensor van con trang thai ngu truoc do nguon cham, hoac
//  nguon 5V chua on dinh luc init.
//  Cach khac phuc (da chung minh bang lenh /cam2): cat nguon sensor qua chan
//  PWDN (HIGH 600ms) -> cap lai (LOW) -> init lai -> 3/3 khung.
//  Ham nay tu goi khi camWarmup() == 0, khong can nguoi dung go /cam2.
//  Tra ve so khung co frame sau khi thu lai.

static int camWarmupRecover() {
  // *** KHONG GOI esp_camera_deinit() ***
  // Log 2026-10-20 chung minh ro: moi lan deinit() -> init lai la that bai voi
  //   "cam_hal: cam_dma_config(509): frame buffer malloc failed" (error 0xffffffff)
  //   -> cam "brick" den khi reset chip. Ly do: deinit khong giai phong sach
  //   bo dem DMA noi bo (doan ghi chu tai camSleep() cua du an da canh bao truoc).
  //   Nguoc lai: init KHONG qua deinit luon thanh cong (xem log: lan /cam2).
  //
  // PHAN NHANH THEO g_cam_on (sua 2026-10-26):
  //  - g_cam_on == false (driver chua init / init loi): cat/cap PWDN + camWake()
  //    -> camWakeInit() init lai -> duong NAY da chung minh 3/3 (lan /cam2).
  //  - g_cam_on == true (driver dang chay nhung khong xuat khung): KHONG cat
  //    nguon. Cat nguon sensor luc driver dang chay se MAT cau hinh VGA/JPEG
  //    da set luc init va khong the init lai duong dan on dinh (deinit bi loi)
  //    -> chi RESET board de init lai tu dau la duong duy nhat an toan.
  if (g_cam_on) {
    Serial.println("[CAM] Driver DANG CHAY nhung khong xuat khung (khong cat nguon).");
    Serial.println("[CAM] KHONG cat PWDN luc driver dang chay (se mat cau hinh VGA/JPEG).");
    Serial.println("[CAM] Hay RESET board de init lai tu dau (nut Reset hoac cat/bat nguon).");
    return 0;
  }

  Serial.println("[CAM] 0/3 khung -> CAT/ CAP NGUON SENSOR QUA PWDN (KHONG deinit)...");
#if defined(PWDN_GPIO_NUM) && (PWDN_GPIO_NUM >= 0)
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);   // TAT nguon sensor
  delay(600);
  digitalWrite(PWDN_GPIO_NUM, LOW);    // MO lai
  delay(300);
#endif
  // Driver chua init (g_cam_on=false) -> camWake() se init (camWakeInit co
  // chuoi PWDN rieng) va day la duong da chung minh 3/3 khung (lan /cam2).
  if (!camWake()) {
    Serial.println("[CAM] LOI: khong init duoc sensor sau cap nguon lai");
    return 0;
  }
  delay(300);
  return camWarmup();
}


//  CHE DO NUT NHAN: poll khong block, debounce, chup 1 anh + POST len server

#if BUTTON_ENABLE

// LED phep biet: dung flash GPIO4 (LED_GPIO_NUM) neu co, khong co thi den do GPIO33.
// Co y dung digitalWrite (bat/tat), KHONG dung LEDC: ledcAttach() tu chon timer
// co the de len timer XCLK cua camera -> sensor Treo (init OK nhung fb NULL mai).
// Ta chi can on/off (khong can mo do sang) nen digitalWrite la du va an toan.
static void btnLed(bool on) {
#if defined(LED_GPIO_NUM)
  led_duty = on ? 255 : 0;                             // dong bo "led_intensity" tren /status
  digitalWrite(LED_GPIO_NUM, on ? HIGH : LOW);
#else
  digitalWrite(33, on ? HIGH : LOW);       // den do status tren ESP32-CAM
#endif
}

// Tin hieu trong phien quet (tach 3 chang de doan loi, vd chup cai coc):
//   tich chop 80-100ms sau moi POST 2xx -> "da chup + da gui toi server"
//     (khong noi gi ve nhan dien - ket qua o cac mau duoi)
// Cac mau phep biet cuoi phien:
//   1 nhap dai 800ms  -> diem danh OK (in ten len Serial)
//   2 nhap vua 300ms  -> nhan dien duoc NHUNG da diem danh roi trong buoi nay
//   3 nhap ngan 120ms -> da gui OK nhung khong khop (xem Serial: khong thay mat
//                        / gallery rong / thay mat nhung la)
//   4 nhap ngan 120ms -> khong chup duoc khung nao (loi camera/nguon)
//   5 nhanh 60ms      -> khong gui duoc (loi mang / server: connect fail,
//                        HTTP != 2xx)
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

// Hen gio ket thuc phien quet (goi camSleep() hien la no-op, giu de API on dinh)
static uint32_t g_sleep_at = 0;
static bool g_sleep_req = false;
static void cam_sleep_request() {
  g_sleep_at = millis() + CAM_OFF_DELAY_MS;
  g_sleep_req = true;
}


//  HEARTBEAT: duy tri ket noi voi server Python ma KHONG truyen hinh anh
//  Chi gui 1 HTTP GET nhe (~100 byte text) moi 3s de biet server con song.

static void heartbeatPoll() {
  if (WiFi.status() != WL_CONNECTED || g_is_scanning) return;
  uint32_t now = millis();
  if (g_last_heartbeat_ms != 0 && (now - g_last_heartbeat_ms) < HEARTBEAT_INTERVAL_MS) {
    return;
  }
  g_last_heartbeat_ms = now;

  push_parse_url();
  Client *c = push_client();
  if (!push_connect(c)) {
    if (g_server_online) {
      Serial.printf("[HEARTBEAT] Mat ket noi server %s:%u\n", push_host.c_str(), push_port);
    }
    g_server_online = false;
    c->stop();
    return;
  }

  // Kem thong so board (heap con lai, do manh WiFi) de server hien thi trien
  // ve va chua duoc trang thai "ESP32 dang cho" (nham prompt: 2 ben chi biet
  // nhau dang hoat dong, khong truyen anh).
  char req[224];
  int n = snprintf(req, sizeof(req),
                   "GET /api/esp32/heartbeat?heap=%u&rssi=%d&phase=idle HTTP/1.1\r\n"
                   "Host: %s:%u\r\n"
                   "User-Agent: ESP32-CAM\r\n"
                   "Connection: close\r\n\r\n",
                   (unsigned)ESP.getFreeHeap(), (int)WiFi.RSSI(),
                   push_host.c_str(), push_port);
  if (n > 0) {
    c->write((const uint8_t *)req, n);
  }

  // Doc status line voi timeout 1500ms
  char status_line[48];
  int i = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    while (c->available()) {
      char ch = (char)c->read();
      if (i < (int)sizeof(status_line) - 1) status_line[i++] = ch;
      if (ch == '\n') goto have_hb_line;
    }
    if (!c->connected()) break;
    delay(1);
  }
have_hb_line:
  status_line[i] = 0;
  char *sp = strchr(status_line, ' ');
  int code = sp ? atoi(sp + 1) : 0;

  // Xa nhanh phan hoi con lai
  uint32_t t1 = millis();
  while (millis() - t1 < 100) {
    while (c->available()) { c->read(); t1 = millis(); }
    if (!c->connected() && !c->available()) break;
    delay(1);
  }
  c->stop();

  bool was_online = g_server_online;
  g_server_online = (code == 200);
  if (g_server_online) {
    g_last_hb_ok_ms = millis();   // cho discovery biet "vua online" -> khong doi URL
  }
  if (g_server_online && !was_online) {
    Serial.printf("[HEARTBEAT] Da ket noi server %s:%u (Online - Che do cho, KHONG truyen anh)\n",
                  push_host.c_str(), push_port);
  } else if (!g_server_online && was_online) {
    Serial.printf("[HEARTBEAT] Server phan hoi loi HTTP %d (Offline)\n", code);
  }
}

// Mot lan nhan nut = 1 phien quet: truyen hinh anh LIEN TUC trong 15s toi server.
// Server thuc hien nhan dien va ghi nhan diem danh DUNG 1 LAN trong dot 15s do.
// Ket thuc 15s: den LED bao ket qua tong hop (1 dai = thanh cong, 2 vua = da diem danh,
// 3 ngan = khong khop, 4 = loi camera, 5 = loi mang).
static void doButtonCapture() {
  static char resp[2816];   // header ~256B + body cap ~2.5KB (static -> khong ton stack)

  Serial.printf("[BTN] Nhan nut -> bat dau truyen anh LIEN TUC %d giay toi server de nhan dien\n",
                BUTTON_SCAN_SECONDS);

  // WiFi chua len -> bao ngay bang 5 nhanh
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BTN] loi mang: WiFi CHUA ket noi duoc -> 5 nhanh");
    btnBlink(5, 60, 60);
    return;
  }

  if (!camWake()) {
    Serial.println("[BTN] loi: khong day duoc camera");
    btnBlink(5, 60, 60);
    return;
  }
  g_sleep_req = false;
  if (!push_ensure_buf()) {
    Serial.println("[BTN] loi: khong cap duoc bo dem POST (het RAM/PSRAM)");
    btnBlink(5, 60, 60);
    cam_sleep_request();
    return;
  }

  g_is_scanning = true;

  // Tao ma phien session_id duy nhat cho dot 15 giay nay
  char session_id[40];
  snprintf(session_id, sizeof(session_id), "sess_%lu_%u", (unsigned long)millis(), (unsigned int)random(1000, 9999));

  // Duong dan POST toi server kem capture=1 va session_id
  push_parse_url();
  String scan_path = push_path;
  if (scan_path.indexOf('?') < 0) {
    scan_path += "?capture=1&session_id=";
  } else {
    scan_path += "&capture=1&session_id=";
  }
  scan_path += session_id;

  Client *c = push_client();

  // EP LAI CAU HINH TRUOC KHI QUET (giong ban minimal da chay duoc):
  // VGA + quality 12. Khong doi fb_count/grab_mode tai day (cai nay da co dinh
  // luc init: 1 + WHEN_EMPTY). Neu truoc do web da doi framesize thi ep ve VGA.
  {
    sensor_t *sens = esp_camera_sensor_get();
    if (sens) {
      if (sens->status.framesize != FRAMESIZE_VGA) {
        Serial.println("[BTN] framesize bi doi tren web -> ep ve VGA");
        sens->set_framesize(sens, FRAMESIZE_VGA);
      }
      if (sens->status.quality != 12) sens->set_quality(sens, 12);
      Serial.printf("[BTN] PSRAM: %s, heap: %u, SPIRAM: %u, sensor PID: 0x%04x, session: %s\n",
                    psramFound() ? "co" : "KHONG",
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    sens ? (unsigned)sens->id.PID : 0,
                    session_id);
    }
  }

#if BUTTON_SCAN_FLASH
  btnLed(true);
  delay(250);
#else
  Serial.println("[BTN] BUTTON_SCAN_FLASH=0 -> quet khong flash (can du sang moi truong)");
#endif

  // NONG MAY: doc 3 khung dau tien de KHOI DONG consumer ngay (khong cho
  // FB-OVF). Neu khong co khung nao -> THU CAP NGUON LAI SENSOR (thuong tai
  // luc boot OV5640 bi treo) -> neu van khong thi dung bang 4 nhap.
  if (camWarmup() == 0 && camWarmupRecover() == 0) {
    Serial.println("[BTN] camera KHONG xuat frame -> bo qua dot 15s, 4 nhap");
    camPowerReport();   // in nguyen nhan reset truoc do (co the la SAP NGUON)
    btnLed(false);
    g_is_scanning = false;
    btnBlink(4, 120, 120);
    cam_sleep_request();
    return;
  }

  uint32_t t_end = millis() + (uint32_t)BUTTON_SCAN_SECONDS * 1000UL;
  int frames = 0, fails = 0, last_fail = 0;  // last_fail: 4 = loi LAY KHUNG, 5 = loi GUI
  int best_outcome = 0;   // 0 = chua khop, 1 = diem danh OK, 2 = da diem danh
  char final_name[64];
  final_name[0] = '\0';

  // Truyen hinh anh LIEN TUC trong suot 15 giay (KHONG break som khi khop)
  while ((int32_t)(t_end - millis()) > 0) {
    uint32_t t_get = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    uint32_t dt_get = millis() - t_get;
    if (!fb) {
      Serial.printf("[BTN] loi CAMERA: khong lay duoc khung (fb NULL, doi %lu ms, heap %u)\n",
                    (unsigned long)dt_get, (unsigned)ESP.getFreeHeap());
      fails++;
      last_fail = 4;
    } else if (fb->len > PUSH_JPEG_MAX) {
      Serial.printf("[BTN] loi CAMERA: khung qua lon (%u bytes)\n", (unsigned)fb->len);
      esp_camera_fb_return(fb);
      fails++;
      last_fail = 4;
    } else {
      size_t len = fb->len;
      memcpy(push_buf, fb->buf, fb->len);
      esp_camera_fb_return(fb);   // tra fb ngay
      frames++;

      if (!push_connect(c)) {
        Serial.printf("[BTN] loi mang: khong ket noi duoc %s:%u\n",
                      push_host.c_str(), push_port);
        c->stop();
        fails++;
        last_fail = 5;
      } else {
        bool sent = push_post_frame(c, push_buf, len, scan_path.c_str());
        int code = push_read_response_body(c, resp, sizeof(resp), 3000);
        c->stop();

        if (!sent || code < 200 || code >= 300) {
          Serial.printf("[BTN] loi server: POST that bai (HTTP %d)\n", code);
          fails++;
          last_fail = 5;
        } else {
          fails = 0;   // POST thanh cong
          g_push_ok = g_push_ok + 1;
          g_push_state = 1;

#if BUTTON_SCAN_FLASH
          btnLed(false);
          delay(80);
          btnLed(true);
#else
          btnLed(true);
          delay(100);
          btnLed(false);
#endif

          int already = 0, faces = -1, ids = -1;
          char name[64];
          name[0] = '\0';
          int matched = btn_parse_result(resp, &already, &faces, &ids,
                                         name, sizeof(name));

          if (matched && !already) {
            if (best_outcome != 1) {
              best_outcome = 1;
              strncpy(final_name, name, sizeof(final_name) - 1);
              final_name[sizeof(final_name) - 1] = '\0';
              Serial.printf("[BTN] Khung %d: KHOP -> %s (Diem danh 1 lan cho dot 15s, tiep tuc truyen du 15s)\n",
                            frames, final_name);
            }
          } else if (matched && already) {
            if (best_outcome == 0) {
              best_outcome = 2;
              strncpy(final_name, name, sizeof(final_name) - 1);
              final_name[sizeof(final_name) - 1] = '\0';
              Serial.printf("[BTN] Khung %d: KHOP -> %s (Da diem danh truoc do, tiep tuc truyen du 15s)\n",
                            frames, final_name);
            }
          } else if (ids == 0) {
            Serial.printf("[BTN] Khung %d: server CHUA CO MAU MAT NAO\n", frames);
          } else if (faces <= 0) {
            Serial.printf("[BTN] Khung %d: da gui OK nhung khong thay mat\n", frames);
          } else {
            Serial.printf("[BTN] Khung %d: thay %d mat nhung khong khop\n", frames, faces);
          }
        }
      }
    }

    if (fails >= BUTTON_MAX_FAILS) {
      Serial.println("[BTN] Loi ket noi/camera lien tiep qua nguong -> dung dot quet");
      break;
    }
    delay(BUTTON_POST_GAP_MS);   // nhip quet ~2-3 khung/giay
  }

  btnLed(false);   // tat flash
  g_is_scanning = false;

  // Ngay lap tuc gui 1 heartbeat (phase=idle) de server biet dot 15s da ket thuc
  // -> hien thi lai che do CHO thay vi cho het cua so 16s sau khung cuoi.
  g_last_heartbeat_ms = 0;   // chan buoc: heartbeat tiep theo gui NGAY
  heartbeatPoll();

  int outcome = best_outcome;
  if (outcome == 0 && fails >= BUTTON_MAX_FAILS) {
    outcome = (last_fail == 4) ? 4 : 5;
  }

  if (outcome == 1) {
    Serial.printf("[BTN] Ket thuc 15s (%d khung): Diem danh THANH CONG -> %s\n",
                  frames, final_name[0] ? final_name : "(khong ten)");
    btnBlink(1, 800, 200);            // 1 nhap dai
  } else if (outcome == 2) {
    Serial.printf("[BTN] Ket thuc 15s (%d khung): %s da diem danh roi\n",
                  frames, final_name[0] ? final_name : "Hoc sinh");
    btnBlink(2, 300, 200);            // 2 nhap vua
  } else if (outcome == 4) {
    Serial.println("[BTN] Ket thuc 15s: Loi camera");
    btnBlink(4, 120, 120);            // 4 nhap ngan
  } else if (outcome == 5) {
    Serial.println("[BTN] Ket thuc 15s: Loi mang/server");
    btnBlink(5, 60, 60);              // 5 nhanh
  } else {
    Serial.printf("[BTN] Ket thuc 15s (%d khung): Da truyen du 15s nhung khong khop\n", frames);
    btnBlink(3, 120, 120);            // 3 nhap ngan
  }
  cam_sleep_request();                // het phien quet
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

//  CHE DO AP CAU HINH (xem ghi chu o dau file)
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


//  LENH SERIAL (Serial Monitor, baud 115200, Newline): doi URL server ma
//  khong can sua code / nap lai. Luu vao NVS (giu sau khi mat dien).
//    /url                                  -> in URL server dang dung
//    /url http://<ip-may>:5001/api/esp32/frame
//                                          -> luu URL moi, lan nhan nut ke
//                                             tiep dung ngay (khong reboot)
//    /help                                 -> liet ke lenh

static void serialHandleCmd(char *line) {
  while (*line == ' ' || *line == '\t') line++;   // bo khoang trang dau
  if (*line == '\0') return;
  if (strncmp(line, "/url", 4) == 0
      && (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
    char *arg = line + 4;
    while (*arg == ' ' || *arg == '\t') arg++;
    size_t n = strlen(arg);   // cat khoang trang cuoi (Serial Monitor hay gui them)
    while (n > 0 && (arg[n - 1] == ' ' || arg[n - 1] == '\t')) n--;
    arg[n] = '\0';
    if (*arg == '\0') {   // khong doi so -> in URL hien tai
      Serial.printf("[CFG] URL server dang dung: %s%s\n",
                    cfg.push_url[0] ? cfg.push_url : PUSH_URL,
                    cfg.push_url[0] ? " (da luu NVS)" : " (mac dinh .ino)");
      return;
    }
    if (strncmp(arg, "http://", 7) != 0 && strncmp(arg, "https://", 8) != 0) {
      Serial.println("[CFG] URL phai bat dau bang http:// (khuyen dung) hoac https://");
      return;
    }
    if (strlen(arg) >= sizeof(cfg.push_url)) {
      Serial.println("[CFG] URL qua dai (toi da 127 ky tu)");
      return;
    }
    strncpy(cfg.push_url, arg, sizeof(cfg.push_url) - 1);
    cfg.push_url[sizeof(cfg.push_url) - 1] = '\0';
    cfg_save();                    // luu NVS -> giu sau khi mat dien
    g_push_url = cfg.push_url;     // dong bo trang /status
    Serial.printf("[CFG] Da luu URL server (NVS): %s\n", cfg.push_url);
    Serial.println("[CFG] Lan nhan nut ke tiep dung ngay, khong can reboot.");
    return;
  }
  if (strcmp(line, "/disc") == 0) {
#if DISCOVERY_ENABLE
    g_discLastSend = 0;   // bo qua han che 5s -> hoi toan mang NGAY
    discSend(DISC_MAGIC "|DISCOVER|manual");
    Serial.printf("[DISC] Da gui DISCOVER. URL hien tai: %s (server hien online: %s)\n",
                  cfg.push_url[0] ? cfg.push_url : "(mac dinh)",
                  g_server_online ? "CO" : "KHONG");
#else
    Serial.println("[DISC] Da tat (DISCOVERY_ENABLE=0) - nap lai firmware voi 1 de bat.");
#endif
    return;
  }
  if (strcmp(line, "/cam") == 0) {
    // Kiem tra camera NGAY LUC NAY khong can bam nut: day camera len, doc 3 khung.
    // Ket qua: "3/3 khung co frame" = camera tot; "0/3" = loi PHAN CUNG.
    Serial.println("[CAM] Kiem tra camera (khong can bam nut)...");
    camPowerReport();
    if (!camWake()) {
      Serial.println("[CAM] LOI: khong init duoc sensor (doc khong duoc PID)");
      return;
    }
    int ok = camWarmup();
    if (ok == 0) {
      // Luc boot OV5640 thuong bi treo (doc duoc PID nhung khong xuat frame).
      // Tu cap nguon lai qua PWDN roi thu lai - khong can gao /cam2 nua.
      ok = camWarmupRecover();
      Serial.printf("[CAM] KET QUA sau recovery: %d/3 khung\n", ok);
    }
    Serial.printf("[CAM] KET QUA: %d/3 khung co frame -> %s\n", ok,
                  ok ? "camera TOT, loi o phan mang/server"
                     : "sensor KHONG xuat frame (loi PHAN CUNG: cap nguon/cao cam)");
    return;
  }
  if (strcmp(line, "/camlog") == 0) {
    // *** CANH BAO TRUNG THUC (phan tich 2026-10-26): /camlog KHONG con hieu qua ***
    // Lib esp32-camera precompiled voi CONFIG_LOG_MAXIMUM_LEVEL=1 (chi ERROR):
    //   esp_log_level.h:  #define ESP_LOG_ENABLED(c) (LOG_LOCAL_LEVEL >= cap)
    //   cam_hal.c goi ESP_LOGW(=2)/ESP_LOGI(->3) -> 1 >= 2 = FALSE -> BI BIEN DIET
    //   ngay luc bien dich. esp_log_level_set() chi la phan runtime, khong the
    //   hoi sinh cac loi da bi bo qua. Cac cau can thiet nhat (NO-SOI, NO-EOI,
    //   EV-EOF-OVF, "Failed to get frame: timeout") nen KHONG BAO GIO in.
    // Van chay thu de xac minh (chi con ESP_LOGE), nhung muon chan doan that su
    // hay dung /camdiag: doc I2C truc tiep + do VSYNC/HREF/PCLK, khong qua log.
    Serial.println("[CAM] === /camlog: chi con hieu voi ESP_LOGE (driver chi loi ERROR) ===");
    Serial.println("[CAM] LOG WARN/INFO cua driver da BI BIEN DIET luc build (MAX_LEVEL=1).");
    Serial.println("[CAM] GO /camdiag DE CHAN DOAN THAT SU.");
    esp_log_level_set("*", ESP_LOG_NONE);                 // tat het truoc
    esp_log_level_set("cam_hal", ESP_LOG_VERBOSE);
    esp_log_level_set("ll_cam", ESP_LOG_VERBOSE);
    esp_log_level_set("cam_ccm", ESP_LOG_VERBOSE);
    esp_log_level_set("cam_task", ESP_LOG_VERBOSE);
    Serial.setDebugOutput(true);                           // cho ESP_LOG in ra UART

    if (!camWake()) {
      Serial.println("[CAM] LOI: khong init duoc sensor");
    } else {
      camWarmup();
    }

    Serial.setDebugOutput(false);                          // TAT NGAY
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("cam", ESP_LOG_ERROR);
    Serial.println("[CAM] === DONG LOG CHAN DOAN (da tat VERBOSE) ===");
    return;
  }

  if (strcmp(line, "/camdiag") == 0) {
    // ==========================================================================
    //  CHAN DOAN TRUC TIEP - khong dua vao log cua driver (vi no da bien mat)
    //
    //  3 cau hoi can tra loi, bang cach doc I2C va do cap GPIO truc tiep:
    //   (1) Sensor co DANG CHAY khong? -> doc 0x3008 (02=chay, 42=standby)
    //   (2) Sensor co XUAT VSYNC khong? -> gpio_get_level(25) nhap nhay ~30-60 lan/s
    //       va xem 0x4740/0x4741 co DAO CUC VSYNC/PCLK khong: dao cuc thi ESP32
    //       khong thay canh -> khong bao gio nhan su kien -> 0/3 MA KHONG CO LOI GI.
    //   (3) Chuoi JPEG co chay khong? -> bat colorbar (0x503D=0x80):
    //       colorbar van 0/3 -> loi o cap xuong (sensor/DVP khong dua du lieu)
    //       colorbar -> 3/3     -> sensor va DVP TOT, loi o khung anh that
    // ==========================================================================
    if (!camWake()) {
      Serial.println("[DIAG] khong wake duoc camera -> khong chan doan tiep duoc");
      return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
      Serial.println("[DIAG] sensor NULL");
      return;
    }

    Serial.println("[DIAG] ================== /camdiag ==================");
    Serial.printf("[DIAG] PID=0x%04X  framesize=%d  pixformat=%d  quality=%d\n",
                  (unsigned)s->id.PID, (int)s->status.framesize,
                  (int)s->pixformat, (int)s->status.quality);
    Serial.printf("[DIAG] co khung trong hang: %s\n",
                  esp_camera_available_frames() ? "CO" : "KHONG");

    // (1) doc register quan trong cua OV5640 qua SCCB truc tiep
    static const struct { uint16_t reg; const char *note; } regs[] = {
      {0x3008, "02=chay, 42=standby, 82=sleep"},
      {0x3000, "00 = mo sensor"},
      {0x3004, "mo xuat anh"},
      {0x3005, "chuc nang xuat"},
      {0x3108, "chia PCLK"},
      {0x3035, "PLL ty so truoc"},
      {0x3036, "PLL bo nhan (4C=6x, 64=8x)"},
      {0x3820, "timing (bit1=VFLIP)"},
      {0x3821, "timing (bit1=HMIRROR)"},
      {0x4300, "dinh dang (68=YUV, 10=RAW, 30=RGB)"},
      {0x4740, "<= DAO CUC: 00=thuong, 40=dao VSYNC"},
      {0x4741, "<= DAO CUC: dao PCLK/HREF"},
      {0x4744, "<= DAO CUC nhung dieu kien"},
      {0x503D, "colorbar (80=dang BAT, 00=TAT)"},
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
      int v = (s->get_reg) ? s->get_reg(s, regs[i].reg, 0xFF) : -1;
      if (v < 0) {
        Serial.printf("[DIAG]   0x%04X = doc LOI           ; %s\n",
                      regs[i].reg, regs[i].note);
      } else {
        Serial.printf("[DIAG]   0x%04X = 0x%02X           ; %s\n",
                      regs[i].reg, v & 0xFF, regs[i].note);
      }
    }

    // (1b) doc cac register JPEG con thieu - xem chuoi JPEG duoc bat day du khong
    static const struct { uint16_t reg; uint8_t exp; const char *note; } jregs[] = {
      {0x3034, 0x1A, "10-bit mode (set_pll luon ghi 1A)"},
      {0x3037, 0x02, "PLL pre-div=2, root_2x=0 (set_pll ghi (2&0xf)|0 = 02)"},
      {0x3039, 0x00, "PLLs bypass = 0 (dang dung PLL)"},
      {0x3103, 0x13, "system clock tu PLL (bit1=1)"},
      {0x3824, 0x04, "PCLK_RATIO = pclk_div (JPEG = 4)"},
      {0x460C, 0x22, "VFIFO: PCLK manual + footer off (JPEG = 22)"},
      {0x471C, 0x50, "JPEG dong goi (sensor_fmt_jpeg = 50)"},
      {0x501F, 0x00, "Format select: 00 = YUV422/JPEG"},
      {0x4407, 0x0C, "JPEG quality QS (set_quality 12 = 0C)"},
    };
    for (size_t i = 0; i < sizeof(jregs) / sizeof(jregs[0]); i++) {
      int v = (s->get_reg) ? s->get_reg(s, jregs[i].reg, 0xFF) : -1;
      if (v < 0) {
        Serial.printf("[DIAG]   0x%04X = doc LOI        ; %s\n", jregs[i].reg, jregs[i].note);
      } else {
        Serial.printf("[DIAG]   0x%04X = 0x%02X (ky vong 0x%02X)%s ; %s\n",
                      jregs[i].reg, v & 0xFF, jregs[i].exp,
                      ((v & 0xFF) == jregs[i].exp) ? " " : " <-- LECH!", jregs[i].note);
      }
    }

    // (2a) VONG CHAM (delay 1ms) - cho VSYNC/HREF (tan so thap ~13-30Hz) dung hon
    //      + XCLK lam CHAN DIEU KHIEN: neu XCLK cung it canh -> gpio_get_level doc
    //      khong duoc chan driver -> PCLK=10 la SAO (khong tin duoc); neu XCLK nhieu
    //      canh ma PCLK it -> PCLK THAT SU CHET.
    Serial.println("[DIAG] --- vong CHAM 300ms (XCLK=GPIO0 la control, VSYNC=25, HREF=23, PCLK=22) ---");
    int lv = gpio_get_level((gpio_num_t)VSYNC_GPIO_NUM);
    int lh = gpio_get_level((gpio_num_t)HREF_GPIO_NUM);
    int lp = gpio_get_level((gpio_num_t)PCLK_GPIO_NUM);
    int lx = gpio_get_level((gpio_num_t)XCLK_GPIO_NUM);
    int vs_e = 0, hr_e = 0, pk_e = 0, xk_e = 0, vs_hi = 0, hr_hi = 0, xk_hi = 0;
    uint32_t t0 = millis();
    for (int i = 0; i < 300; i++) {
      int v = gpio_get_level((gpio_num_t)VSYNC_GPIO_NUM);
      int h = gpio_get_level((gpio_num_t)HREF_GPIO_NUM);
      int p = gpio_get_level((gpio_num_t)PCLK_GPIO_NUM);
      int x = gpio_get_level((gpio_num_t)XCLK_GPIO_NUM);
      if (v != lv) { vs_e++; lv = v; if (v) vs_hi++; }
      if (h != lh) { hr_e++; lh = h; if (h) hr_hi++; }
      if (p != lp) { pk_e++; lp = p; }
      if (x != lx) { xk_e++; lx = x; if (x) xk_hi++; }
      delay(1);
    }
    Serial.printf("[DIAG]   sau %ums: XCLK edges=%d (control!) | VSYNC=%d (len=%d) | HREF=%d (len=%d) | PCLK=%d\n",
                  (unsigned)(millis() - t0), xk_e, vs_e, vs_hi, hr_e, hr_hi, pk_e);

    // (2b) VONG NHANH (khong delay) - do XCLK/PCLK cho chu xac, tranh alias 900Hz
    //      vs 11-20MHz. Cang nhieu canh cang tot; PCLK ~ 1/2 XCLK la binh thuong.
    Serial.println("[DIAG] --- vong NHANH khong delay (do XCLK vs PCLK cho chu) ---");
    lx = gpio_get_level((gpio_num_t)XCLK_GPIO_NUM);
    lp = gpio_get_level((gpio_num_t)PCLK_GPIO_NUM);
    int xk_f = 0, pk_f = 0;
    t0 = micros();
    for (int i = 0; i < 40000; i++) {
      int x = gpio_get_level((gpio_num_t)XCLK_GPIO_NUM);
      int p = gpio_get_level((gpio_num_t)PCLK_GPIO_NUM);
      if (x != lx) { xk_f++; lx = x; }
      if (p != lp) { pk_f++; lp = p; }
    }
    uint32_t dtus = micros() - t0;
    Serial.printf("[DIAG]   sau %uus: XCLK canh=%u | PCLK canh=%u (PCLK/XCLK = %.2f)\n",
                  (unsigned)dtus, (unsigned)xk_f, (unsigned)pk_f,
                  (xk_f > 0) ? (float)pk_f / (float)xk_f : 0.0f);

    // (2c) BUS DU LIEU Y2..Y9 - nguyen nhan "truoc chay duoc gio loi": neu 1 chan
    //      du lieu bi KE (FPC lung / han nguoi / chan do), moi byte JPEG bi hong ->
    //      driver soi SOI (FF D8) that bai -> frame bi drop IM LAP (log da bien diet)
    //      -> 0/3 khong co loi gi. Chan song dao dong hang NGHIN lan; chan ke ~0 canh.
    static const struct { gpio_num_t pin; const char *name; } ypins[] = {
      {(gpio_num_t)Y2_GPIO_NUM, "Y2/GPIO5"},  {(gpio_num_t)Y3_GPIO_NUM, "Y3/GPIO18"},
      {(gpio_num_t)Y4_GPIO_NUM, "Y4/GPIO19"}, {(gpio_num_t)Y5_GPIO_NUM, "Y5/GPIO21"},
      {(gpio_num_t)Y6_GPIO_NUM, "Y6/GPIO36"}, {(gpio_num_t)Y7_GPIO_NUM, "Y7/GPIO39"},
      {(gpio_num_t)Y8_GPIO_NUM, "Y8/GPIO34"}, {(gpio_num_t)Y9_GPIO_NUM, "Y9/GPIO35"},
    };
    const int NYP = 8;
    int ylast[8], yedge[8];
    for (int k = 0; k < NYP; k++) { ylast[k] = gpio_get_level(ypins[k].pin); yedge[k] = 0; }
    Serial.printf("[DIAG] --- bus du lieu Y2..Y9 (vong nhanh, ~%uus) ---\n", (unsigned)14000);
    t0 = micros();
    for (int i = 0; i < 40000; i++) {
      for (int k = 0; k < NYP; k++) {
        int v = gpio_get_level(ypins[k].pin);
        if (v != ylast[k]) { yedge[k]++; ylast[k] = v; }
      }
    }
    dtus = micros() - t0;
    int ymax = 0, ymin = 1 << 30, ymini = 0;
    for (int k = 0; k < NYP; k++) {
      if (yedge[k] > ymax) ymax = yedge[k];
      if (yedge[k] < ymin) { ymin = yedge[k]; ymini = k; }
    }
    for (int k = 0; k < NYP; k++) {
      Serial.printf("[DIAG]   %s canh=%-6d%s\n", ypins[k].name, yedge[k],
                    (yedge[k] * 10 < ymax) ? "   <-- GAN NHU KE (chan nay suspect!)" : "");
    }
    Serial.printf("[DIAG]   nhan xet: min=%s (%d canh) / max=%d canh. Neu min ~0 va max nhieu -> CO CHAN DU LIEU KE.\n",
                  ypins[ymini].name, ymin, ymax);
    if (ymin > ymax / 20) {
      Serial.println("[DIAG]   Tat ca 8 chan deu dao dong -> bus du lieu TOT, loi khong phai FPC/chan do.");
    }

    // (3) test colorbar: doi noi dung anh nhung van phai la JPEG hop le
    Serial.println("[DIAG] --- test colorbar ---");
    Serial.printf("[DIAG] truoc colorbar  : %d/3 khung\n", camWarmup());

    if (s->set_colorbar) {
      s->set_colorbar(s, 1);
      int cb = (s->get_reg) ? s->get_reg(s, 0x503D, 0xFF) : -1;
      Serial.printf("[DIAG] da bat colorbar (0x503D doc lai = 0x%02X)\n", cb & 0xFF);
      delay(300);
      Serial.printf("[DIAG] colorbar BAT    : %d/3 khung\n", camWarmup());

      s->set_colorbar(s, 0);
      cb = (s->get_reg) ? s->get_reg(s, 0x503D, 0xFF) : -1;
      Serial.printf("[DIAG] da tat colorbar (0x503D doc lai = 0x%02X)\n", cb & 0xFF);
      delay(300);
      Serial.printf("[DIAG] colorbar TAT    : %d/3 khung\n", camWarmup());
    } else {
      Serial.println("[DIAG] sensor khong ho tro set_colorbar -> bo qua buoc nay");
    }

    Serial.println("[DIAG] --------- CACH DOC KET QUA ---------");
    Serial.println("[DIAG] XCLK~20MHz + PCLK/XCLK~0.3-0.5 -> PLL LOCK, PCLK DUNG. Sensor TOT.");
    Serial.println("[DIAG]   (vong cham do PCLK=10 la SAO do alias 900Hz - KHONG tin vong cham)");
    Serial.println("[DIAG] 0x3037=0x02 la DUNG (set_pll ghi 02) - khong phai LECH");
    Serial.println("[DIAG] neu CA 8 chan Y2..Y9 deu dao dong -> bus du loi TOT, loi o PHIA ESP32");
    Serial.println("[DIAG] neu co chan Y nao ~0 canh -> CHAN DO/FPC HONG -> day la nguyen nhan");
    Serial.println("[DIAG] colorbar BAT van 0/3 + Y ke -> xac nhan loi cap du lieu/chan do");
    Serial.println("[DIAG] colorbar BAT = 3/3   -> sensor + DVP TOT, loi o khung anh that");
    Serial.println("[DIAG] ===================================");
    return;
  }

  if (strcmp(line, "/cam2") == 0) {
    // CAP NGUON LAI SENSOR (chi dung de THU khi /cam tra 0/3).
    //
    // KHONG GOI esp_camera_deinit() - day la loi da gay ra "brick" camera:
    // log 2026-10-20 chung minh 2/2 lan deinit() -> init lai de that bai voi
    //   "cam_hal: cam_dma_config(509): frame buffer malloc failed" (0xffffffff),
    //   trong khi CUNG luc do lan khong qua deinit lai init duoc va 3/3 khung.
    // Ly do: deinit khong giai phong sach bo dem DMA noi bo.
    // Goi ham recovery (PHAN NHANH theo g_cam_on): driver chua init thi cat/cap
    // PWDN + init lai; driver dang chay thi KHONG cat nguon (se mat cau hinh).
    int ok = camWarmupRecover();
    Serial.printf("[CAM] KET QUA sau recovery: %d/3 khung\n", ok);
    if (ok == 0) {
      Serial.println("[CAM] Van 0/3 -> xem dong tren: neu bao RESET board thi reset");
      Serial.println("[CAM] (driver dang chay, khong the cat nguon an toan). Neu van 0/3");
      Serial.println("[CAM] sau reset -> kiem tra: (1) cap FPC cam, (2) nguon 5V/2A.");
    }
    return;
  }
  if (strcmp(line, "/help") == 0) {
    Serial.println("[CFG] Lenh Serial:");
    Serial.println("  /url                    - xem URL server dang dung");
    Serial.println("  /url <http://ip:port/duong-dan> - doi + luu URL server (NVS)");
    Serial.println("  /cam                    - kiem tra camera (3 khung) khong can bam nut");
    Serial.println("  /camlog                 - mo log VERBOSE (chi con ESP_LOGE - xem /camdiag)");
    Serial.println("  /camdiag                - chan doan truc tiep: doc reg OV5640 + do VSYNC/PCLK + colorbar");
    Serial.println("  /cam2                   - cap nguon lai sensor roi kiem tra lai");
#if DISCOVERY_ENABLE
    Serial.println("  /disc                   - hoi toan mang 'ai la server?' ngay");
#endif
    Serial.println("  /help                   - liet ke lenh");
    return;
  }
  Serial.println("[CFG] Lenh khong hieu. Go /help de xem.");
}

static void serialPoll() {
  static char line[160];
  static size_t len = 0;
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[len] = '\0';
      len = 0;
      serialHandleCmd(line);
      continue;
    }
    if (len + 1 < sizeof(line)) line[len++] = ch;   // tran -> bo phan thua
  }
}

void setup() {
  Serial.begin(115200);

  // DA CHAN DOAN XONG (2026-10-12): NGUYEN NHAN "cam_hal: FB-OVF" + fb_get NULL
  // 4000ms chinh la DO NHET UART BOI LOG VERBOSE NAY:
  //  - Mo esp_log_level_set(VERBOSE) cho cac tag cam/cam_hal/ll_cam + dat
  //    Serial.setDebugOutput(true) -> MOI khung hang (~25 fps) sinh vai dong
  //    log blocking tren UART 115200 (~1ms/dong).
  //  - Task camera bi chan ghi log -> khong tra buffer kip -> driver bao
  //    FB-OVF (frame buffer overflow) -> chinh dong spam do lai nghet hon
  //    -> esp_camera_fb_get() het timeout 4000ms -> NULL.
  //  - Ban CamButtonStrip (KHONG co 2 cai nay) chay binh tren CUNG board,
  //    cung sensor OV5640, cung cau hinh VGA/q12/fb1/WHEN_EMPTY.
  // GIU NGUYEN false + khong mo VERBOSE: chi in log cua CHUNG TA (Serial.printf)
  // la du, khong can log driver camera trong suat dong.
  Serial.setDebugOutput(false);
  Serial.println();

  // Doc cau hinh da luu trong NVS (lan dau: mac dinh ssid/password/PUSH_URL
  // o tren file .ino). Sau nay sua tren trang web /setup, khong can code lai.
  Serial.printf("[CFG] Firmware: %s\n", FW_VERSION);
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
  // Camera dang OI tu luc boot, chi day len khi co nguoi yeu cau (nhan nut
  // -> doButtonCapture -> camWake) hoac dev mo /capture, /stream, /live?state=1.
#else
  if (!camWake()) {   // che do cu: loi camera -> dung setup nhu truoc
    return;
  }
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();          // GPIO output thuong (khong LEDC) + tat san
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
    // KHONG ep TxPower max: ban minimal (mac dinh) chay on dinh, con max TX
    // + camera + flash cung luc de gay sut ap tren nguon yeu -> sensor treo.
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
  // (Khac moi mau nhap ket qua -> nhanh nhanh biet board song, ká»ƒ cáº£ khi chua co WiFi.)
  btnLed(true);
  delay(1500);
  btnLed(false);

#if BUTTON_ENABLE
#endif

  // KHONG DAY CAMERA TAI BOOT 
  // Chi init camera ngay truoc khi doc frame (doButtonCapture hoac dev mo
  // /capture, /stream, /live). Camera dang OI se du day len khi can, va se tu
  // "tat" (khong doc frame) sau CAM_OFF_DELAY_MS khi khong con ai su dung.
  // (Luu y: loi truoc day "cam_hal: FB-OVF" KHONG phai do thoi diem init ma do
  // log driver camera VERBOSE nghet UART - xem doan Serial.setDebugOutput.)
  Serial.println("[CAM] Camera dang OI - se day khi nhan nut / dev mo /capture, /stream");

  startCameraServer();

  //  TU TIM IP SERVER (UDP broadcast) 
  // Server Python (discovery.py) moi 5s gui "FACESYNC|SERVER|http://ip:5001" ra
  // cong 4321. Neu IP server doi (DHCP) thi o day tu cap nhat url trong NVS.
  discBegin();
  if (discBootBurst()) {
    Serial.printf("[DISC] Da dung dung URL server tu luc boot: %s\n", cfg.push_url);
  } else {
    Serial.println("[DISC] Chua nghe thay server nao (se hoi lai khi offline)");
  }
  discInfoPoll();   // gui ngay 1 goi "minh la ESP32 nay" -> server biet ip camera

#if PUSH_ENABLE
  // Task day luong live len may chu (uu tien thap hon web server -> khong lam gi stream)
  // BUTTON_ENABLE = 1 -> PUSH_ENABLE da bi tu tat o tren -> khong co task nay.
  if (xTaskCreate(push_task, "cam_push", 8192, NULL, 1, NULL) != pdPASS) {
    Serial.println("[PUSH] khong tao duoc task");
  }
#endif

#if BUTTON_ENABLE
  Serial.printf("[BTN] Che do nut nhan: nut GPIO%d -> GND (nhan xuong = muc thap)\n", BUTTON_PIN);
  Serial.printf("[BTN] LED phep tren GPIO%d, quet toi da %d giay moi lan nhan -> %s (+?capture=1)\n",
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
  Serial.println("[CFG] Go /help tren Serial Monitor de xem lenh (vd /url doi server).");
}

void loop() {
  serialPoll();   // lenh Serial (/url, /help) - khong block, chay moi che do
  discPoll();     // lang nghe server (va hoi toan mang khi offline) - khong block
  discInfoPoll(); // thong bao ip camera cho server moi 15s - khong block
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

  buttonPoll();   // nhan nut -> truyen anh lien tuc 15 giay (block trong phien quet)

  heartbeatPoll(); // duy tri ket noi (khong truyen anh) trong che do cho

  // Nhin tat cam theo lich (hien tai camSleep() la no-op de tranh loi driver;
  // giu lai loi goi de API khong doi).
  if (g_sleep_req && (int32_t)(millis() - g_sleep_at) >= 0 && !g_stream_active) {
    g_sleep_req = false;
    camSleep();
  }
  delay(5);   // nhe CPU, van du do cho debounce 30ms
#else
  heartbeatPoll();
  delay(100);
#endif
}
