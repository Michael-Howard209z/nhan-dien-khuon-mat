# ESP32-CAM + Server nhận diện khuôn mặt (Python)

Thư mục này là firmware ESP32-CAM đã tích hợp sẵn với server Python ở thư mục gốc
(`server.py`). Gồm mẫu `CameraWebServer` của Espressif + **chế độ nút nhấn & heartbeat**:
- **Chế độ chờ (idle)**: ESP32-CAM và server KHÔNG truyền hình ảnh, chỉ gửi heartbeat
  nhẹ (`GET /api/esp32/heartbeat`) mỗi 3s để biết nhau đang online/hoạt động.
- **Khi nhấn nút**: ESP32-CAM truyền hình ảnh LIÊN TỤC trong 15 giây đến server.
- **Server nhận diện**: ghi nhận điểm danh đúng 1 LẦN trong đợt 15 giây đó.
- **Hết 15 giây**: đèn LED nháy báo kết quả tổng hợp và trở về chế độ chờ (không truyền ảnh).

## 1. Chế độ hoạt động (chọn 1 trong 3)

| Chế độ | ESP32 (`.ino`) | Server (`.env` / web) | Khi nào dùng |
|---|---|---|---|
| **BUTTON — nút nhấn (mặc định)** | `BUTTON_ENABLE = 1` | Chế độ chờ: không truyền ảnh (chỉ heartbeat 3s). Nhấn nút: truyền ảnh liên tục 15s đến `PUSH_URL?capture=1&session_id=...` (điểm danh 1 lần/phiên) | Chạy thật: tiết kiệm băng thông, truyền ảnh liên tục 15s khi bấm nút |
| **PULL** | `BUTTON_ENABLE = 0`, `PUSH_ENABLE = 0` | `STREAM_URL = http://<ip-esp32>:81/stream` | Test nhanh 1 cam, không sửa firmware nhiều |
| **PUSH** (liên tục) | `BUTTON_ENABLE = 0`, `PUSH_ENABLE = 1`, `PUSH_MODE_MJPEG = 0`, `PUSH_URL = http://<ip-may>:5001/api/esp32/frame` | Không cần `STREAM_URL` | Nhiều cam, NAT, hoặc muốn cam chủ động đẩy liên tục |

- `<ip-esp32>`: xem trên Serial Monitor sau khi nạp (baud 115200). Web ở `:80`, stream ở `:81`.
- `<ip-may>`: IP máy chạy `python server.py` (cùng WiFi với ESP32). `:5001` = `CAMERA_PORT` trong `.env`.
- Khi `BUTTON_ENABLE = 1`, `PUSH_ENABLE` **bị tự động tắt** (không có task push liên tục,
  không stream nội bộ tự chạy) — đúng yêu cầu để chip không bị "đói" tài nguyên.

## 2. Cấu hình WiFi không cần code — chế độ AP tự tạo (trang `/setup`)

Khi ESP32 **không kết nối được WiFi** (sai mật khẩu, đổi mạng, server đổi IP...) nó
**tự phát một mạng WiFi riêng** thay vì chỉ in cảnh báo như trước:

1. Kết nối điện thoại/máy tính vào mạng **`ESP32-CAM-Setup`** (mật khẩu mặc định `12345678`).
2. Điện thoại sẽ **tự mở trang cấu hình** (captive portal); nếu không, mở
   `http://192.168.4.1/` hoặc `http://192.168.4.1/setup`.
3. Trang `/setup` cho phép cài đặt và **lưu vào NVS** (giữ nguyên sau khi mất điện):
   - **Mạng WiFi**: quét mạng, chọn SSID + mật khẩu.
   - **IP tĩnh** (tùy chọn): IP / Gateway / Subnet / DNS (bỏ chọn = DHCP).
   - **Server nhận diện**: URL endpoint (`http://<ip-may>:5001/api/esp32/frame` —
     IP **hoặc tên miền** đều được). Áp dụng ngay, không cần reboot.
   - **Hướng camera**: lật dọc (vflip) / lật ngang (hmirror) — áp dụng ngay, có
     xem ảnh trực tiếp (`:81/stream`) để chỉnh.
   - **Tên/mật khẩu mạng AP** tự tạo (mặc định `ESP32-CAM-Setup` / `12345678`).
   - **Khôi phục mặc định** (xoá cấu hình đã lưu).
