# 📷 Server Điểm Danh Khuôn Mặt (ESP32-CAM + Python)

Hệ thống điểm danh bằng khuôn mặt cho trường học:

- **Điểm danh bằng nút bấm trên ESP32-CAM:** học sinh nhấn nút → thiết bị quét liên
  tục tối đa 15 giây (giữ flash sáng, POST từng khung lên server, trúng là dừng
  sớm) → server nhận diện, ghi điểm danh **theo buổi** (sáng/chiều) → ESP32 nháy
  LED báo kết quả rồi **tắt camera**.
- **Đăng ký khuôn mặt:** giáo viên / quản trị **upload ảnh lên** (không chụp tại camera).
- **Một trang đăng nhập duy nhất** (`:5000`) — server tự điều hướng theo vai trò:
  **admin** (quản trị toàn trường) · **teacher** (giáo viên, chỉ lớp được gán) ·
  **developer** (trang debug ở cổng riêng `:5001`).
- **Không còn vai trò phụ huynh** (không còn cột SĐT phụ huynh).
- **Camera preview là công cụ debug của developer** — xem luồng, cấu hình engine,
  cài đặt ESP32. Preview **không tự điểm danh**.
- **Nhận diện:** YuNet (phát hiện) + SFace (nhận dạng), nhãn tiếng Việt, mỗi người
  một **ô vuông** có mã ID theo dõi riêng.
- **Lưu trữ:** SQLite (học sinh, lớp + lịch học, điểm danh theo buổi, ghi chú nghỉ,
  tài khoản) + mẫu ảnh khuôn mặt đã căn 112×112.

## 1. Cài đặt (máy tính Windows/Linux)

```bat
cd nhan-dien-khuon-mat-main
pip uninstall -y opencv-python opencv-contrib-python   # tránh xung đột module
pip install -r requirements.txt
```

> Bắt buộc dùng **opencv-contrib-python** (đủ module `cv2.face`, `FaceDetectorYN`,
> `FaceRecognizerSF` và các bộ tracker).

Lần chạy đầu tiên, server **tự tải model** về `data/models/`:

| Model | Vai trò | Nguồn |
|---|---|---|
| `face_detection_yunet_2023mar.onnx` (~230 KB) | phát hiện mặt + 5 điểm mốc | opencv_zoo |
| `face_recognition_sface_2021dec.onnx` (~38 MB) | vector đặc trưng 128 chiều | opencv_zoo |

Không có mạng? copy sẵn 2 file `.onnx` vào `data/models/`.
Không tải được model SFace? hệ thống tự chuyển sang **LBPH** cũ (ngưỡng đơn vị khác, xem mục 3).

## 2. Luồng điểm danh (chạy thật)

```
[Học sinh bấm nút trên ESP32-CAM]
   │  (BUTTON_PIN=13, kéo nội lên, kích mức thấp)
   ▼
ESP32: khởi động camera → bật flash (giữ SÁNG suốt phiên quét = tín hiệu "đang quét")
   │
   ▼
Quét tối đa 15 giây (~2–3 khung/giây):
  mỗi khung → POST http://<ip-may>:5001/api/esp32/frame?capture=1 (multipart, field "file")
    → server nhận diện khung đó → trả JSON ngay
    → TRÚNG (matched ≥ 1) → DỪNG SỚM, không cần hết 15 giây
   │
   ▼
Server: nhận diện → tìm trong DB → ghi điểm danh 1 dòng cho BUỔI hiện tại
   │        (lớp 1 buổi/ngày → luôn buổi 1; lớp 2 buổi → theo khung giờ)
   │        (khung lặp lại trong cùng buổi → server tự loại trùng, trả already:true)
   ▼
Trả JSON: {ok, matched, already, full_name, class_name, session, session_name, results}
   │
   ▼
ESP32: tắt flash → nháy LED báo kết quả → giữ cam thêm 1,5 s → TẮT camera (sensor power down)
```

### Ý nghĩa nháy LED

| Mẫu nháy | Ý nghĩa |
|---|---|
| Flash **sáng liên tục** | Đang quét (tối đa 15 giây) — đứng yên trước camera |
| **1 nháy dài ~800 ms** | Điểm danh OK (`matched ≥ 1, already:false`) |
| **2 nháy vừa ~300 ms** | Đã nhận ra nhưng **đã điểm danh buổi này rồi** (`already:true`) |
| **3 nháy ngắn ~120 ms** | Hết 15 giây mà không thấy mặt / không khớp ai |
| **5 nháy nhanh ~60 ms** | Lỗi mạng / lỗi server / POST lỗi liên tiếp |

