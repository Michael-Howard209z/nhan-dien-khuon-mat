# -*- coding: utf-8 -*-
"""Tự động phát hiện server (cho ESP32-CAM) và camera IP (cho server) qua UDP broadcast.

Vì sao cần:
  * IP máy chạy server đổi theo DHCP -> ESP32 giữ url cũ trong NVS là "mất" server.
  * Server cần biết IP camera để tự điền luồng PULL (STREAM_URL) mà không gõ tay.

Cách hoạt động (cổng UDP 4321, cả hai phía):
  * Server broadcast mỗi ~5s:   ``FACESYNC|SERVER|http://<ip>:<camera_port>``
    (gói đi ra ngoài -> Windows Firewall mặc định KHÔNG chặn đường ra).
  * ESP32 nghe thấy, nếu đang OFFLINE thì chuyển sang url mới và lưu NVS.
  * ESP32 hỏi:                   ``FACESYNC|DISCOVER|<mode>``
    Server trả lời trực tiếp (unicast) + broadcast lại.
  * ESP32 báo danh:              ``FACESYNC|ESP32|<ip>|<mode>``
    Server dùng để biết IP camera ngay cả khi ESP32 chưa gửi được HTTP nào
    (mode: ``push`` / ``button`` / ``pull``).

Lưu ý firewall (đường VÀO tới server): cần 1 rule cho UDP 4321, ví dụ
  netsh advfirewall firewall add rule name="face-discovery-udp4321" dir=in action=allow protocol=UDP localport=4321
Không có rule này thì server vẫn chạy tốt (ESP32 tự tìm server nhờ broadcast),
chỉ mất phần "server tự biết IP camera trước khi ESP32 kết nối HTTP".
"""

from __future__ import annotations

import socket
import threading
import time

PROTO = "FACESYNC"      # tiền tố bắt buộc của mọi gói tin
DISC_MAGIC = PROTO      # tên gọi khác cho tiện đọc ở nơi khác (firmware dùng chung)
DISC_PORT = 4321        # cổng UDP dùng chung cho cả hai phía
_ANNOUNCE_EVERY = 5.0   # giây giữa 2 lần server broadcast


def _log(msg: str) -> None:
    print(f"[discovery] {msg}", flush=True)


def _is_private_ipv4(ip: str) -> bool:
    """Chỉ nhận IPv4 địa phương 'thật': bỏ loopback / 169.254 (APIPA) / multicast."""
    parts = ip.split(".")
    if len(parts) != 4:
        return False
    try:
        nums = [int(p) for p in parts]
    except ValueError:
        return False
    if any(n > 255 for n in nums):
        return False
    if nums[0] == 127 or nums[0] == 0:
        return False
    if nums[0] == 169 and nums[1] == 254:
        return False
    if nums[0] >= 224:
        return False
    return True


def _is_lan_ipv4(ip: str) -> bool:
    """Dải LAN nội bộ RFC1918 (10/8, 172.16/12, 192.168/16) - ưu tiên chọn trước.

    Tránh máy có VPN (vd 26.x, 100.64.x) bị chọn nhầm làm "IP server".
    """
    parts = ip.split(".")
    if not _is_private_ipv4(ip):
        return False
    a, b = int(parts[0]), int(parts[1])
    return (a == 10 or a == 192 and b == 168 or a == 172 and 16 <= b <= 31)


def _best_local_ip(candidates: list) -> str | None:
    """Chọn IPv4 LAN tốt nhất trong danh sách (ưu tiên RFC1918, không lấy VPN)."""
    for ip in candidates:
        if _is_lan_ipv4(ip):
            return ip
    for ip in candidates:
        if _is_private_ipv4(ip):
            return ip
    return None


def local_ip_for(peer_ip: str | None = None) -> str:
    """IP địa phương sẽ dùng để nói chuyện với ``peer_ip``.

    Thủ thuật chuẩn: tạo socket UDP, ``connect()`` (không gửi packet nào) rồi đọc
    ``getsockname()`` -> hệ điều chọn đúng interface đi tới peer. Không cần quét
    mạng, không đoán, và tự đúng kể cả khi máy có VPN / nhiều card mạng.
    """
    if peer_ip and _is_private_ipv4(peer_ip):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((peer_ip, 9))          # port 9 (discard) - không gửi gì
            ip = s.getsockname()[0]
            if _is_private_ipv4(ip):
                return ip
        except OSError:
            pass
        finally:
            s.close()

    # Dự phòng: lấy IPv4 của hostname (bỏ loopback, ưu tiên dải LAN thật,
    # không lấy địa chỉ VPN như 26.x / 100.64.x).
    try:
        ips = [info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None,
                                                         socket.AF_INET)]
        picked = _best_local_ip(ips)
        if picked:
            return picked
    except OSError:
        pass
    return "127.0.0.1"


