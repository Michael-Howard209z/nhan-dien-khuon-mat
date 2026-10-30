# Màn hình hiển thị kết quả điểm danh

Thư mục này chứa 2 firmware hiển thị (không nhận diện):

| Thiết bị | Vai trò | Thư mục |
|---|---|---|
| ESP32-C3 Super Mini + LCD1602 (PCF8574 I2C) | Hiển thị **tên** học sinh sau khi điểm danh | `esp32lcd/` |
| ESP32-2432S028 "CYD" 2.8" (**ST7789V** + XPT2046) | Hiển thị **ảnh trực tiếp** để học sinh căn mặt + tên/kết quả | `esp32cyd/` |

> **Lưu ý quan trọng về phần cứng:** bo mạch CYD của dự án này là phiên bản
> **ESP32-2432S028 (không có chữ "R", 2 cổng USB)**, sử dụng màn hình **ST7789V**
> chứ không phải ILI9341. Firmware mặc định `drv=1` (ST7789) - xem các mục
> `/drv`, `/inv`, `/rgb` trong bảng lệnh bên dưới.

Cả hai đều **chỉ đọc** kết quả từ ESP32-CAM qua `GET http://<ip-cam>/result`
(mỗi 0,7 - 1 giây). Không có chức năng nhận diện nào chạy trên 2 bo mạch này.

```text
ESP32-CAM (nút GPIO13) --POST ảnh--> Máy chủ (:5001, YuNet + SFace)
        |                                      |
        | lưu kết quả cuối                     | ghi điểm danh 1 lần/buổi
        v                                      v
   GET /result (JSON)  <---- ESP32-C3 (LCD1602) : hiển thị tên
                       <---- CYD (320x240)      : hiển thị tên + ảnh trực tiếp
```

---

# Phần A - LCD1602 + ESP32-C3

Xem hướng dẫn chi tiết: [`esp32lcd/README.md`](esp32lcd/README.md)

Tóm tắt:

- Dây nối: SDA -> GPIO4, SCL -> GPIO5, VCC -> 5V, GND -> GND.
- Thư viện: `LiquidCrystal_PCF8574` (đã cài).
- Nạp chương trình: `flash_esp32c3.bat COM7`
- Cổng Serial 115200: `/ip`, `/ip 192.168.0.101`, `/find`, `/help`.

---

# Phần B - CYD ESP32-2432S028R (màn hình căn mặt)

## B.1 Chức năng

1. Hiển thị **ảnh trực tiếp chỉ trong 15 giây quét** (sau khi nhấn nút trên ESP32-CAM)
   từ `http://<cam>:81/stream` (MJPEG), để học sinh nhìn thấy mình và căn mặt.
   **Khi chờ (không nhấn nút): không mở luồng hình ảnh** -> ESP32-CAM không chụp liên tục,
   giảm tải máy chủ và tiết kiệm điện. Màn hình CYD hiển thị trang "SẴN SÀNG ĐIỂM DANH".
   Kết nối Wi-Fi + UDP 4321 + truy vấn `/result` vẫn được duy trì để khi nhấn nút
   có hình ảnh ngay, không phải chờ kết nối lại.
2. Đọc `GET http://<cam>:80/result` mỗi 0,5 giây, hiển thị thông báo:
   `ĐIỂM DANH OK` / `ĐÃ ĐIỂM DANH` / `KHÔNG NHẬN RA` / `LỖI CAMERA` / `LỖI MẠNG`.
3. ESP32-CAM thông báo thời điểm bắt đầu/kết thúc quét qua UDP
   `FACESYNC|SCAN|1|<ip>` / `FACESYNC|SCAN|0|<ip>` (cổng 4321) -> CYD
   mở/đóng luồng hình ảnh **ngay lập tức**, không cần chờ chu kỳ truy vấn 0,5 giây.
   Nếu UDP bị chặn, CYD sử dụng trường `scanning` trong `/result` làm phương án dự phòng.
   Sau khi quét xong, CYD đóng luồng hình ảnh để giảm nhiệt, nhưng vẫn giữ kết nối
   để lần nhấn nút tiếp theo có hình ảnh ngay.