### Điểm danh theo buổi — tối đa 1 lần/buổi

- Bảng `attendance` có khoá `(student_id, day, session)` — mỗi học sinh mỗi buổi
  **chỉ 1 dòng**, dù bấm nút bao nhiêu lần (các lần sau chỉ cộng cột `hits` và trả
  `already:true` cho thiết bị).
- Khung giờ do admin cấu hình ở tab **Lớp & lịch học** (1 hoặc 2 buổi/ngày).
  Lớp 1 buổi → mọi giờ điểm danh đều tính buổi 1.
- Preview / nhận diện thường (**không** qua `?capture=1`) **không** ghi điểm danh.

## 3. Cấu hình trong file `.env`

Toàn bộ cấu hình nằm trong **`.env`** (copy từ `.env.example`):

| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `STREAM_URL` | *(URL camera)* | Luồng preview cho developer (điện thoại / PULL ESP32). Để trống = không preview. Webcam USB: `0` |
| `HOST` | `0.0.0.0` | Địa chỉ server lắng nghe |
| `PORT` | `5000` | **Cổng quản lý** — 1 trang đăng nhập duy nhất → admin/giáo viên vào `/quan-ly` |
| `CAMERA_PORT` | `5001` | **Cổng debug** — trang preview + cấu hình engine + cài ESP32 (**chỉ developer**). `POST /api/esp32/frame` và `GET /api/ping` mở cho thiết bị |
| `ADMIN_PASSWORD` | `admin` | Mật khẩu tài khoản `admin` (**đổi trước khi đưa lên mạng trường**) |
| `DEV_USERNAME` | `developer` | Tên đăng nhập tài khoản developer |
| `DEV_PASSWORD` | `developer` | Mật khẩu tài khoản developer (mở cổng debug) |
| `PROCESS_FPS` | `12` | Số khung hình xử lý mỗi giây |
| `JPEG_QUALITY` | `85` | Chất lượng JPEG khung hình preview |
| `LOG_INTERVAL` | `8` | Khoảng chờ tối thiểu (giây) giữa 2 lần ghi nhật ký cùng một người (điểm danh theo buổi vẫn tối đa 1 dòng/buổi) |
| `MATCH_THRESHOLD` | `0.42` | **Khoảng cách** tối đa vẫn coi là khớp (nhỏ = nghiêm ngặt) |
| `MATCH_MARGIN` | `0.10` | Độ lệch tối thiểu so với ứng viên thứ hai |
| `DETECT_SCORE` | `0.62` | Ngưỡng chắc chắn của detector YuNet (0–1) |
| `MIN_FACE_SIZE` | `44` | Bỏ qua mặt nhỏ hơn (px) — giảm nhận nhầm, tăng tốc |
| `TRACK_ENABLED` | `1` | Giữ ID người + ô vuông qua các khung mất dặt |
| `TRACK_CONFIRM` | `3` | Số khung liên tiếp khớp mới **hiện tên** |
| `TRACK_MAX_AGE` | `12` | Số khung liên tiếp không thấy mặt vẫn giữ ô vuông |
| `TRACK_SMOOTH` | `0.45` | 0 = đứng yên, 1 = bám mặt ngay |
| `TRACK_BACKEND` | `medianflow` | Tracker phụ khi detector trượt: `medianflow` \| `csrt` \| `kcf` \| `none` |

- Sửa `.env` → khởi động lại: `python server.py`
- Đổi trên trang debug (API `POST /api/config`) → tự ghi lại vào `.env` và áp dụng ngay.
- `.env` cũ để `MATCH_THRESHOLD = 75` (đơn vị LBPH) sẽ **tự được quy đổi** sang
  `0.42` (đơn vị khoảng cách cosin) và server báo lại trong console.
- `PORT` và `CAMERA_PORT` **phải khác nhau**. Nếu để trùng, server tự đẩy cổng debug
  lên `PORT + 1` và báo lại trong console.
- Mật khẩu admin/developer trong `.env` là **nguồn chân lý**: mỗi lần khởi động server
  tự đồng ý hoá mật khẩu trong DB theo `.env` (đổi `.env` là đổi mật khẩu đăng nhập).

### Đơn vị của `MATCH_THRESHOLD`

| Mô hình | Đơn vị | Ví dụ giá trị |
|---|---|---|
| **SFace** (mặc định) | khoảng cách cosin, `0` = giống hệt, `2` = đối lập | `0.42` |
| **LBPH** (khi thiếu model) | điểm LBPH, nhỏ = giống | `75` |

