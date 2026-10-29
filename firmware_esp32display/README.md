# Man hinh hien thi ket qua diem danh

Thu muc nay chua 2 firmware hien thi (khong nhan dien):

| Thiet bi | Vai tro | Thu muc |
|---|---|---|
| ESP32-C3 Super Mini + LCD1602 (PCF8574 I2C) | Hien thi **ten** hoc sinh sau khi diem danh | `esp32lcd/` |
| ESP32-2432S028R "CYD" 2.8" (ILI9341 + XPT2046) | Hien thi **anh live** de hoc sinh can mat + ten/ket qua | `esp32cyd/` |

Ca hai deu **chi doc** ket qua tu ESP32-CAM qua `GET http://<ip-cam>/result`
(moi 0.7 - 1 giay). Khong co nhan dien nao chay tren 2 board nay.

```
ESP32-CAM (nut GPIO13) --POST anh--> Server (:5001, YuNet + SFace)
        |                                   |
        |  luu ket qua cuoi                 | ghi diem danh 1 lan/buoi
        v                                   v
   GET /result (JSON)  <---- ESP32-C3 (LCD1602)  : hien ten
                       <---- CYD (320x240)       : hien ten + anh live
```

---

# Phan A - LCD1602 + ESP32-C3

Xem huong dan chi tiet: [`esp32lcd/README.md`](esp32lcd/README.md)

Tom tat:

- Day: SDA -> GPIO4, SCL -> GPIO5, VCC -> 5V, GND -> GND.
- Thu vien: `LiquidCrystal_PCF8574` (da cai).
- Nap: `flash_esp32c3.bat COM7`
- Serial 115200: `/ip`, `/ip 192.168.0.101`, `/find`, `/help`.

---

# Phan B - CYD ESP32-2432S028R (man hinh can mat)

## B.1 Chuc nang

1. Hien **anh live chi trong 15 giay quet** (sau khi bam nut tren ESP32-CAM)
   tu `http://<cam>:81/stream` (MJPEG) de hoc sinh thay minh va can mat.
   **Idle (khong bam nut): khong mo stream** -> ESP32-CAM khong chup lien tuc,
   may mat, tiet kiem dien. Man hinh CYD hien trang "SAN SANG DIEM DANH".
   Kết nối WiFi + UDP 4321 + polling /result duoc giu de bam nut co anh ngay,
   khong delay.
2. Doc `GET http://<cam>:80/result` moi 0.5s, hien banner:
   `DIEM DANH OK` / `DA DIEM DANH` / `KHONG NHAN RA` / `LOI CAMERA` / `LOI MANG`.
3. ESP32-CAM bao bat dau/ket thuc qua UDP `FACESYNC|SCAN|1|<ip>` /
   `FACESYNC|SCAN|0|<ip>` (cong 4321) -> CYD mo/dong stream **ngay**,
   khong cho 0.5s poll. Neu UDP bi chan, CYD dung cot `scanning` trong
   `/result` du phong. Sau quet xong CYD dong stream de ha nhiet, nhung van
   giu ket noi de lan bam sau co anh luon.
4. Nut cam ung goc tren phai: `DUNG` / `XEM` (tat/bat xem truc tiep).
5. Tu tim IP camera qua UDP broadcast 4321 (`FACESYNC|CAM?`).

## B.2 Phan cung / noi day

CYD la board "tat ca trong mot": man hinh + cam ung + ESP32 da noi san.
Chi can cam cap USB.

| Chuc nang | Chan |
|---|---|
| TFT MOSI / MISO / SCLK | 13 / 12 / 14 (HSPI) |
| TFT CS / DC / RST / BL | 15 / 2 / -1 / 21 |
| Touch CS / IRQ | 33 / 36 |
| Touch SCLK / MISO / MOSI | 25 / 39 / 32 (VSPI) |
| SD card (khong dung) | 23 / 19 / 18 / 5 |

Board khong co PSRAM (4MB Flash, 520KB SRAM) -> firmware dung JPEGDEC
decode VGA -> 320x240 (`JPEG_SCALE_HALF`) va ve truc tiep tung block,
khong can framebuffer 150KB.

## B.3 Thu vien can cai

```
TFT_eSPI              (Bodmer)          >= 2.5.43
XPT2046_Touchscreen   (Paul Stoffregen) >= 1.4
JPEGDEC               (Larry Bank)      >= 1.8.5
```

Lenh cai:

```
arduino-cli lib install "TFT_eSPI"
arduino-cli lib install "XPT2046_Touchscreen"
arduino-cli lib install "JPEGDEC"
```

**Quan trong:** cau hinh chan TFT nam trong `esp32cyd/tft_setup.h`
(TFT_eSPI tu dong include file `tft_setup.h` nam cung thu muc sketch,
uu tien hon `User_Setup.h` cua thu vien). Khong can sua `User_Setup.h`.

Hai bien the day SPI cua CYD:

- `CYD_DISPLAY_ON_VSPI 0` (mac dinh) - man hinh HSPI 13/12/14, dung cho
  da so board CYD ban tren thi truong.
