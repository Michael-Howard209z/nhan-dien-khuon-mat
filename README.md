# 📷 Server Nhận Diện Khuôn Mặt (Python + Điện thoại camera IP → chuẩn bị cho ESP32)

Server Python xử lý hình ảnh khuôn mặt:

- **Nguồn ảnh hiện tại:** điện thoại Android làm camera IP (app *IP Webcam* / *DroidCam*) truyền luồng MJPEG qua WiFi.
- **Nguồn ảnh sau này:** ESP32 (ESP32-CAM) POST JPEG tới `/api/esp32/frame` — endpoint đã sẵn sàng.
- **Thông tin lưu trữ:** Họ và tên, Ngày sinh, Lớp (SQLite) + mẫu ảnh khuôn mặt.
- **Nhận diện:** OpenCV Haar (phát hiện) + LBPH (nhận dạng), nhãn vẽ tiếng Việt có dấu.

## 1. Cài đặt (máy tính Windows/Linux)

```bat
cd face_server
pip uninstall -y opencv-python opencv-contrib-python   # tránh xung đột module
pip install -r requirements.txt
```

> Bắt buộc dùng **opencv-contrib-python** (đủ module `cv2.face` cho LBPH).

## 2. Dùng điện thoại làm camera IP

Máy này đang cấu hình sẵn cho **DroidCam** (trong file `.env`):

- Điện thoại: mở app **DroidCam** → **Start** (IP `10.173.125.108`, port `4747`).
- Máy tính và điện thoại phải **cùng một mạng WiFi**.
- URL đã đặt: `STREAM_URL=http://10.173.125.108:4747/video` (DroidCam dùng `/video`)

Nếu đổi app hoặc đổi IP → sửa `STREAM_URL` trong file `.env` (mục 3)
hoặc nhập URL mới trên trang web → **Kết nối** (sẽ tự ghi lại vào `.env`).

Các URL khác:

| App / nguồn        | URL                          |
|--------------------|------------------------------|
| IP Webcam          | `http://IP:8080/video`       |
| IP Webcam (1 ảnh)  | `http://IP:8080/shot.jpg`    |
| DroidCam           | `http://IP:4747/video`       |
| Webcam USB         | `0`                          |

## 3. Cấu hình trong file .env

Toàn bộ cấu hình nằm trong **`.env`** (bản mẫu: `.env.example`):

| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `STREAM_URL` | `http://10.173.125.108:4747/video` | Luồng camera điện thoại (DroidCam). Để trống = tắt camera. Webcam USB: `0` |
| `HOST` | `0.0.0.0` | Địa chỉ server lắng nghe |
| `PORT` | `5000` | Cổng server |
| `PROCESS_FPS` | `12` | Số khung hình xử lý mỗi giây |
| `JPEG_QUALITY` | `85` | Chất lượng JPEG khung hình phát ra |
| `MATCH_THRESHOLD` | `75` | Ngưỡng nhận diện LBPH (nhỏ = nghiêm ngặt hơn). Đo thực tế: cùng người ~60, người khác ~89 |
| `LOG_INTERVAL` | `8` | Giây giữa 2 lần ghi log cho cùng một người |

- Sửa `.env` → khởi động lại: `python server.py`
- Đổi trên trang web (mục *Kết nối camera* / API `POST /api/config`) → tự ghi lại vào `.env` và áp dụng ngay.

## 4. Chạy server

```bat
python server.py
```

Mở trang quản lý: **http://localhost:5000** (máy khác trong mạng: `http://<ip-máy>:5000`).

## 5. Đăng ký & nhận diện

1. Nhập **họ và tên**, **ngày sinh**, **lớp**.
2. Bấm **📷 Chụp 5 mẫu** (hoặc chọn ảnh có sẵn) — nên ≥ 3 mẫu, ánh sáng tốt.
3. Bấm **💾 Lưu đăng ký** — model tự huấn luyện lại.
4. Trang video sẽ khoanh vùng và hiển thị: `Họ tên | Lớp | Ngày sinh (điểm)`.
5. Nhật ký nhận diện tự ghi lại (mỗi người tối đa 1 bản ghi / 8 giây).

**Nhận diện tốt ở khoảng cách xa:** LBPH chỉ nhận ra cự ly mà nó đã học.
Ngoài việc chụp lúc ngồi gần, hãy bấm **+Mẫu** (hoặc "Chụp 5 mẫu") thêm 1 lần khi
ngồi **lùi ra 2–3 m** (mặt trong khung ~100–140 px). Thực tế: với 6 mẫu xa,
điểm của cùng người giảm từ ~82 xuống ~45 → nhận ra xa mà vẫn không nhầm người khác.
Công cụ đo/ngộ: `python tune_threshold.py` (in phân bố điểm cùng người vs người khác),
`python add_samples_far.py <id> <số mẫu>` (thêm mẫu cự ly xa, tự bỏ khung của người khác).