Đo trên chính máy này với SFace: **cùng một người ≤ 0.34**, **người khác ≥ 0.84**
→ `0.42` nằm giữa, chừa biên cho ảnh nhỏ/ánh sáng lệch.

## 4. Chạy server & đăng nhập

```bat
python server.py
```

Server mở **hai cổng cùng lúc** (một tiến trình, chung bộ nhớ nhận diện):

| Cổng | Mặc định | Dành cho |
|---|---|---|
| **Cổng quản lý** | `http://<ip-máy>:5000` | **Mọi người** — 1 trang đăng nhập duy nhất |
| **Cổng debug** | `http://<ip-máy>:5001` | **Chỉ developer** — preview, cấu hình engine, cài ESP32 |

Máy khác trong mạng thay `localhost` bằng IP máy chủ: `http://192.168.1.9:5000`.

### Một trang đăng nhập duy nhất

Vào `http://<ip-máy>:5000` → nhập **tên đăng nhập + mật khẩu** → server tự điều
hướng theo vai trò của tài khoản:

| Vai trò | Tài khoản | Đăng nhập xong đi đâu | Quyền |
|---|---|---|---|
| **Quản trị** (`admin`) | tên `admin`, mật khẩu `ADMIN_PASSWORD` trong `.env` | `/quan-ly` — đủ 8 tab | toàn trường |
| **Giáo viên** (`teacher`) | **admin tạo** ở tab *Giáo viên* | `/quan-ly` — 3 tab, chỉ lớp được gán | lớp của mình |
| **Developer** | `DEV_USERNAME` / `DEV_PASSWORD` trong `.env` | trang debug ở cổng `:5001` | preview + engine + ESP32 |

> Không có self-registration, không có tài khoản phụ huynh. Giáo viên bị xoá thì
> phiên đang mở tự bị đăng xuất ở lần gọi API kế tiếp.

### Một trang quản lý `/quan-ly`, tab theo vai trò

| Tab | Quản trị | Giáo viên |
|---|:---:|:---:|
| 📊 **Tổng quan** — ô số liệu, sĩ số lớp, biểu đồ 7 ngày | ✔ | |
| 👦 **Đăng ký khuôn mặt** — upload ảnh + danh sách/sửa học sinh | ✔ | ✔ (lớp mình) |
| 🗓️ **Hôm nay** — bảng đủ/thiếu/vắng/nghỉ theo buổi | ✔ | ✔ (lớp mình) |
| 🗓️ **Điểm danh** — lưới theo tháng (●◐N✕) | ✔ (mọi lớp) | |
| 📈 **Báo cáo** — ngày/tuần/tháng + **tải CSV** | ✔ | ✔ (lớp mình) |
| 🏫 **Lớp & lịch học** — thêm/xoá/đổi tên lớp + khung giờ buổi | ✔ | |
| 👩‍🏫 **Giáo viên** — tạo tài khoản, gán lớp | ✔ | |
| ⚙️ **Hệ thống** — CPU/RAM/ổ đĩa, engine, cổng dịch vụ | ✔ | |

Gõ tay `?tab=` của vai trò khác cũng không lộ nội dung — server đưa về tab hợp lệ
của vai trò đang đăng nhập.

#### 👦 Đăng ký khuôn mặt (upload ảnh — không chụp camera)

- Chọn lớp, nhập **họ tên + ngày sinh**, **chọn 1–8 ảnh** có sẵn (jpg/png) → **Đăng ký**.
- Danh sách bên phải: tìm theo tên/lớp, **Thêm mẫu** (upload thêm ảnh), **Sửa**
  (tên/ngày sinh/lớp), **Xoá mẫu mặt** (đăng ký lại từ đầu, không mất lịch sử điểm
  danh), **Xoá học sinh**.
- **Chỉ nên đăng ký một người trong mỗi ảnh.** Ảnh có nhiều mặt sẽ bị từ chối hoặc
  lấy mặt lớn nhất.

#### 🗓️ Hôm nay & 📈 Báo cáo

- **Hôm nay:** mỗi học sinh 1 dòng với trạng thái theo từng buổi:
  **đủ** (● điểm đủ 2 buổi / ● 1 buổi lớp 1 buổi) · **thiếu** (◐ thiếu buổi) ·
  **vắng** (✕ không điểm danh) · **nghỉ** (N + lý do do giáo viên ghi).
  Bấm ∅ để **ghi chú nghỉ** (lý do có gợi ý sẵn, lưu kèm tên giáo viên nhập).