4. Bấm **Lưu cấu hình**: nếu đổi mạng/IP/AP thì thiết bị tự khởi động lại (~2 s);
   nếu chỉ đổi hướng camera/URL server thì áp dụng ngay, không reboot.

Một số lưu ý:

- **Vào lại chế độ cấu hình**: *giữ nút nhấn ~1,5 giây ngay lúc boot* (trước khi
  nháy LED báo boot) → vào AP kể cả khi WiFi đang kết nối bình thường.
- `ssid` / `password` / `PUSH_URL` trong `.ino` giờ chỉ là **giá trị mặc định lần
  đầu** (chưa có gì trong NVS) — không cần sửa code nữa.
- **Trang `/setup` cũng mở được khi đã kết nối WiFi** tại `http://<ip-esp32>/setup`.
- API: `GET /api/cfg` (JSON cấu hình), `POST /api/cfg` (lưu, body `key=value&...`,
  `reset=1` = khôi phục mặc định), `GET /api/scan` (quét WiFi).
- **Lệnh Serial** (Serial Monitor, baud 115200, Newline) — đổi URL server mà không
  cần sửa code / nạp lại, lưu vào NVS (giữ sau khi mất điện):
  - `/url` → in URL server đang dùng (kèm nguồn NVS hay mặc định `.ino`).
  - `/url http://<ip-may>:5001/api/esp32/frame` → lưu URL mới, lần nhấn nút kế
    tiếp dùng ngay, không cần reboot. URL phải bắt đầu bằng `http://`/`https://`,
    tối đa 127 ký tự.
  - `/help` → liệt kê lệnh.
- Khi ở chế độ AP, điểm danh **chưa hoạt động** (chưa có mạng tới server) — vào
  `/setup`, bật "Xem ảnh trực tiếp" chỉnh hướng camera trước rồi cài mạng WiFi.

## 3. Chế độ nút nhấn (BUTTON) — đấu nối & hoạt động

### Đấu nối

- **Nút nhấn**: một đầu vào **GPIO13**, đầu còn lại nối **GND**.
  Firmware cấu hình `INPUT_PULLUP`, kích **mức THẤP** (nhả = HIGH, nhấn = LOW), debounce 30 ms.
  - *Sao không dùng GPIO12 (MTDI):* strapping pin — nút kéo LOW lúc boot sẽ đổi điện áp
    điều khiển SPIRAM (1.8V) → boot lỗi / mất PSRAM.
  - *Sao không dùng GPIO16/17:* đường PSRAM của ESP32-CAM. *GPIO4:* đã dùng cho flash.
    *GPIO0:* strapping boot mode (kéo LOW = vào chế độ nạp). *GPIO1/3:* UART.
- **LED báo kết quả**: dùng **flash LED GPIO4** (AI Thinker); nếu board không có
  `LED_GPIO_NUM` thì dùng đèn đỏ status **GPIO33**.
- Nút và server phải ở trạng thái bình thường; nút chỉ có tác dụng khi ESP32 đã kết nối WiFi
  (firmware kết nối WiFi ngay khi boot, có vòng lặp chờ).

### Luồng 1 lần bấm (truyền ảnh liên tục 15 giây)

1. `camWake()` — bật nguồn sensor (PWDN GPIO32 = LOW) + `esp_camera_init` (nếu chưa init).
2. Tạo mã phiên `session_id` ngẫu nhiên duy nhất cho phiên 15s này.
3. Bật flash (nếu `BUTTON_SCAN_FLASH=1`) hoặc dùng ánh sáng phòng.
4. Lặp liên tục (~2–3 khung/giây) trong suốt **15 giây**:
   chụp JPEG → `POST PUSH_URL?capture=1&session_id=<sess_id>` (multipart/form-data).
   - Server nhận diện khuôn mặt và **ghi nhận điểm danh đúng 1 LẦN** trong suốt phiên 15s đó.
   - Các khung hình tiếp theo trong cùng phiên 15s vẫn được gửi bình thường để server xử lý và hiển thị, không bị ghi đè hay nhân bản điểm danh (`already:true`).
   - ESP32 KHÔNG dừng sớm mà **tiếp tục truyền đủ 15 giây** để đảm bảo đầy đủ hình ảnh.
   - Nếu lỗi kết nối liên tiếp quá 3 lần (`BUTTON_MAX_FAILS`) → dừng sớm và báo lỗi.