- `CYD_DISPLAY_ON_VSPI 1` - neu board cua ban noi man hinh o 23/19/18
  (mot so tai lieu/ban variant). Khi do cam ung tu chuyen sang HSPI.

## B.4 Build va nap

Build (FQBN cho CYD, khong PSRAM):

```
set CLI=%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
"%CLI%" compile -b "esp32:esp32:esp32:CPUFreq=240,FlashFreq=80,FlashMode=dio,FlashSize=4M,PartitionScheme=huge_app,DebugLevel=none,PSRAM=disabled" --build-path "%CD%\firmware_esp32display\build_cyd" "%CD%\firmware_esp32display\esp32cyd"
```

Nap:

```
flash_esp32cyd.bat COM5
```

(hoac mo `firmware_esp32display\build_cyd\esp32cyd.ino.bin` bang
`esptool.py --chip esp32 --baud 921600 write_flash 0x10000 ...`).

## B.5 Su dung

1. Sua `WIFI_SSID` / `WIFI_PASS` o dau `esp32cyd.ino` cho dung WiFi truong.
2. Nap, mo Serial Monitor **115200**.
3. Lan dau: CYD tu thu `192.168.0.101`, neu khong duoc se gui UDP
   `FACESYNC|CAM?` de tim ESP32-CAM. Neu van khong thay, go:
   ```
   /ip 192.168.0.101
   ```
   (IP duoc luu vao NVS, lan sau khong can go lai).
4. Man hinh se hien anh live + khung can mat. Hoc sinh bam nut tren
   ESP32-CAM -> man hinh chuyen sang `DANG QUET`, sau do hien ten.

Lenh Serial:

| Lenh | Tac dung |
|---|---|
| `/help` | Danh sach lenh |
| `/ip` | Xem IP camera dang dung |
| `/ip 192.168.0.101` | Dat IP camera (luu NVS) |
| `/find` | Tim camera qua UDP 4321 |
| `/live on` / `/live off` | Bat/tat xem truc tiep |
| `/test` | Chay lai self-test man hinh (4 mau + vien + 4 goc) |
| `/rot 0..7` | Doi rotation ngay va **luu vao NVS** (mac dinh 1 = landscape 320x240) |
| `/rot` | Xem rotation dang dung |
| `/wizard` | Tu thu lan luot rot 0..7 (2s/buoc) + the kiem tra de tim huong dung |
| `/touch` | Bat/tat in toa do cam ung (hieu chuan) |
| `/stats` | fps, kich thuoc frame, thoi gian decode, heap |
| `/reboot` | Khoi dong lai |

## B.6 Hieu chuan cam ung

Mac dinh trong `esp32cyd.ino`:

```
TOUCH_X_MIN 1500   TOUCH_X_MAX 3000
TOUCH_Y_MIN 1400   TOUCH_Y_MAX 2900
```

Neu cham lech:

1. Go `/touch` de bat debug, cham 4 goc man hinh.
2. Ghi lai gia tri RAW nho nhat/lon nhat cua X va Y.
3. Sua 4 macro tren, build va nap lai.

Neu cam ung bi xoay 90 do hoac nguoc truc, dung 3 macro ngay duoi
`TOUCH_Y_MAX` trong `esp32cyd.ino`:

```
TOUCH_SWAP_XY   0   // 1 = doi vai tro X/Y (xoay 90 do)
TOUCH_INVERT_X  0   // 1 = dao dau truc X
TOUCH_INVERT_Y  0   // 1 = dao dau truc Y
```

Cach chon nhanh: cham goc **tren-trai**. Toa do in ra phai gan `(0,0)`.
- Ra `(0,240)` -> bat `TOUCH_INVERT_Y`.
- Ra `(320,0)` -> bat `TOUCH_INVERT_X`.
- Ra `(0,0)` nhung truc bi trao (cham ngang ra doc) -> bat `TOUCH_SWAP_XY`
  truoc, roi chinh lai 2 macro dao dau neu can.

## B.7 Xu ly su co