- **Báo cáo:** chọn lớp + khoảng ngày/tuần/tháng → bảng trạng thái từng ngày +
  tổng hợp, nút **Tải CSV** (kèm BOM `﻿` nên Excel mở đúng tiếng Việt).

#### 🏫 Lớp & lịch học

- Thêm / xoá / **đổi tên lớp** (tên tự chuẩn hoá: `12 a1` → `12A1`). Lớp còn học
  sinh thì bắt buộc chọn lớp đích để chuyển sang — không bao giờ mất dữ liệu.
- **Lịch học:** 1 hoặc 2 buổi/ngày + khung giờ bắt đầu/kết thúc mỗi buổi →
  quyết định học sinh được tính điểm mấy lần trong ngày (tối đa 1 lần/buổi).

#### 👩‍🏫 Giáo viên

- Tạo tài khoản: **tên đăng nhập + mật khẩu (≥ 6 ký tự) + họ tên + chọn lớp**.
  Tài khoản không có `password_hash` trong bảng trả về API.
- Gán/bớt lớp sau đó bằng `PUT /api/teachers/<id>/classes`.

### Bảng màu trạng thái (dùng thống nhất mọi tab)

| Màu | Ý nghĩa |
|---|---|
| ✅ xanh lá | **có mặt** / điểm đủ buổi |
| 🔵 xanh dương | **thiếu** buổi (◐ — có điểm nhưng chưa đủ) |
| ⚠ hổ phách | **nghỉ** (đã có lý do) |
| ❌ đỏ | **không điểm danh** (✕) |
| ⚪ xám | **chưa tới ngày / chưa tới buổi** |

Chữ dùng **Be Vietnam Pro** (tự host trong `static/fonts/`, chạy offline), số liệu
và giờ dùng **JetBrains Mono**.

## 5. Nhận diện (engine)

### Ô vuông & ID theo dõi

- Mỗi mặt được vẽ trong **ô vuông** cạnh = `max(w, h) × SQUARE_PAD`, luôn vuông kể
  cả khi sát mép khung; mỗi ô có **mã ID** (`#1`, `#2`…) ổn định suốt thời gian người
  đó còn trong khung.
- Ô được **làm mượt** và **giữ nguyên khi mất dặt** tối đa `TRACK_MAX_AGE` khung;
  tên chỉ hiện sau `TRACK_CONFIRM` khung khớp liên tiếp → không nháy tên lung lay.
- Màu trên preview: 🟩 xanh = đã nhận đúng tên · 🔵 cyan = đang xác nhận ·
  🔴 đỏ = chưa nhận diện.

### Chống nhận nhầm khi có nhiều người

Ba lớp bảo vệ, đều **ưu tiên báo "Chưa nhận diện" còn hơn đoán sai**:

1. **Ngưỡng riêng từng người** — suy từ độ giống nhau giữa các mẫu của chính họ.
2. **Biên so với ứng viên thứ hai** — gần bằng nhau (`MATCH_MARGIN`) thì không chốt.
3. **Mỗi người một ô duy nhất trong khung** — 2 mặt cùng giống 1 người thì chỉ giữ
   kết quả tốt nhất.

## 6. API debug (cổng `CAMERA_PORT` — mặc định `:5001`)

**Mọi endpoint đều cần phiên developer** (trừ `/api/ping` và `/api/esp32/frame`).
Người lạ → `401`; trang `/` không đăng nhập → chuyển về `http://<ip>:5000/`.

| Method | Endpoint | Quyền | Mô tả |
|---|---|---|---|
| GET | `/` | developer | Trang debug: preview, cấu hình engine, cài ESP32 |
| GET | `/video_feed` | developer | Luồng MJPEG đã khoanh vùng |
| GET | `/api/frame.jpg` | developer | Khung hình nguyên bản |
| GET | `/api/status` | developer | Trạng thái camera + kết quả + thông tin engine |
| GET / POST | `/api/config` | developer | Đọc / đổi cấu hình (ghi `.env`, áp dụng ngay) |
| POST | `/api/recognize` | developer | Nhận diện 1 ảnh gửi lên (không ghi điểm danh) |
| GET | `/api/log` | developer | Nhật ký nhận diện (giữ 500 dòng gần nhất) |
| GET | `/api/ping` | **ai cũng được** | Sức khoẻ thiết bị `{ok, pong}` |
| POST | `/api/esp32/frame[?capture=1]` | **mở cho thiết bị** | ESP32 POST ảnh → JSON (xem mục 8) |
| GET | `/api/esp32/status` | developer | Trạng thái PUSH: `push_alive`, `push_count`, IP ESP32 |
| GET | `/api/esp32/cam_status` · `/api/esp32/control` · `/api/esp32/flash` · `/api/esp32/live` | developer | Proxy cài đặt ESP32: đọc trạng thái cam, bật/tắt cam (`?state=0\|1`), nháy flash |