5. Tắt flash → nháy LED theo kết quả tổng hợp của cả đợt 15s:
   - 1 nháy dài: điểm danh thành công.
   - 2 nháy vừa: đã điểm danh trước đó.
   - 3 nháy ngắn: đã truyền đủ 15s nhưng không khớp ai.
   - 4 nháy: lỗi camera; 5 nháy: lỗi mạng/server.
6. Trở lại chế độ chờ (không truyền ảnh, tiếp tục heartbeat mỗi 3s).

### Ý nghĩa nháy LED (sau mỗi lần bấm)

Tách 3 chặng để đoán lỗi (vd chĩa camera vào cái cốc): **tích** = đã chụp + đã
gửi tới server (chưa nói gì về nhận diện) → **mẫu nháy** = kết quả.

| Mẫu nháy | Ý nghĩa |
|---|---|
| **Chớp tắt 80–100 ms sau mỗi khung gửi thành công** | Đã chụp + đã gửi (tích). Thấy tích đều mà cuối 3 nháy = camera + mạng tốt, chỉ không khớp mặt |
| **Sáng cố định ~1,5 s khi boot** | Firmware đã chạy, sẵn sàng (khác mọi mẫu nháy kết quả) |
| **1 nháy dài ~800 ms** | Điểm danh OK — log Serial in `[BTN] Diem danh OK: <họ tên>` |
| **2 nháy vừa ~300 ms** | Đã nhận diện được nhưng **đã điểm danh rồi** trong buổi này (`already:true`) |
| **3 nháy ngắn ~120 ms** | Đã gửi OK nhưng không khớp — xem Serial: `khong thay mat` (khung trống/chụp vật) · `server CHUA CO MAU` (chưa đăng ký ai) · `thay N mat nhung khong khop` (người lạ) |
| **4 nháy ngắn ~120 ms** | Không chụp được khung nào (lỗi camera/nguồn — không hề có tích nào trước đó) |
| **5 nháy nhanh ~60 ms** | Không gửi được (lỗi mạng / lỗi server: connect fail, HTTP != 2xx, **hoặc WiFi chưa kết nối**) |

> **WiFi chưa lên sau 20 s → firmware tự chuyển sang chế độ AP cấu hình** (mục 2):
> kết nối vào mạng `ESP32-CAM-Setup` rồi mở `http://192.168.4.1/` để sửa WiFi/server.
> Bấm nút trong lúc này vẫn báo **5 lần nháy** (loi mang) vì chưa gửi lên được server.

Phản hồi server (HTTP 200, JSON top-level ASCII):

```json
{"ok":true,"matched":1,"already":false,"full_name":"Nguyen Van A", ...}   // -> 1 nháy dài
{"ok":true,"matched":1,"already":true, ...}                               // -> 2 nháy
{"ok":true,"matched":0,"already":false,"full_name":"", "count":0, ...}    // -> 3 nháy
```

## 4. Web điều khiển (`:80`) — dành cho developer

Web server trên cổng 80 **vẫn chạy** trong chế độ nút nhấn để cấu hình camera:

- `/` — trang cấu hình (độ phân giải, chất lượng, flash, xoay ảnh).
- `/status` — JSON trạng thái; ngoài các trường cũ thêm `"cam_on":0|1` (đã init
  sensor chưa) và `"button":<GPIO>` (13, hoặc -1 nếu tắt chế độ nút). Cam chỉ ở
  `cam_on:0` từ lúc boot đến lần quét/xem đầu tiên — sau đó luôn `1` (driver
  không cho deinit/reinit giữa chừng nên giữ nguyên suốt uptime).
- `/control?var=...&val=...` — chỉnh sensor (trả 503 nếu cam chưa từng bật).
- `/flash?state=1|0` — bật/tắt đèn flash.
- `/capture` — chụp 1 ảnh JPEG (tự `camWake()` nếu cam chưa bật; lỗi thì trả 503).
- `/stream` (cổng 81) — stream MJPEG; **tự bật cam** khi có người mở. Request tắt
  cam trong lúc stream chạy bị bỏ qua (`camSleep()` hiện là no-op để tránh lỗi
  driver deinit/reinit).
- **`GET /live?state=1`** → bật cam (init lần đầu nếu chưa có); **`GET /live?state=0`**
  hiện không tắt được (giữ nguyên để tránh lỗi driver — muốn tắt hoàn toàn thì
  reset board).
  Trả `{"ok":1,"cam":0|1}` — dùng cho panel debug của server Python.