class DiscoveryService:
    """Luồng nền: broadcast tên server + trả lời câu hỏi DISCOVER + nghe ESP32."""

    def __init__(self, camera_port: int, on_camera=None,
                 announce_every: float = _ANNOUNCE_EVERY) -> None:
        self._camera_port = camera_port
        self._on_camera = on_camera          # callback(ip: str, mode: str)
        self._announce_every = max(1.0, float(announce_every))
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._sock: socket.socket | None = None
        self._last_camera_ip: str | None = None
        self._last_camera_mode: str = ""

    # ------------------------- giao diện dùng ngoài -------------------------
    @property
    def last_camera_ip(self) -> str | None:
        return self._last_camera_ip

    @property
    def last_camera_mode(self) -> str:
        return self._last_camera_mode

    def start(self) -> bool:
        if self._thread and self._thread.is_alive():
            return True
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:                                    # cho phep nhan broadcast tren Windows
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            except OSError:
                pass
            sock.bind(("", DISC_PORT))
            sock.settimeout(0.5)
            self._sock = sock
        except OSError as exc:
            _log(f"khong mo duoc UDP {DISC_PORT}: {exc} -> tat chuc nang tu tim")
            return False

        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="face-discovery",
                                        daemon=True)
        self._thread.start()
        _log(f"chay ( cong UDP {DISC_PORT}, thong bao server moi "
             f"{self._announce_every:.0f}s )")
        return True

    def stop(self) -> None:
        self._stop.set()
        t = self._thread
        if t and t.is_alive():
            t.join(timeout=2.0)
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def announce_now(self) -> None:
        """Goi broadcast ngay (dung khi server vua doi IP)."""
        self._broadcast(self._server_url())

    # ----------------------------- bên trong -------------------------------
    def _server_url(self) -> str:
        # GUI DAY DU endpoint (khong chi host:port): ESP32 dung truc tiep lam
        # push_url. Neu chi gui "http://ip:5001" thi khi nhan vao NVS se MAT
        # duong dan "/api/esp32/frame" -> POST anh loi 404 (heartbeat van chay
        # vi heartbeatPoll dung path co dinh).
        return (f"http://{local_ip_for(self._last_camera_ip)}"
                f":{self._camera_port}/api/esp32/frame")

    def _broadcast(self, payload: str) -> None:
        sock = self._sock
        if not sock:
            return
        data = payload.encode("utf-8", "replace")
        targets = ["255.255.255.255"]
        sub = self._subnet_broadcast(self._last_camera_ip)
        if sub:
            targets.append(sub)
        for target in targets:
            try:
                sock.sendto(data, (target, DISC_PORT))
            except OSError:
                continue

    @staticmethod
    def _subnet_broadcast(peer_ip: str | None) -> str | None:
        """Địa chỉ broadcast /24 của dải có camera (router đôi khi chặn 255.255.255.255)."""
        if not peer_ip or not _is_private_ipv4(peer_ip):
            return None
        head = peer_ip.rsplit(".", 1)[0]
        return f"{head}.255" if head != peer_ip else None

    def _handle(self, raw: bytes, addr: tuple) -> None:
        text = raw.decode("utf-8", "replace").strip("\r\n\x00 ")
        if not text.startswith(PROTO + "|"):
            return
        fields = text.split("|")
        if len(fields) < 2:
            return
        kind = fields[1]
        ip = addr[0]

        if kind == "DISCOVER":
            # ESP32 hoi "ai la server?" -> tra loi truc tiep + broadcast lai.
            url = self._server_url()
            _log(f"ESP32 hoi (mode={fields[2] if len(fields) > 2 else '?'} "
                 f"tu {ip}) -> tra loi {url}")
            try:
                if self._sock:
                    self._sock.sendto(f"{PROTO}|SERVER|{url}".encode(),
                                      (ip, addr[1] or DISC_PORT))
            except OSError:
                pass
            self._broadcast(url)
            self._note_camera(ip, fields[2] if len(fields) > 2 else "")

        elif kind == "ESP32":
            cam_ip = fields[2] if len(fields) > 2 and fields[2] else ip
            mode = fields[3] if len(fields) > 3 else ""
            self._note_camera(cam_ip, mode)

    def _note_camera(self, ip: str, mode: str) -> None:
        # Chi chap nhan IPv4 dải LAN that (10/8, 172.16/12, 192.168/16) - khong
        # lay dia chi VPN (26.x, 100.64.x) hoac APIPA lam "ip camera".
        if not _is_lan_ipv4(ip):
            return
        changed = (ip != self._last_camera_ip) or (mode != self._last_camera_mode)
        self._last_camera_ip = ip
        if mode:
            self._last_camera_mode = mode
        if changed:
            _log(f"biet ip camera: {ip} (che do {self._last_camera_mode or 'khong ro'})")
            if self._on_camera:
                try:
                    self._on_camera(ip, self._last_camera_mode)
                except Exception as exc:            # callback loi van khong chan luong
                    _log(f"callback loi: {exc}")

    def _run(self) -> None:
        last_announce = 0.0
        while not self._stop.is_set():
            now = time.time()
            if now - last_announce >= self._announce_every:
                last_announce = now
                self._broadcast(f"{PROTO}|SERVER|{self._server_url()}")
            try:
                raw, addr = self._sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self._handle(raw, addr)
            except Exception as exc:
                _log(f"loi xu ly goi tin tu {addr[0]}: {exc}")
