# Tu tim IP nhau qua UDP (discovery)

He thong truoc day can **nguoi dung go tay IP**:
- ESP32 luu `push_url` (vd `http://192.168.1.9:5001/...`) trong NVS.
- Server can biet IP camera de keo luong PULL (`STREAM_URL`).

Neu IP doi (DHCP, doi may, doi mang) -> ca hai "mat" nhau. Goi them nay cho
**ca hai tu tim nhau**, khong can sua gi tay.

## Giao thuc (cong UDP 4321)

| Ai | Gui | Y nghia |
|----|-----|---------|
| Server | broadcast `FACESYNC\|SERVER\|http://<ip>:5001` moi 5s | "Toi la server day" |
| ESP32 | broadcast `FACESYNC\|DISCOVER\|<mode>` khi OFFLINE | "Ai la server?" |
| Server | unicast `FACESYNC\|SERVER\|http://...` tra loi DISCOVER | Tra loi truc tiep |
| ESP32 | broadcast `FACESYNC\|ESP32\|<ip>\|<mode>` moi 15s | "Minh la camera, ip nay" |

`mode` = `button` / `push` / `pull` (tu `BUTTON_ENABLE`, `PUSH_ENABLE`).

**Quy tac doi URL (quan trong, de khong lam gian doan phien dang chay tot):**
ESP32 CHI chuyen sang url moi khi dang **OFFLINE**. Dang online ma nghe thay
url khac -> giu nguyen va log canh bao.

**Ngoai le (bug da sua 2026-10-19):** neu url dang luu TRONG NVS thieu duong dan
API (vd `http://192.168.0.100:5001` thay vi `...:5001/api/esp32/frame`) thi
heartbeat van "online" (heartbeatPoll dung path co dinh) nhung POST anh se loi
404. Truong hop nay duoc phep doi URL NGAY, khong doi qua trang thai online.
Server cung da sua: broadcast day du endpoint, khong chi host:port.

> Bai hoc: khi doi URL bang discovery, PHAI giu nguyen duong dan API. Neu chi
> doi host:port thi se mat "/api/esp32/frame" -> he thong "trung" (heartbeat
> online) nhung khong diem danh duoc.

## 1. Server Linux

```bash
# copy 2 file moi len server (server.py + discovery.py) roi chay binh thuong
python3 server.py
```

Log phai co dong:
```
[discovery] chay ( cong UDP 4321, thong bao server moi 5s )
```

Mo cong UDP 4321 cho ESP32 hoi truc tiep (neu dung ufw):
```bash
sudo ufw allow 4321/udp
sudo ufw reload
```
> Khong mo duoc cung KHONG sao: server van broadcast duoc (di ra ngoai, khong
> bi chan), ESP32 van tu doi URL duoc. Chi mat phan "server biet IP camera
> truoc khi ESP32 gui duoc HTTP".

Kiem tra server dang phat hay chua (chay tren may khac cung duoc):
```bash
sudo tcpdump -ni any udp port 4321
# hoac:  python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1);s.bind(('',4321));print(s.recvfrom(256))"
```

### Server tu biet IP camera
Server nhan `FACESYNC|ESP32|<ip>|<mode>` -> hien thi trong
`/api/esp32/status` voi `esp32_ip_source = "udp"`.

Server **chi tu dien luong PULL** (`http://<ip>:81/stream`) khi firmware bao
`mode=pull` VA chua ai cau hinh `STREAM_URL`. Che do `button`/`push` (san pham
hien tai) thi **khong keo** `:81/stream` vi se day thuc camera dang OI va tranh
nguon anh.

## 2. ESP32-CAM

Firmware moi co them cong tac `DISCOVERY_ENABLE` (dau file `.ino`):

| Gia tri | Y nghia |
|---------|---------|
| `1` (mac dinh) | Bat discovery. Khong anh huong gi den camera. |
| `0` | Tat het. Dung de CHAN DOAN: neu nghi code moi gay loi thi tat di. |

Lenh Serial moi (Serial Monitor 115200):

| Lenh | Cong dung |
|------|-----------|
| `/cam` | Kiem tra camera NGAY (3 khung) **khong can bam nut** + in chan doan NGUON (lan reset truoc do co bi SAP NGUON/brownout khong). `3/3` = camera tot, `0/3` = loi phan cung. |
| `/camlog` | **Chan doan sau**: mo log VERBOSE cua driver camera (cam_hal / ll_cam) trong 1 lan test -> in ro tai sao fb_get() tra NULL. Xong tu tat. |
| `/cam2` | **Cap nguon lai sensor** (keo PWDN 600ms) roi kiem tra lai. Dung khi `/cam` tra `0/3` de xem sensor co bi "treo" khong. |
| `/disc` | Hoi toan mang "ai la server?" ngay luc do. |
| `/url` | Xem/sua URL server cu (van hoat dong binh thuong). |

Luc boot, board hoi toan mang **ngay 3 lan** (moi lan nghe 700ms) truoc khi vao
che do cho -> su dung dung URL tu dau, khong can cho 5-15s.

## 3. Thu nhanh khi ESP32 khong tim thay server

1. Serial Monitor 115200 -> bam `/disc`. Thay `[DISC] Tim thay server moi: ...`
   la discovery dang chay.