## 5. Cấu hình khuyến dùng cho nhận diện

- Độ phân giải: **VGA 640×480** (mặc định firmware — vừa chứng minh chạy ổn định
  trên OV5640, vừa nhận xa hơn QVGA). Đừng chuyển xuống QVGA sau init (treo
  sensor OV5640, fb NULL). Chế độ nút nhấn quét ~2–3 khung/giây trong tối đa
  15 s nên dư sức chạy VGA.
- Chất lượng JPEG: `12` (10 = nét, 20+ = nhẹ/nhanh).
- `CAM_FB_COUNT` / `CAM_GRAB_LATEST` (đầu file `.ino`): mặc định **1 + WHEN_EMPTY**
  — đúng cấu hình của bản chẩn đoán `CamButtonStrip` đã chạy được trên chính
  board này. Chỉ đổi sang `2 + LATEST` khi cần mở `:81/stream` trong lúc bấm nút
  (2 consumer cùng giữ frame buffer).
- Server: có thể hạ `MIN_FACE_SIZE` xuống `24–32` nếu mặt vẫn nhỏ, tăng `DETECT_SCORE`
  nếu nhiều báo giả.
- `PUSH_MODE_MJPEG = 0` cho server Flask (mỗi khung 1 POST field `file`).
  `= 1` (luồng MJPEG chunked) chỉ dùng khi server hiểu MJPEG push — Flask dễ treo.

### Thang chẩn đoán khi gặp fb NULL (0/3 khung)

**Nguyên nhân thật đã chốt (2026-10-12): `cam_hal: FB-OVF` + `fb_get` NULL 4000 ms
là do bật log driver camera VERBOSE** (`esp_log_level_set(...VERBOSE)` × 5 +
`Serial.setDebugOutput(true)`): mỗi khung ~25 fps sinh vài dòng log blocking trên
UART 115200 (~1 ms/dòng) → task camera không trả buffer kịp → driver báo frame
buffer overflow → chính dòng spam FB-OVF làm nghẹt thêm → `fb_get` timeout.
Bản tối giản `CamButtonStrip` (không có 2 cài đặt này) chạy tốt trên **cùng** board,
cùng sensor OV5640, cùng VGA/q12/fb1. Firmware đã tắt lại (`setDebugOutput(false)`,
không bật VERBOSE) và đưa `CAM_FB_COUNT=1` + `CAM_GRAB_LATEST=0`.

Nếu sau này cần chẩn đoán lại, chỉ bật log theo **thời điểm** (ngay trước khi
`fb_get`), không bật VERBOSE suốt dòng:

| Bước | Xem gì | Kết luận |
|---|---|---|
| 1. `[CAM] init: VGA/q12/fb_count=1/grab=WHEN_EMPTY/PSRAM=co` + `[CAM] nong may 3/3 OK` | Camera ấm máy tốt khi bấm nút | Camera OK, sang bước network/server |
| 2. `[BTN] POST ... HTTP 200` | Server trả JSON có `results`/`matched` | Toàn bộ pipeline thông |
| 3. Nạp `CamButtonStrip/CamButtonStrip.ino` (bản tối giản, cùng pin + cùng VGA/q12/fb1) | `POST 200` vs `fb NULL` | `POST 200` = phần cứng tốt (lỗi ở firmware chính); `fb NULL` = **nguồn/dây/cam**, sửa phần cứng |

## 6. Quay lại hành vi cũ / tắt các tính năng mới

- `BUTTON_ENABLE = 0` → hết chế độ nút nhấn; `loop()` trở về `delay(10000)` như cũ,
  cam được init ngay khi boot, `/live` vẫn hoạt động.
- `PUSH_ENABLE = 1` (và `BUTTON_ENABLE = 0`) → bật lại task PUSH liên tục như cũ
  (PUSH_MODE_MJPEG = 0 POST từng khung, = 1 đẩy luồng MJPEG).
- `PUSH_ENABLE = 0` + `BUTTON_ENABLE = 0` → PULL (server kéo `:81/stream`).
- `CAM_OFF_DELAY_MS` — thời gian giữ cam sau khi gửi xong (mặc định 1500 ms).
- `BUTTON_PIN` — GPIO của nút (mặc định 13, kéo nội lên, kích mức thấp).
- `BUTTON_SCAN_FLASH` — `1` = giữ flash sáng khi quét (mặc định); `0` = quét không
  flash, dùng khi **nguồn yếu** (xem mục sự cố dưới).