Nhóm `/api/students*`, `/api/classes*`, `/api/teachers*`, `/api/report.csv`,
`/api/system`, `/api/portal/overview` **không có** ở cổng này (chỉ cổng `:5000`).

Mỗi phần tử trong `results`:

```jsonc
{
  "box": [x, y, w, h],           // hộp detector
  "square": [x, y, side, side],  // ô vuông đã làm mượt (luôn vuông)
  "track_id": 1,                 // ID theo dõi (null ở ảnh độc lập)
  "confirming": false,           // true = đang chờ đủ khung xác nhận
  "match": true,                 // true = đã chốt tên
  "student_id": 3, "full_name": "Nguyễn Văn An",
  "date_of_birth": "2005-03-12", "class_name": "10A1",
  "confidence": 0.19,            // KHOẢNG CÁCH (SFace) — nhỏ hơn = giống hơn
  "match_margin": 0.61           // cách biệt so với ứng viên thứ hai
}
```

`/api/recognize` và `/api/esp32/frame` xử lý **một ảnh độc lập** (không dùng
tracking) nên cùng một ảnh luôn cho cùng kết quả.

## 7. API quản lý (cổng `PORT` — mặc định `:5000`)

Dùng cookie phiên đăng nhập — gọi bằng `requests.Session()`:

```python
import requests
s = requests.Session()
s.post("http://localhost:5000/dang-nhap",
       data={"username": "admin", "password": "admin"})
s.get("http://localhost:5000/quan-ly?tab=tong-quan")
```

Đăng nhập **mọi vai trò qua cùng một endpoint** `POST /dang-nhap` với
`username` + `password` — server điều hướng theo role (admin/giáo viên →
`/quan-ly`; developer → `/debug` → cổng `:5001`).

### Đường dẫn trang

| Method | Đường dẫn | Quyền | Mô tả |
|---|---|---|---|
| `GET` | `/` | ai cũng được | Trang đăng nhập duy nhất (đã đăng nhập thì chuyển tới trang của mình) |
| `POST` | `/dang-nhap` | ai cũng được | Đăng nhập (`username`, `password`) |
| `GET` | `/debug` | developer | Chuyển hướng sang trang debug ở cổng `CAMERA_PORT` |
| `GET` | `/quan-ly?tab=<tab>` | admin, teacher | Trang quản lý duy nhất, chia tab theo vai trò |
| `GET/POST` | `/dang-xuat` | đã đăng nhập | Đăng xuất |

Tab hợp lệ — admin: `tong-quan`, `dang-ky`, `hom-nay`, `diem-danh`, `bao-cao`,
`lop`, `giao-vien`, `he-thong`; giáo viên: `dang-ky`, `hom-nay`, `bao-cao`.
Tab lạ → về tab đầu tiên của vai trò.

### Lớp & lịch học

| Method | Đường dẫn | Quyền | Mô tả |
|---|---|---|---|
| `GET` | `/api/classes` | đã đăng nhập | Danh mục lớp + sĩ số + người có mặt hôm nay + lịch học |
| `POST` | `/api/classes` | admin | Thêm lớp — `{"name": "12A1"}` |
| `POST` | `/api/classes/<tên>/rename` | admin | Đổi tên lớp — `{"name": "12A9"}` (học sinh tự đổi theo) |
| `POST` | `/api/classes/<tên>/schedule` | admin | Lịch học — `{"sessions": 1\|2, "sang_start": "07:00", "sang_end": "11:00", "chieu_start": "13:00", "chieu_end": "16:00"}` |
| `DELETE` | `/api/classes/<tên>?move_to=<lớp>` | admin | Xoá lớp; còn học sinh thì bắt buộc có `move_to` |

### Học sinh & điểm danh