| Hien tuong | Nguyen nhan / cach xu ly |
|---|---|
| Man hinh trang, khong hien gi | Sai bien the day SPI -> doi `CYD_DISPLAY_ON_VSPI` 0 <-> 1. Kiem tra lai chan BL=21. |
| Mau bi sai: DO <-> XANH, cyan hien thanh vang | Doi thu tu mau trong `tft_setup.h`: `#define TFT_RGB_ORDER TFT_RGB` <-> `TFT_BGR`. |
| Mau bi am ban (inverted) | Board 2 cong USB co the dung panel ST7789: doi `#define CYD_PANEL_ST7789 0` thanh `1` trong `tft_setup.h`. |
| Man hinh toi, co chu mo | Chinh do sang bang PWM o `TFT_BL` (hien dang bat muc cao). |
| Khong thay camera | Go `/find`, roi `/ip <ip-cam>`. Kiem tra cung WiFi/LAN, ESP32-CAM dang bat. |
| Anh giat, fps thap | Xem `/stats`: neu decode > 60ms, kiem tra `SPI_FREQUENCY` (40MHz). Nhieu client stream cung luc se tranh frame. |
| `KET NOI` mai | Camera dang bi client khac chiem (mo nhieu tab `/stream`). Dong bot tab. |
| Cham nut khong an | Chay `/touch` de kiem tra toa do; hieu chuan lai B.6. |
| Treo khi quet | Xem `scanning` co ve 1 khong (log `[CAM] Bat dau quet`). Fail-safe tu bo sau 20s. |
| Chi hien **2/3 man hinh** / mat phan ben phai hoac duoi | Panel la **240x320 theo chieu DOC (portrait)**. Khung ngang 320x240 chi dat duoc bang `tft.setRotation(1)` hoac `(3)` (mac dinh la **1**). **Khong** dung rotation 0/2 (do la portrait -> UI 320px bi cat con 2/3), va **khong** doi `TFT_WIDTH`/`TFT_HEIGHT` thanh 320/240 (sai cua so dia chi GRAM -> chi ve 2/3). Go `/test`: vien trang phai kin 4 canh va du 4 nhan goc `0,0` / `319,0` / `0,239` / `319,239`. Neu van portrait: go `/rot 3` de thu, log se in `rotation N -> WxH`. |
| Anh co soc ngang nhung UI/chu sach | Chay `/test`: neu self-test sach thi panel + driver OK, soc nam trong anh JPEG tu camera (moi truong/anh sang/bien do). Thu tang `SPI_FREQUENCY` len 27-40MHz neu `/stats` bao decode cham. |
| Boot cham ~2,5 giay | Self-test luc boot (`CYD_SELFTEST 1`). Doi `CYD_SELFTEST` thanh `0` trong `esp32cyd.ino` khi da xac nhan panel tot. |

## B.8 Ghi chu ky thuat

- `JPEGDEC` can ~17.5KB RAM (doi tuong global). Bo dem JPEG cap phat luc
  boot: thu 112KB -> 96 -> 80 -> 64 -> 48 -> 32KB (lay muc dau tien thanh cong).
  VGA quality 12 thuong ~20-50KB nen 112KB la du; neu heap thap, tu tut xuong.
- `tft.setSwapBytes(true)` vi JPEGDEC xuat RGB565 dang uint16 chuan
  (little-endian), con ILI9341 nhan byte cao truoc.
- **Huong man hinh:** CYD 2.8" dung panel **240x320 portrait** (native).
  Khung ngang 320x240 = `tft.init()` (native 240x320) + `tft.setRotation(1)`
  (mac dinh) hoac `(3)`. `LCD_W 320` / `LCD_H 240` trong sketch la
  kich thuoc SAU xoay, khong phai `TFT_WIDTH`/`TFT_HEIGHT`. Doi 2 macro
  driver thanh 320/240 se lam lech cua so dia chi -> chi hien 2/3 man hinh
  hoac soc.
- **Thu vien TFT_eSPI bi ep cung driver:** file `User_Setup_Select.h` cua
  thu vien tung co 3 dong `#define ILI9341_DRIVER` / `TFT_WIDTH 240` /
  `TFT_HEIGHT 320` nam NGOAI `#ifndef USER_SETUP_LOADED`, khien moi thay doi
  trong `tft_setup.h` bi vo hieu. Da sua lai: 3 dong do gio nam trong
  `#if !defined(USER_SETUP_LOADED)`.
- **SPI clock 27 MHz:** ILI9341 tren CYD chi nen chay <= 40 MHz. Ban truoc de
  **60 MHz** (commit f5c7c52 ghi nham "20MHz") gay soc/tearing + chi ve mot phan
  man hinh. Muon nhanh hon thu 40 MHz; neu con soc ha 20 MHz.
- **Kiem tra huong luc boot:** `applyRotation(1)` (mac dinh) roi kiem tra
  `tft.width() > tft.height()`; neu khong, tu thu rotation 0..3 va chon cai
  landscape dau tien, log `[TFT] CANH BAO...`. Lenh `/rot N` doi rotation
  ngay khong can nap lai.
- **Self-test** (`CYD_SELFTEST 1`, ham `drawSelfTest()`): 4 mau dac full man
  hinh -> vien trang 2px kin 4 canh -> nhan toa do 4 goc -> dong `SELFTEST
  320x240`. Muc dich: tach loi panel/driver khoi loi anh JPEG. Chay lai bang
  lenh `/test`. Log boot dung phai la:
  `[TFT] driver ILI9341, native 240x320 portrait -> rotation2 320x240 (UI 320x240)`.
- **Chong ghep frame hong:** `readFrame()` bo phan JPEG cu va bat dau lai khi
  gap SOI moi truoc khi thay EOI (frame bi cat giua chung do rot mang/camera
  treo) — tranh 2 nua frame dinh nhau gay vach ngang.
- Stream doc theo chunk (`Transfer-Encoding: chunked` cua esp_http_server);
  tu dong fallback neu server tra raw.
- `HTTPClient` dung `setReuse(true)` cho `/result` de giam cap phat sinh.
- Chi 1 client stream nen: neu mo them trinh duyet vao `:81/stream`,
  CYD se gianh frame va co the bi dut.