2. Khong thay gi? Kiem tra server co chay `discovery.py` khong (dong
   `[discovery] chay ( cong UDP 4321 ...)`).
3. Van khong duoc -> doi sang cach cu, khong can discovery: Serial `/url http://192.168.0.100:5001/api/esp32/frame`
   (luu vao NVS, giu sau khi mat dien).

## 4. Camera bi "NULL - khong co frame" (0/3 khung)

Day khong lien quan den discovery. Firmware da chan doan san:

```
[CAM] nong may khung 1: NULL - khong co frame (doi 4000 ms)
[CAM] nong may xong: 0/3 khung co frame
```

`esp_camera_init()` thanh cong + doc duoc `sensor PID: 0x5640` (OV5640) nghia la
**I2C/SCCB va nguon sensor TOT**. Nhung `fb_get()` tra NULL nghia la sensor
khong XUAT frame.

### 1. SENSOR BI TREO LUC COLD BOOT (nguyen nhan THUC TE da chot 2026-10-20)

Loi nay xay ra **moi lan tat/bat nguon**, khong lien quan den code hay phan cung
hong. Cau chung minh:

| Buoc | So chuoi PWDN | Ket qua |
|---|---|---|
| Binh thuong luc boot | 0 | `0/3` khung (NULL) |
| `camWakeInit()` (ban pwdn-powercycle) | 1 | `0/3` khung |
| `/cam2` sau do | 2 | **`3/3` khung** |

**Nguyen nhan goc:** luc chip reset, chan GPIO32 (PWDN) dang o trang thai
input/Hi-Z -> sensor tu len nguon trong khi XCLK (GPIO0) chua on dinh -> OV5640
bi "treo": I2C van doc duoc PID nhung khong xuat frame. Ban truoc chi
`digitalWrite(PWDN, LOW)` (vi rang sensor da TAT) -> khong thuc su cap lai.

**Da sua (firmware `2026-10-20-pwdn2-nodeinit`):** `camWakeInit()` cat/cap
nguon PWDN **2 LAN** truoc khi `esp_camera_init()` - log chung minh 1 lan la
khong du (lan 1: 0/3, lan 2: 3/3).

### 1b. *** KHONG DUOC GOI `esp_camera_deinit()` *** (gay "brick" camera)

Loi nghiem trong nhat, da xay ra ngay khi vua sua xong muc 1:

```
E cam_hal: cam_dma_config(509): frame buffer malloc failed
E camera: Camera config failed with error 0xffffffff
```

| Thu | Ket qua |
|---|---|
| `deinit()` -> `init()` | **luon that bai** (2/2 lan trong log), cam "brick" den khi reset chip |
| `init()` KHONG qua `deinit()` | luon thanh cong |

Ly do: `esp_camera_deinit()` khong giai phong sach bo dem DMA noi bo -> lan
init ke tiep phai cap phan bo THUA -> loi. Doan ghi chu tai `camSleep()` cua du
an da canh bao truoc dieu nay.

**Vi vay:** recovery chi duoc phep **cat/cap nguon sensor qua PWDN**, driver
camera van giu nguyen. `camWarmupRecover()` va lenh `/cam2` da sua theo huong
nay. Trong toan bo firmware da khong con goi `esp_camera_deinit()`.

**Kiem tra dang chay ban nao:** dong dau tien khi boot phai la
`[CFG] Firmware: 2026-10-20-pwdn-powercycle`. Neu van hien
`2026-10-19-udp-discovery` nghia la **CHUA NAP lai** (tat/bat nguon khong doi duoc firmware).

### 2. Nguon yeu (hay gap nhat truoc khi chot nguyen nhan tren)

5V tu cong USB may tinh/hub khong du dong cho camera + WiFi. Dung nguon 5V/2A
rieng hoac cap USB ngan, day day. Note: brownout chi bat duoc khi ESP32 MAT DIEN;
con sensor co the "chet" o muc dong thap hon ma ESP32 van chay tot -> **khong
hien dong brownout van co the la nguon yeu**.

### 3. Cap cam (FPC) long: thao ra lap lai chac chan, kiem tra day bi gian/duong.

### 4. Do sang moi truong qua thap + `BUTTON_SCAN_FLASH=0`: van cho frame nhung
den toi. Khong gay NULL, nhung anh se den.

### Chan doan nguon (dung khi nghi nguon yeu)

Lenh `/cam` in them dong:
```
[PWR] Lan reset truoc: SAP NGUON (BROWNOUT) - NGUON YEU!
```
Neu thay dong nay -> board bi mat dien luc that su. Cach fix (xep theo hieu qua):
1. Nguon 5V/2A RIENG (nguon sac dt tot), khong dung cong USB cua may tinh/hub.
2. Cap USB NGAN + day (cap dai/long -> suot ap -> brownout).
3. Them tu 470uF/10V (hoac 1000uF) dong dien vao chan 5V va GND cua board.

Van `0/3` ma khong co brownout -> thao/lap lai cap FPC cua camera.

**Cach loai tru ro rang:** nap `build_esp32_noDISC` (DISCOVERY_ENABLE=0) roi bam
nut. Van `0/3` -> loi PHAN CUNG, khong phai do code.
