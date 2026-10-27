# Man LCD hien thi ten hoc sau diem danh (ESP32-C3 + LCD1602 PCF8574 I2C)

## 1. Tong quan

He thong co 3 bo phan:

| Bo phan | Vai tro | Firmware |
|---|---|---|
| ESP32-CAM | Chup anh, POST len server, luu ket qua nhan dien | `firmware_esp32cam/firmware_esp32cam.ino` (>= `2026-10-27-lcd1`) |
| Server Python | Nhan dien YuNet + SFace, ghi diem danh 1 lan/buoi | `server.py` |
| ESP32-C3 + LCD1602 | Doc ket qua tu ESP32-CAM qua HTTP, hien thi ten len LCD | `firmware_esp32display/esp32lcd/esp32lcd.ino` |

Luu luu du lieu:

```
[Hoc sinh bam nut GPIO13 o ESP32-CAM]
         |
         v
ESP32-CAM: chup anh lien tuc 15s -> POST len server (capture=1&session_id=...)
         |
         v
Server: nhan dien, ghi diem danh (1 lan/buoi), luu ket qua (matched/full_name/already/...)
         |
         +-- HTTP GET /result (tra JSON) <-- [ESP32-C3] poll moi 1s
                   |
                   v
               LCD1602 hien thi ten hoc sinh / thong bao
```

ESP32-C3 **khong tu nhan dien** — no chi hien thi. Toan bo logic nhan dien van o ESP32-CAM + server nhu hien tai.

## 2. Yeu cau phan cung

- ESP32-C3 Super Mini (hoac ESP32C3 Dev Module)
- LCD1602 + backpack PCF8574 I2C (dia chi 0x27 hoac 0x3F)
- Nối dây:
  - VCC -> 5V (hoac 3V3 neu LCD duoc)
  - GND -> GND
  - SDA -> GPIO4
  - SCL -> GPIO5

<img src="images/esp32c3.png" width="300" alt="Ảnh 1">
<img src="images/lcd.png" width="300" alt="Ảnh 2">

- ESP32-CAM da chay firmware `2026-10-27-lcd1` hoac cao hon (co endpoint `GET /result`).
- Ca 2 board cung mot WiFi (SSID `Test` / pass `88888888`).

## 3. Thu vien can cai (Arduino IDE Library Manager)

- `LiquidCrystal_PCF8574` (Mathertel) — da co
- Khong can `ArduinoJson` (parse JSON don gian bang `strstr`).

## 4. Nap firmware

### ESP32-CAM (co endpoint /result)

```
flash_esp32.bat COMx
```

FQBN bat buoc:
`esp32:esp32:esp32cam:CPUFreq=240,FlashFreq=80,FlashMode=dio,PartitionScheme=huge_app,DebugLevel=none`

Kiem tra Serial Monitor (115200): dong dau phai hien
`[CFG] Firmware: 2026-10-27-lcd1`

### ESP32-C3 + LCD

```
flash_esp32c3.bat COMx
```

FQBN:
`esp32:esp32:esp32c3:CDCOnBoot=cdc`

ESP32-C3 Super Mini co the can nhan nut **BOOT** luc reset de vao che do nap (tu thao tu cua arduino-cli neu dung CDCOnBoot=cdc).

## 5. Cau hinh IP ESP32-CAM

LCD can biet IP cua ESP32-CAM de go `GET /result`. 2 cach:

### Cach A: Tu dong qua UDP discovery (khuyen dung)

Khi ESP32-CAM va LCD cung mang, LCD gui `FACESYNC|CAM?` (UDP 4321 broadcast). ESP32-CAM tra loi ngay IP cua no. LCD luu IP vao NVS (nho qua khi mat dien).

Lenh Serial tren LCD: `/find` -> tim ngay.

### Cach B: Khai bao thu cong

```
LCD Serial (115200): /ip 192.168.0.101
```

IP nay la IP cua **ESP32-CAM** (xem o Serial cua cam: `WiFi connected, IP: ...`), KHONG phai IP may server.

## 6. Lenh Serial tren LCD (115200)

| Lenh | Nghia |
|---|---|
| `/help` | Hien thi danh sach lenh |
| `/ip` | Xem IP cam hien tai + trang thai ONLINE/OFFLINE |
| `/ip 192.168.0.101` | Dat IP ESP32-CAM (luu NVS) |
| `/find` | Tim ESP32-CAM qua UDP broadcast ngay lap tuc |

## 7. Man hinh LCD (16x2)

| Trang thai | Dong 1 | Dong 2 | Nghia |
|---|---|---|---|
| Idle | `San sang diem danh` | `Bam nut tren cam` | Cho bam nut o cam |
| Scan | `Dang quet...` | `Giu mat truoc cam` | Cam dang POST anh 15s |
| OK | `Nguyen Van A` | `Diem danh OK` | Nhan dien thanh cong, da ghi 1 lan |
| Already | `Nguyen Van A` | `Da diem danh` | Da diem danh buoi nay roi (khong ghi them) |
| No match | `Khong nhan ra` | `Thu lai nhe` | 15s het nhung khong khop ai |
| Cam err | `Loi camera` | `Kiem tra cam/nguon` | ESP32-CAM khong lay duoc khung |
| Net err | `Loi mang/server` | `Kiem tra WiFi cam` | Khong doc duoc /result |
| Offline | `Mat ket noi cam` | `192.168.0.101` | Mat ket noi WiFi den cam |

Ten dai hon 16 ky tu se chay chu (marquee) o dong 1.

## 8. Giai thich `already: true`

`already: true` (LCD hien `Da diem danh`) co 2 nghia, ca 2 deu **dung** va khong ghi them dong DB:

1. **Trong cung 15s**: khung sau khung dau tien cung `session_id` -> server tra `already: true` (dede trong `scan_sessions` RAM).
2. **Buoi/ngay nay da diem danh**: khoa chinh DB la `(student_id, day, session)`. Neu hoc sinh da co dong trong buoi nay, khung moi (khu `session_id` moi) van tra `already: true`, server khong them dong moi (chi cong `hits`).

Vay neu bam nut lan 2 cung ngay cung buoi cho cung 1 hoc sinh, LCD se hien ten + `Da diem danh` — la **duoc quy dinh**, khong phai bug.

## 9. Xu ly loi

| Loi | Nguyen nhan likely | Cach khac phuc |
|---|---|---|
| LCD hien `Mat ket noi cam` | Sai IP cam, cam mat WiFi, hoac `/result` chua sap xep | Kiem tra `/ip`, go `/find`, cam co chay firmware >= `2026-10-27-lcd1` khong |
| LCD hien `Loi mang/server` | ESP32-CAM bat nhung server mat, hoac WiFi cam yeu | Kiem tra server `192.168.0.50:5001` con song, cam o cung LAN |
| LCD khong hien gi /`KHONG thay PCF8574` | Day I2C sao, dia chi 0x3F thay 0x27 | Code tu dong quet 0x27/0x3F. Kiem tra SDA=GPIO4, SCL=GPIO5 |
| Compile LCD loi `ArduinoJson.h` | Thu vien bo trong file cu | File hien da xoa ArduinoJson. Xoa build, compile lai |

## 10. Cau truc thu muc

```
firmware_esp32display/esp32lcd/esp32lcd.ino   # firmware ESP32-C3 + LCD
flash_esp32c3.bat                              # script nap ESP32-C3
build/                                         # ket qua compile (tu dong sinh)
```