4. Nút cảm ứng ở góc trên bên phải: `DỪNG` / `XEM` (tắt/bật chế độ xem trực tiếp).
5. Tự tìm IP camera qua UDP broadcast 4321 (`FACESYNC|CAM?`).

## B.2 Phần cứng / nối dây

CYD là bo mạch tích hợp tất cả trong một: màn hình + cảm ứng + ESP32 đã được nối sẵn.
Chỉ cần cắm cáp USB.

| Chức năng | Chân |
|---|---|
| TFT MOSI / MISO / SCLK | 13 / 12 / 14 (HSPI) |
| TFT CS / DC / RST / BL | 15 / 2 / -1 / 21 |
| Touch CS / IRQ | 33 / 36 |
| Touch SCLK / MISO / MOSI | 25 / 39 / 32 (VSPI) |
| Thẻ SD (không sử dụng) | 23 / 19 / 18 / 5 |

Bo mạch không có PSRAM (Flash 4MB, SRAM 520KB) -> firmware dùng JPEGDEC
để giải mã VGA xuống 320x240 (`JPEG_SCALE_HALF`) và vẽ trực tiếp từng khối,
không cần bộ đệm khung hình 150KB.

## B.3 Thư viện cần cài

```text
TFT_eSPI              (Bodmer)          >= 2.5.43
XPT2046_Touchscreen   (Paul Stoffregen) >= 1.4
JPEGDEC               (Larry Bank)      >= 1.8.5
```

Lệnh cài đặt:

```text
arduino-cli lib install "TFT_eSPI"
arduino-cli lib install "XPT2046_Touchscreen"
arduino-cli lib install "JPEGDEC"
```

**Quan trọng:** cấu hình chân TFT nằm trong `esp32cyd/tft_setup.h`
(TFT_eSPI tự động nạp tệp `tft_setup.h` nằm cùng thư mục sketch,
ưu tiên hơn `User_Setup.h` của thư viện). Không cần sửa `User_Setup.h`.

Hai biến thể dây SPI của CYD:

- `CYD_DISPLAY_ON_VSPI 0` (mặc định) - màn hình dùng HSPI 13/12/14, phù hợp với
  phần lớn bo mạch CYD bán trên thị trường.
- `CYD_DISPLAY_ON_VSPI 1` - nếu bo mạch của bạn nối màn hình vào các chân 23/19/18
  (một số tài liệu/phiên bản bo mạch khác). Khi đó, cảm ứng tự chuyển sang HSPI.

## B.4 Biên dịch và nạp chương trình

Biên dịch (FQBN dành cho CYD, không có PSRAM):

```text
set CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
"%CLI%" compile -b "esp32:esp32:esp32:CPUFreq=240,FlashFreq=80,FlashMode=dio,FlashSize=4M,PartitionScheme=huge_app,DebugLevel=none,PSRAM=disabled" --build-path "%CD%\firmware_esp32display\build_cyd" "%CD%\firmware_esp32display\esp32cyd"
```

Nạp chương trình:

```text
flash_esp32cyd.bat COM5
```

(Hoặc mở `firmware_esp32display\build_cyd\esp32cyd.ino.bin` bằng
`esptool.py --chip esp32 --baud 921600 write_flash 0x10000 ...`).

## B.5 Sử dụng

1. Sửa `WIFI_SSID` / `WIFI_PASS` ở đầu tệp `esp32cyd.ino` cho đúng Wi-Fi của trường.
2. Nạp chương trình, mở Serial Monitor ở tốc độ **115200**.
3. Lần đầu: CYD tự thử `192.168.0.101`; nếu không được, bo mạch sẽ gửi UDP
   `FACESYNC|CAM?` để tìm ESP32-CAM. Nếu vẫn không tìm thấy, nhập:
   ```text
   /ip 192.168.0.101
   ```
   (IP được lưu vào NVS, lần sau không cần nhập lại).