API cho client khác (Postman, app Android...):

| Method | Endpoint                        | Mô tả                              |
|--------|---------------------------------|------------------------------------|
| GET    | `/api/status`                   | Trạng thái camera + kết quả mới    |
| GET    | `/api/students`                 | Danh sách người đăng ký            |
| POST   | `/api/students`                 | Đăng ký (JSON: full_name, date_of_birth, class_name, images[]) |
| PUT    | `/api/students/<id>`            | Sửa thông tin                      |
| DELETE | `/api/students/<id>`            | Xoá người + mẫu ảnh                |
| POST   | `/api/students/<id>/samples`    | Thêm mẫu ảnh                       |
| POST   | `/api/recognize`                | Nhận diện trên 1 ảnh gửi lên       |
| POST   | `/api/esp32/frame`              | **ESP32** POST JPEG → JSON kết quả |
| GET    | `/api/log`                      | Nhật ký nhận diện                  |
| GET    | `/video_feed`                   | Luồng MJPEG đã khoanh vùng         |

## 6. Sau này khi có ESP32 (ESP32-CAM)

Endpoint `/api/esp32/frame` đã sẵn: nhận JPEG (body bytes, `Content-Type: image/jpeg`)
trả về JSON `{results: [{full_name, date_of_birth, class_name, confidence, match, box}]}`.

Ví dụ code Arduino (ESP32-CAM):

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

const char* ssid = "WIFI_CUA_BAN";
const char* pass = "MAT_KHAU";
const char* server = "http://192.168.1.10:5000/api/esp32/frame";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  // ... init camera (esp_camera_config_t) như mẫu ESP32-CAM ...
}

void loop() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) {
    HTTPClient http;
    http.begin(server);
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.POST(fb->buf, fb->len);     // gửi trực tiếp buffer JPEG
    if (code == 200) Serial.println(http.getString());  // JSON kết quả
    http.end();
    esp_camera_fb_return(fb);
  }
  delay(300);   // ~3 fps
}
```

Khi ESP32 đang gửi khung hình, server ưu tiên nguồn ESP32; nếu ESP32 ngừng gửi
> 5 giây, server tự quay lại dùng camera điện thoại.

## 7. Cấu trúc thư mục

```
face_server/
├── .env               # TOÀN BỘ cấu hình (URL camera, cổng, ngưỡng...)
├── .env.example       # bản sao cấu hình mẫu
├── server.py          # Flask app + API + vòng xử lý khung hình
├── face_engine.py     # detect (Haar) + recognize (LBPH) + vẽ nhãn
├── stream.py          # đọc MJPEG từ điện thoại / webcam
├── database.py        # SQLite: students + recognition_log
├── requirements.txt
├── test_api.py        # 28 kiểm thử API (chạy server trước)
├── probe_camera.py    # chẩn đoán endpoint camera
├── grab_live.py       # tải 1 khung từ /video_feed để xem
├── tune_threshold.py  # đo phân bố điểm LBPH, gợi ý ngưỡng
├── add_samples_live.py    # thêm N mẫu từ camera cho 1 người
├── add_samples_far.py     # thêm mẫu cự ly xa (lọc theo vị trí mặt)
├── templates/index.html   # trang quản lý
└── data/
    ├── students.db    # tự tạo
    └── faces/<id>/*.jpg   # mẫu ảnh mặt
```

## 8. Xử lý sự cố

| Vấn đề | Cách sửa |
|---|---|
| `Thiếu module cv2.face` | `pip uninstall opencv-python` rồi `pip install opencv-contrib-python` |
| Không thấy luồng video | Check cùng WiFi, mở `http://ip-phone:4747` (DroidCam) / `:8080/video` (IP Webcam) trên trình duyệt, tắt防火 wall cổng 4747/8080/5000 |
| Nhận diện sai / "Chưa nhận diện" | Thêm nhiều mẫu hơn (+Mẫu), chụp ở **nhiều khoảng cách** (gần + xa — xem mục 5), ánh sáng đều; tăng `MATCH_THRESHOLD` trong `.env` (75 → 80) nếu vẫn miss. Đo bằng `python tune_threshold.py` |
| Match nhầm người khác | Giảm `MATCH_THRESHOLD` (dùng `tune_threshold.py` xem chênh lệch), thêm nhiều mẫu chuẩn của người thật |
| Chậm | Giảm `PROCESS_FPS` trong `.env`, hoặc giảm độ phân giải trong app camera (512–640 px là đủ) |
