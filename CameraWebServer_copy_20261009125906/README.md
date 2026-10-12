# ESP32-CAM + Server nhận diện khuôn mặt (Python)

Thư mục này là firmware ESP32-CAM đã tích hợp sẵn với server Python ở thư mục gốc
(`server.py`). Gồm mẫu `CameraWebServer` của Espressif + **chế độ nút nhấn** (mặc định):
nhấn nút → quét liên tục tối đa 15 giây (POST từng khung lên server, trúng là dừng
sớm) → LED báo kết quả → tắt camera.

## 1. Chế độ hoạt động (chọn 1 trong 3)

| Chế độ | ESP32 (`.ino`) | Server (`.env` / web) | Khi nào dùng |
|---|---|---|---|
| **BUTTON — nút nhấn (mặc định)** | `BUTTON_ENABLE = 1` | Không cần `STREAM_URL`; ESP32 POST `PUSH_URL?capture=1` liên tục tối đa 15 s mỗi lần bấm (trúng là dừng sớm) | Chạy thật: 1 nút = 1 phiên quét điểm danh, cam OFF khi nghỉ |
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

### Luồng 1 lần bấm (quét tối đa 15 giây)

1. `camWake()` — bật nguồn sensor (PWDN GPIO32 = LOW) + `esp_camera_init` (nếu đang OFF).
2. Bật flash → chờ 250 ms (sensor cần LED sáng trước khi chụp) → **giữ flash SÁNG
   suốt phiên quét** (ánh sáng ổn định, đồng thời là tín hiệu "đang quét").
3. Lặp (~2–3 khung/giây, tối đa `BUTTON_SCAN_SECONDS` = 15 giây):
   chụp 1 JPEG → `POST PUSH_URL?capture=1` (multipart/form-data, field `file`,
   boundary `esp32cam`) → đọc JSON phản hồi ngay. Mỗi POST 2xx nháy **tích**
   80–100 ms (báo "đã chụp + đã gửi", tách bạch với kết quả nhận diện).
   - `matched ≥ 1, already:false` → điểm danh OK → **dừng sớm**, 1 nháy dài.
   - `matched ≥ 1, already:true` → đã điểm danh buổi này → **dừng sớm**, 2 nháy.
   - Chưa khớp → quét tiếp; log Serial phân loại: `server CHUA CO MAU` /
     `khong thay mat` (khung trống) / `thay N mat nhung khong khop`.
   - Lỗi chụp liên tiếp (`fb NULL`) → dừng, **4 nháy** (lỗi camera, khác 5 nháy mạng).
   - POST lỗi liên tiếp `BUTTON_MAX_FAILS` (3) lần → dừng, 5 nháy.
4. Tắt flash → nháy LED theo kết quả + in log Serial (ASCII).
5. Giữ cam bật thêm `CAM_OFF_DELAY_MS` (1500 ms) rồi kết thúc phiên quét
   (KHÔNG deinit: driver esp-camera không chịu deinit/reinit giữa chừng —
   init lại sẽ chết `frame buffer malloc failed`; camera init 1 lần duy nhất
   rồi giữ nguyên suốt uptime, sensor chỉ chạy thật khi quét hoặc stream).

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
- Server: có thể hạ `MIN_FACE_SIZE` xuống `24–32` nếu mặt vẫn nhỏ, tăng `DETECT_SCORE`
  nếu nhiều báo giả.
- `PUSH_MODE_MJPEG = 0` cho server Flask (mỗi khung 1 POST field `file`).
  `= 1` (luồng MJPEG chunked) chỉ dùng khi server hiểu MJPEG push — Flask dễ treo.

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

### Sự cố: `chup anh that bai` (fb NULL) + 5 nháy dù WiFi tốt

Init camera OK nhưng sensor không ra khung nào (mỗi lần treo vài giây) — điển
hình là **nguồn nuôi yếu**: flash + WiFi công suất max + camera cùng lúc làm sụt
áp, sensor treo trong khi CPU/WiFi vẫn sống.

1. **Đổi nguồn trước:** sạc điện thoại 5V⎓2A cấp thẳng vào chân **5V + GND**
   (dây ngắn, to), không nuôi qua cáp USB-UART mảnh/cổng laptop yếu.
2. Nạp firmware mới, bấm nút, đọc dòng chẩn đoán:
   `[BTN] PSRAM: co/khong, heap: ..., SPIRAM: ..., sensor PID: 0x...`
   - `PID: 0x0000` → sensor mất giao tiếp (nguồn/dây).
   - `PSRAM: KHONG` → board không có PSRAM hoặc chọn sai board (phải là
     `AI Thinker ESP32-CAM`, PSRAM Enabled).
   - PID `0x2642` (OV2640) + vẫn NULL → nguồn vẫn yếu, làm bước 1 kỹ hơn.
3. Tạm thời đặt `BUTTON_SCAN_FLASH = 0`, nạp lại, quét dưới ánh sáng môi trường
   đủ (giảm dòng tiêu thụ lúc quét).

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