### Sự cố: 4 nháy "lỗi camera" (fb NULL) dù WiFi tốt

`esp_camera_fb_get()` luôn trả NULL nghĩa là **sensor không xuất frame** (không
phải lỗi mạng — mạng hỏng sẽ là 5 nháy). Firmware mới in rõ nguyên nhân ở 2 dòng:

- Lúc boot: `[CAM] nong may khung 1: OK/NULL (doi ... ms)` → `3/3 khung` là camera tốt.
- Lúc bấm nút: `[BTN] loi CAMERA: khong lay duoc khung (fb NULL, doi ... ms, heap ...)`.

Khi 3/3 NULL: kiểm tra theo thứ tự

1. **Đổi nguồn trước:** sạc điện thoại 5V⎓2A cấp thẳng vào chân **5V + GND**
   (dây ngắn, to), không nuôi qua cáp USB-UART mảnh/cổng laptop yếu.
2. Đọc dòng chẩn đoán:
   `[BTN] PSRAM: co/khong, heap: ..., SPIRAM: ..., sensor PID: 0x...`
   - `PID: 0x0000` → sensor mất giao tiếp (nguồn/dây).
   - `PSRAM: KHONG` → board không có PSRAM hoặc chọn sai board (phải là
     `AI Thinker ESP32-CAM`, PSRAM Enabled).
   - PID `0x2642` (OV2640) + vẫn NULL → nguồn vẫn yếu, làm bước 1 kỹ hơn.
3. Tạm thời đặt `BUTTON_SCAN_FLASH = 0`, nạp lại, quét dưới ánh sáng môi trường
   đủ (giảm dòng tiêu thụ lúc quét).
4. Nếu PID đọc được (`0x5640` OV5640) mà vẫn 3/3 NULL → nghi **XCLK (GPIO0) bị
   chiếm**: tuyệt đối không gọi `ledcAttach`/`analogWrite` ở bất kỳ chân nào
   khi camera đang chạy (LEDC timer 0 là của XCLK 20 MHz).

## 7. Nạp firmware

1. Arduino IDE / arduino-cli: board `AI Thinker ESP32-CAM` (PSRAM Enabled), partition `custom` (có `partitions.csv`).
2. Nạp (không bắt buộc phải sửa `ssid`/`password`/`PUSH_URL` nữa — đó chỉ là giá trị
   mặc định; cài đặt thực tế làm trên trang `/setup`, xem mục 2).
3. Mở Serial 115200 lấy IP → mở `http://<ip-esp32>/` chỉnh flash / độ phân giải,
   `http://<ip-esp32>/setup` cấu hình mạng + server + hướng camera.
4. Kiểm tra: bấm nút → log Serial `[BTN] Diem danh OK: ...` + LED nháy 1 lần dài;
   server nhận `POST /api/esp32/frame?capture=1`.
5. *Quên WiFi / muốn cài lại*: giữ nút ~1,5 s lúc boot → vào mạng `ESP32-CAM-Setup`
   rồi mở `http://192.168.4.1/` (xem mục 2).

## 8. File trong thư mục

- `CameraWebServer_copy_*.ino` — cấu hình WiFi/PUSH/BUTTON, chế độ AP tự tạo
  (`startSetupMode()` + captive portal), `camWake()`/`camSleep()`,
  vòng lặp poll nút nhấn + quét POST liên tục tối đa 15 s (nháy LED phản hồi).
- `app_config.h` / `app_config.cpp` — lưu cấu hình persistent trong NVS
  (WiFi, IP tĩnh, URL server, hướng camera, AP), `cfg_load()`/`cfg_save()`/`cfg_reset()`.
- `setup_page.h` — trang HTML `/setup` (cấu hình WiFi + server + hướng camera).
- `app_httpd.cpp` — web `:80` + stream `:81/stream` + `/capture` + `/status`
  (có `push`, `push_ok`, `push_err`, `cam_on`, `button`) + `/live?state=0|1`,
  tự `camWake()` khi mở `/capture`, `/stream`, `/bmp`,
  thêm `/setup` + `/api/cfg` + `/api/scan` (captive portal redirect).
- `board_config.h` / `camera_pins.h` — chọn `CAMERA_MODEL_AI_THINKER`.
- `partitions.csv`, `ci.json` — cấu hình build.