4. Khi học sinh nhấn nút trên ESP32-CAM, CYD mở luồng hình ảnh trong 15 giây và hiển thị
   **hình ảnh camera + khung căn mặt** để học sinh căn đúng khuôn mặt. CYD **KHÔNG hiển thị
   tên / kết quả điểm danh** (màn hình LCD riêng đảm nhiệm việc đó). Kết quả chỉ được ghi
   ra Serial theo định dạng `[RES] <tên> | <trạng thái> | <lớp>`.

Lệnh Serial:

| Lệnh | Tác dụng |
|---|---|
| `/help` | Hiển thị danh sách lệnh |
| `/ip` | Xem IP camera đang sử dụng |
| `/ip 192.168.0.101` | Đặt IP camera (lưu vào NVS) |
| `/find` | Tìm camera qua UDP 4321 |
| `/live on` / `/live off` | Bật/tắt chế độ xem trực tiếp |
| `/scan` | Buộc mở luồng hình ảnh trong 20 giây để kiểm tra video (không cần nhấn nút trên camera) |
| `/test` | Chạy lại tự kiểm tra màn hình (4 màu + viền + 4 góc) |
| `/rot 0..7` | Đổi hướng xoay ngay và **lưu vào NVS** (mặc định 1 = ngang 320x240) |
| `/rot` | Xem hướng xoay hiện tại |
| `/wizard` | Tự lần lượt thử các hướng xoay 0..7 (2 giây/bước) + thẻ kiểm tra để tìm hướng đúng |
| `/panel` | Đọc mã chip điều khiển màn hình (ILI9341 `0x009341` / ST7789 `0x858552`) |
| `/drv st7789` \| `/drv ili9341` \| `/drv auto` | Đổi trình điều khiển màn hình (lưu vào NVS, có hiệu lực sau `/reboot`) |
| `/inv on` \| `/inv off` | Bật/tắt đảo màu (INVON/INVOFF) - có hiệu lực ngay |
| `/rgb rgb` \| `/rgb bgr` | Đổi thứ tự màu RGB <-> BGR - có hiệu lực ngay |
| `/imgrot 0..3` | Xoay/lật **hình camera trên CYD** (0=giữ nguyên, 1=xoay 180, 2=lật ngang, 3=lật dọc). **Không** đổi hướng cảm biến trên ESP32-CAM nên ảnh gửi đến máy chủ nhận diện vẫn đúng. Lưu vào NVS. |
| `/touch` | Bật/tắt in tọa độ cảm ứng (hiệu chuẩn) |
| `/stats` | FPS, kích thước khung hình, thời gian giải mã, heap |
| `/reboot` | Khởi động lại |

## B.6 Hiệu chuẩn cảm ứng

Giá trị mặc định trong `esp32cyd.ino`:

```text
TOUCH_X_MIN 1500   TOUCH_X_MAX 3000
TOUCH_Y_MIN 1400   TOUCH_Y_MAX 2900
```

Nếu chạm bị lệch:

1. Nhập `/touch` để bật chế độ gỡ lỗi, sau đó chạm 4 góc màn hình.
2. Ghi lại giá trị RAW nhỏ nhất/lớn nhất của X và Y.
3. Sửa 4 macro ở trên, biên dịch và nạp lại chương trình.

Nếu cảm ứng bị xoay 90 độ hoặc ngược trục, sử dụng 3 macro ngay dưới
`TOUCH_Y_MAX` trong `esp32cyd.ino`:

```text
TOUCH_SWAP_XY   0   // 1 = đổi vai trò X/Y (xoay 90 độ)
TOUCH_INVERT_X  0   // 1 = đảo chiều trục X
TOUCH_INVERT_Y  0   // 1 = đảo chiều trục Y
```

Cách chọn nhanh: chạm góc **trên-trái**. Tọa độ in ra phải gần `(0,0)`.

- Ra `(0,240)` -> bật `TOUCH_INVERT_Y`.
- Ra `(320,0)` -> bật `TOUCH_INVERT_X`.
- Ra `(0,0)` nhưng hai trục bị tráo (chạm ngang lại ra dọc) -> bật `TOUCH_SWAP_XY`
  trước, sau đó chỉnh lại 2 macro đảo chiều nếu cần.