| Method | Đường dẫn | Quyền | Mô tả |
|---|---|---|---|
| `GET` | `/api/students[?class=&q=]` | đã đăng nhập | Danh sách (giáo viên: chỉ lớp mình; admin gõ `q` → tìm toàn trường) |
| `POST` | `/api/students` | admin, GV | Đăng ký — JSON/multipart: `full_name`, `date_of_birth`, `class_name`, `images[]` |
| `GET/PUT/DELETE` | `/api/students/<id>` | xem: đã đăng nhập; sửa/xoá: theo quyền lớp | Sửa thông tin / xoá (kèm mẫu mặt) |
| `POST` | `/api/students/<id>/samples` | admin, GV (lớp mình) | Thêm mẫu ảnh (multipart `images`) |
| `POST` | `/api/students/<id>/reset-face` | admin, GV (lớp mình) | Xoá toàn bộ mẫu mặt để đăng ký lại |
| `GET` | `/api/students/<id>/attendance` | admin, GV (lớp mình) | Số buổi/ngày đã đi học + chi tiết từng ngày |
| `GET` | `/api/report.csv?class=&start=&end=` | admin, GV (lớp mình) | Tải báo cáo CSV (BOM cho Excel) |
| `GET` | `/api/portal/overview` | admin | Số liệu tổng quan |
| `GET` | `/api/system` | admin | CPU / RAM / ổ đĩa / uptime / engine / cổng |

### Ghi chú vắng / nghỉ

| Method | Đường dẫn | Quyền | Mô tả |
|---|---|---|---|
| `POST` | `/api/absences` | admin, GV (lớp mình) | `{"student_id": 3, "day": "2026-10-01", "reason": "Sốt"}` |
| `DELETE` | `/api/absences?student_id=3&day=2026-10-01` | admin, GV (lớp mình) | Xoá ghi chú |

### Giáo viên / tài khoản

| Method | Đường dẫn | Quyền | Mô tả |
|---|---|---|---|
| `GET` | `/api/teachers` | admin | Danh sách tài khoản (không trả `password_hash`) |
| `POST` | `/api/teachers` | admin | Tạo — `{"username", "password" (≥6), "full_name", "classes": [...]}` |
| `PUT` | `/api/teachers/<id>/classes` | admin | Gán lại lớp — `{"classes": [...]}` |
| `DELETE` | `/api/teachers/<id>` | admin | Xoá tài khoản |

> **Phân quyền:** giáo viên chỉ chạm được lớp được gán — lớp khác trả `403`
> `{"ok": false, "error": ...}`; trang/API dành cho vai trò khác trả `302` về trang
> của chính họ; chưa đăng nhập → về trang đăng nhập.

## 8. ESP32-CAM (firmware trong `CameraWebServer_copy_20261009125906/`)

### Chế độ nút nhấn (mặc định — chạy thật)

| Biến trong `.ino` | Mặc định | Ý nghĩa |
|---|---|---|
| `BUTTON_ENABLE` | `1` | Bật chế độ nút nhấn = 1 phiên quét điểm danh |
| `BUTTON_PIN` | `13` | GPIO nút (INPUT_PULLUP, kích mức thấp; **cấm** GPIO 12/16/17/4/0/1/3/2) |
| `BUTTON_SCAN_SECONDS` | `15` | Quét tối đa từng đó giây mỗi lần bấm (trúng là dừng sớm) |
| `BUTTON_POST_GAP_MS` | `300` | Nghỉ giữa 2 khung POST (~2–3 khung/giây) |
| `BUTTON_MAX_FAILS` | `3` | POST lỗi liên tiếp quá mức này → báo 5 nháy, dừng quét |
| `PUSH_URL` | `http://<ip-may>:5001/api/esp32/frame` | Server nhận ảnh (`:5001` = `CAMERA_PORT`) |
| `CAM_OFF_DELAY_MS` | `1500` | Giữ cam sau khi quét xong rồi **tắt sensor** (PWDN HIGH) |
| Flash | GPIO đèn flash | Giữ SÁNG suốt phiên quét (vừa chiếu sáng vừa báo "đang quét") |
| LED báo | GPIO4 (ledcWrite, fallback GPIO33) | Nháy theo kết quả (bảng mục 2) |

Luồng firmware: đánh thức camera (`camWake`) → flash 250 ms → lặp chụp +
`POST PUSH_URL?capture=1` (multipart, field `file`, boundary `esp32cam`, tối đa
15 giây, trúng là dừng) → parse JSON trả về bằng `strstr` (key ASCII, không cần
thư viện JSON) → tắt flash → nháy LED → `camSleep`. Camera **không khởi động lúc
boot** — chỉ bật khi có người bấm nút.

Phản hồi của server (JSON top-level, key tiếng Việt ở giá trị):

```jsonc
{"ok": true, "matched": 1, "already": false,
 "full_name": "Nguyễn Văn An", "class_name": "10A1",
 "session": 1, "session_name": "Buổi sáng",
 "count": 1, "results": [ ... ]}
```