## B.7 Xử lý sự cố

| Hiện tượng | Nguyên nhân / cách xử lý |
|---|---|
| Màn hình trắng, không hiển thị gì | Sai biến thể dây SPI -> đổi `CYD_DISPLAY_ON_VSPI` từ 0 sang 1 hoặc ngược lại. Kiểm tra lại chân BL=21. |
| Màu bị sai: ĐỎ <-> XANH, màu cyan hiển thị thành vàng | Nhập `/rgb bgr` hoặc `/rgb rgb` (có hiệu lực ngay). Tương đương đổi `TFT_RGB_ORDER` trong `tft_setup.h`. |
| Màu bị âm bản / đảo màu (INVON/INVOFF) | Nhập `/inv on` hoặc `/inv off` (có hiệu lực ngay). Panel CYD ST7789 cần `/inv off`. |
| Chọn `/rot 1 3 5 7` nhưng hình KHÔNG xoay | Panel là **ST7789** chứ không phải ILI9341: nhập `/drv st7789`, sau đó `/reboot`. |
| Màn hình tối, chữ mờ | Điều chỉnh độ sáng bằng PWM tại `TFT_BL` (hiện đang bật ở mức cao). |
| Không tìm thấy camera | Nhập `/find`, sau đó `/ip <ip-cam>`. Kiểm tra cùng Wi-Fi/LAN và ESP32-CAM đang bật. |
| Hình bị giật, FPS thấp | Xem `/stats`: nếu thời gian giải mã > 60ms, kiểm tra `SPI_FREQUENCY` (40MHz). Nhiều thiết bị cùng xem luồng sẽ tranh giành khung hình. |
| Luôn hiện `KẾT NỐI` | Camera đang bị thiết bị khác chiếm dụng (mở nhiều tab `/stream`). Đóng bớt tab. |
| Chạm nút không phản hồi | Chạy `/touch` để kiểm tra tọa độ; hiệu chuẩn lại theo mục B.6. |
| Treo khi quét | Kiểm tra `scanning` có chuyển thành 1 không (log `[CAM] Bắt đầu quét`). Cơ chế an toàn sẽ tự thoát sau 20 giây. |
| Chỉ hiển thị **2/3 màn hình** / mất phần bên phải hoặc phía dưới | Panel có độ phân giải **240x320 theo chiều DỌC (portrait)**. Khung ngang 320x240 chỉ đạt được bằng `tft.setRotation(1)` hoặc `(3)` (mặc định là **1**). **Không** dùng rotation 0/2 (đó là chiều dọc -> giao diện rộng 320px bị cắt còn 2/3), và **không** đổi `TFT_WIDTH`/`TFT_HEIGHT` thành 320/240 (sai cửa sổ địa chỉ GRAM -> chỉ vẽ được 2/3). Nhập `/test`: viền trắng phải kín 4 cạnh và đủ 4 nhãn góc `0,0` / `319,0` / `0,239` / `319,239`. Nếu vẫn ở chiều dọc: nhập `/rot 3` để thử; nhật ký sẽ in `rotation N -> WxH`. |
| Hình có sọc ngang nhưng giao diện/chữ vẫn rõ | Chạy `/test`: nếu tự kiểm tra hiển thị sạch thì panel + trình điều khiển vẫn ổn, sọc nằm trong ảnh JPEG từ camera (môi trường/ánh sáng/độ phân giải). Thử tăng `SPI_FREQUENCY` lên 27-40MHz nếu `/stats` báo giải mã chậm. |
| Khởi động chậm khoảng 2,5 giây | Do tự kiểm tra lúc khởi động (`CYD_SELFTEST 1`). Đổi `CYD_SELFTEST` thành `0` trong `esp32cyd.ino` sau khi đã xác nhận panel hoạt động tốt. |

## B.8 Ghi chú kỹ thuật