`GET /live?state=0|1` trên ESP32 (port 80) bật/tắt camera từ xa — trang debug
dùng nút **Bật/Tắt cam ESP32** gọi qua proxy `/api/esp32/live`.

### Các chế độ khác

| Cách | ESP32 (`.ino`) | Server |
|---|---|---|
| **Nút nhấn** (khuyên dùng) | `BUTTON_ENABLE = 1` | không cần `STREAM_URL` |
| **PUSH liên tục** | `BUTTON_ENABLE = 0`, `PUSH_ENABLE = 1`, `PUSH_MODE_MJPEG = 0` | xem `GET /api/esp32/status` |
| **PULL** (chỉ debug) | `PUSH_ENABLE = 0` | `.env`: `STREAM_URL = http://<ip-esp32>:81/stream` |

- `<ip-esp32>` xem trên Serial Monitor (baud 115200). Web `:80`, stream `:81`.
- Nạp firmware: board `AI Thinker ESP32-CAM` (PSRAM), partition `custom`
  (`partitions.csv`). **Chi tiết cấu hình + nháy LED: xem
  `CameraWebServer_copy_20261009125906/README.md`.**
- Để VGA 640×480 trở lên cho nhận diện xa 2–3 m. Nếu mặt nhỏ hơn `MIN_FACE_SIZE`
  (44) bị bỏ qua, hạ xuống `24–32` và tăng `DETECT_SCORE` một chút.

> **Chưa biên dịch tại chỗ:** repo không có `arduino-cli`/`pio` — mở
> `CameraWebServer_copy_*/CameraWebServer.ino` bằng **Arduino IDE**, cài board
> ESP32 + chọn `AI Thinker ESP32-CAM`, sửa `ssid`/`password`/`PUSH_URL` rồi nạp.

## 9. Kiểm thử & đo ngưỡng

```bat
python test_db.py        # tầng dữ liệu: lớp, lịch học, điểm danh theo buổi, users (offline)
python test_engine.py    # bộ nhận diện (offline, không cần server/camera)
python server.py         # mở server ở cả 2 cổng (5000 + 5001)
python test_portal.py    # 1 trang đăng nhập + 3 vai trò + tab + phân quyền (2 cổng)
python test_api.py       # API: đăng ký, nhận diện, capture=1, MJPEG, luật 1 lần/buổi
```

- `test_db.py` và `test_engine.py` chạy trên database riêng (`data/_test_db/`,
  `data/_test_out/`) nên **không đụng** dữ liệu thật trong `data/students.db`.
- `test_api.py` và `test_portal.py` **tự đăng nhập** (`admin`/`admin` +
  developer từ `.env`), **tự dọn dẹp** (xoá học sinh, lớp, tài khoản test) và đọ
  cổng từ `.env` — chạy được song song với việc dùng hệ thống thật.

Đo lại ngưỡng cho đúng dữ liệu của bạn (offline, dùng mẫu đã đăng ký):

```bat
python tune_threshold.py                          # chỉ dùng mẫu đã đăng ký
python tune_threshold.py --live --frames 40       # đo thêm trên luồng preview
```

## 10. Cấu trúc thư mục

```
nhan-dien-khuon-mat-main/
├── .env / .env.example # TOÀN BỘ cấu hình (cổng, ngưỡng, mật khẩu admin/developer)
├── server.py           # 2 Flask app trong 1 tiến trình: cổng quản lý + cổng debug
├── auth.py             # PBKDF2 + phân quyền (admin / teacher / developer)
├── face_engine.py      # detector YuNet + matcher SFace + tracker ô vuông + vẽ nhãn
├── stream.py           # đọc MJPEG từ điện thoại / webcam (preview developer)
├── database.py         # SQLite: users, students, classes+lịch học, attendance theo
│                       #   buổi, absences, recognition_log (tự migrate khi khởi động)
├── requirements.txt
├── test_db.py          # kiểm thử tầng dữ liệu (offline)
├── test_engine.py      # kiểm thử bộ nhận diện (offline)
├── test_api.py         # kiểm thử API — cần server đang chạy
├── test_portal.py      # kiểm thử đăng nhập/phân quyền 2 cổng — cần server
├── tune_threshold.py   # đo phân bố khoảng cách, gợi ý ngưỡng
├── probe_camera.py     # chẩn đoán endpoint camera
├── grab_live.py        # tải 1 khung từ /video_feed để xem
├── static/style.css    # hệ thống giao diện + bảng màu trạng thái
├── static/fonts.css    # @font-face tự host (Be Vietnam Pro + JetBrains Mono)
├── static/fonts/*.woff2
├── templates/
│   ├── base.html       # khung chung: điều hướng theo vai trò, đăng xuất
│   ├── landing.html    # 🎓 1 trang đăng nhập duy nhất (cổng PORT)
│   ├── portal.html     # 🗂️ Trang quản lý DUY NHẤT — 8 tab admin / 3 tab GV
│   └── index.html      # 🛠️ Trang debug developer (cổng CAMERA_PORT): preview,
│                       #    cấu hình engine, cài ESP32, bật/tắt cam, thử capture
├── CameraWebServer_copy_20261009125906/   # firmware ESP32-CAM (xem README riêng)
└── data/
    ├── students.db     # tự tạo (migrate idempotent mỗi lần khởi động)
    ├── faces/<id>/*.jpg        # mẫu ảnh mặt đã căn 112×112 (+ bản _flip)
    └── models/*.onnx   # model YuNet + SFace (tự tải lần đầu)
```

## 11. Xử lý sự cố

| Vấn đề | Cách sửa |
|---|---|
| `Thiếu module cv2.face` | `pip uninstall opencv-python` rồi `pip install opencv-contrib-python` |
| Bấm nút mà LED nháy 5 lần (lỗi mạng) | Kiểm tra WiFi của ESP32, `PUSH_URL` trỏ đúng IP máy chạy server + cổng `5001`, tường lửa mở 5001 |
| LED nháy 3 lần (không nhận ra) | Đứng gần/đứng thẳng, ánh sáng đều; xem trang debug (cổng 5001) để thấy ô vuông; thêm lại mẫu mặt |
| LED nháy 2 lần | Đúng rồi — bạn đã điểm danh buổi này rồi (tối đa 1 lần/buổi) |
| Điểm danh sai buổi / muốn sửa buổi | Sửa khung giờ ở tab **Lớp & lịch học**; xoá dòng điểm danh trong DB nếu cần chấm lại |
| `WRONG_VERSION_NUMBER` / lỗi SSL | Nhập nhầm `https://` — đổi `STREAM_URL` thành `http://IP:4747/video` |
| Preview quay mãi không có hình | IP sai / chưa bấm **Start** trên điện thoại / sai WiFi / app camera đang bận cho thiết bị khác |
| `trả về trang web chứ không phải luồng video` | App camera chỉ nhận 1 client — đóng tab/ứng dụng khác đang xem luồng đó |
| Sửa `.env` xong không có tác dụng | File `.env` có ký tự BOM ở đầu khiến khoá đầu bị bỏ qua — xoá BOM (server đã tự bỏ BOM khi đọc/ghi) |
| Mở `:5001` thấy **đề nghị đăng nhập** | Đúng rồi — cổng 5001 là **chỉ developer**. Đăng nhập tài khoản developer, hoặc dùng cổng `:5000` cho quản lý |
| Vào `:5000/quan-ly` bị đá về trang đăng nhập | Bạn chưa đăng nhập (hoặc là developer — developer vào `/debug` chứ không có `/quan-ly`) |
| Quên mật khẩu admin/developer | Đổi `ADMIN_PASSWORD` / `DEV_PASSWORD` trong `.env` rồi khởi động lại server (`.env` là nguồn chân lý) |
| Không tải được model | Kiểm tra mạng, hoặc copy 2 file `.onnx` từ opencv_zoo vào `data/models/` |
| Nhận diện sai / "Chưa nhận diện" | Thêm nhiều mẫu hơn (nhiều khoảng cách + góc), ánh sáng đều; **tăng** `MATCH_THRESHOLD` (0.42 → 0.50) nếu miss. Đo bằng `tune_threshold.py` |
| Nhận nhầm người khác | **Giảm** `MATCH_THRESHOLD`, tăng `MATCH_MARGIN`, thêm mẫu chuẩn |
| Không thấy mặt nào | Quá xa/siêu nhỏ → giảm `MIN_FACE_SIZE`; nhiều vật giống mặt → tăng `DETECT_SCORE` |
| Tên hiện chậm | Giảm `TRACK_CONFIRM` (3 → 2) |
| Ô vuông nhảy / mất khi chớp sáng | Tăng `TRACK_MAX_AGE`, giảm `TRACK_SMOOTH`; hoặc `TRACK_BACKEND = csrt` |
| Chậm | Giảm `PROCESS_FPS`, giảm độ phân giải preview (512–640 px là đủ) |