- `JPEGDEC` cần khoảng 17,5KB RAM (đối tượng toàn cục). Bộ đệm JPEG được cấp phát lúc
  khởi động: thử lần lượt 112KB -> 96 -> 80 -> 64 -> 48 -> 32KB (chọn mức đầu tiên
  cấp phát thành công). Ảnh VGA chất lượng 12 thường khoảng 20-50KB nên 112KB là đủ;
  nếu heap thấp, kích thước sẽ tự giảm.
- `tft.setSwapBytes(true)` vì JPEGDEC xuất RGB565 dưới dạng uint16 chuẩn
  (little-endian), trong khi ILI9341 nhận byte cao trước.
- **Hướng màn hình:** CYD 2.8" dùng panel **240x320 theo chiều dọc (portrait)** (gốc).
  Khung ngang 320x240 = `tft.init()` (gốc 240x320) + `tft.setRotation(1)`
  (mặc định) hoặc `(3)`. `LCD_W 320` / `LCD_H 240` trong sketch là
  kích thước SAU KHI xoay, không phải `TFT_WIDTH`/`TFT_HEIGHT`. Đổi 2 macro
  trình điều khiển thành 320/240 sẽ làm sai cửa sổ địa chỉ -> chỉ hiển thị 2/3 màn hình
  hoặc xuất hiện sọc.
- **Thư viện TFT_eSPI bị ép cứng trình điều khiển:** tệp `User_Setup_Select.h` của
  thư viện trước đây có 3 dòng `#define ILI9341_DRIVER` / `TFT_WIDTH 240` /
  `TFT_HEIGHT 320` nằm NGOÀI `#ifndef USER_SETUP_LOADED`, khiến mọi thay đổi
  trong `tft_setup.h` bị vô hiệu. Đã sửa lại: 3 dòng đó hiện nằm trong
  `#if !defined(USER_SETUP_LOADED)`.
- **Xung nhịp SPI 27 MHz:** ILI9341 trên CYD chỉ nên chạy <= 40 MHz. Phiên bản trước
  đặt **60 MHz** (commit f5c7c52 ghi nhầm "20MHz"), gây sọc/rách hình (tearing)
  và chỉ vẽ được một phần màn hình. Muốn nhanh hơn, thử 40 MHz; nếu vẫn còn sọc,
  giảm xuống 20 MHz.
- **Kiểm tra hướng lúc khởi động:** `applyRotation(1)` (mặc định), sau đó kiểm tra
  `tft.width() > tft.height()`; nếu không đúng, tự thử rotation 0..3 và chọn hướng
  ngang đầu tiên, ghi nhật ký `[TFT] CẢNH BÁO...`. Lệnh `/rot N` đổi hướng xoay
  ngay mà không cần nạp lại chương trình.
- **Tự kiểm tra** (`CYD_SELFTEST 1`, hàm `drawSelfTest()`): hiển thị 4 màu đặc toàn
  màn hình -> viền trắng dày 2px kín 4 cạnh -> nhãn tọa độ 4 góc -> dòng `SELFTEST
  320x240`. Mục đích: phân biệt lỗi panel/trình điều khiển với lỗi ảnh JPEG. Chạy lại bằng
  lệnh `/test`. Nhật ký khởi động đúng phải là:
  `[TFT] driver ILI9341, native 240x320 portrait -> rotation2 320x240 (UI 320x240)`.
- **Chống ghép khung hình hỏng:** `readFrame()` loại bỏ phần JPEG cũ và bắt đầu lại
  khi gặp SOI mới trước khi thấy EOI (khung hình bị cắt giữa chừng do mạng rớt/camera
  treo) — tránh hai nửa khung hình dính vào nhau gây vạch ngang.
- Luồng được đọc theo từng khối (`Transfer-Encoding: chunked` của esp_http_server);
  tự động chuyển sang phương án dự phòng nếu máy chủ trả dữ liệu thô.
- `HTTPClient` sử dụng `setReuse(true)` cho `/result` để giảm số lần cấp phát bộ nhớ.
- Chỉ nên có 1 thiết bị xem luồng: nếu mở thêm trình duyệt tại `:81/stream`,
  CYD sẽ phải chia sẻ khung hình và có thể bị gián đoạn.
