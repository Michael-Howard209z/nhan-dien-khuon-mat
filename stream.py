"""Đọc khung hình từ điện thoại làm camera IP (MJPEG/HTTP), ESP32-CAM
hoặc webcam local.

Hỗ trợ:
  - IP Webcam (Android):   http://<ip-phone>:8080/video
  - DroidCam:              http://<ip-phone>:4747/video
  - ESP32-CAM (PULL):      http://<ip-esp32>:81/stream   (khuyên dùng)
                           http://<ip-esp32>/capture     (1 ảnh, chậm hơn)
  - Ảnh chụp một lần:      http://<ip-phone>:8080/shot.jpg
  - Webcam local / RTSP:   "0", "1", "rtsp://..."

ESP32-CAM có 2 chế độ (xem firmware CameraWebServer_copy_*/.ino):
  * PULL (mặc định khuyên dùng): STREAM_URL=http://<ip-esp32>:81/stream,
    PUSH_ENABLE=0. Server kéo MJPEG, nhận diện + điểm danh như camera thường.
  * PUSH: ESP32 POST JPEG tới /api/esp32/frame, PUSH_ENABLE=1. Không cần STREAM_URL.
"""
import threading
import time
from urllib.parse import urlsplit, urlunsplit

import cv2
import numpy as np
import requests

# Đường dẫn hay gặp của các app camera + ESP32-CAM. Dùng để tự dò khi người
# dùng nhập sai (thiếu path hoặc nhầm cổng 80/81 của ESP32).
FALLBACK_PATHS = ("/video", "/videofeed", "/mjpeg", "/shot.jpg",
                  "/stream", "/capture", "/jpg", "/cam")


def _err_scheme(url: str) -> str:
    """Thông báo khi dán nhầm https:// mà app camera chỉ chạy http://."""
    plain = "http://" + url[len("https://"):] if url.startswith("https://") else url
    return (
        f"Không dùng được {url} — điện thoại chỉ phục vụ HTTP thường, "
        f"không có HTTPS. Hãy nhập: {plain}"
    )


def _err_net(url: str, exc: Exception) -> str:
    """Thông báo khi không kết nối được tới máy chủ camera."""
    parts = urlsplit(url)
    where = parts.netloc or url
    if isinstance(exc, requests.exceptions.ConnectTimeout):
        return f"Không kết nối được {where} — quá thời gian chờ. Kiểm tra IP/port."
    if isinstance(exc, requests.exceptions.ConnectionError):
        return (
            f"Không kết nối được {where}. Kiểm tra: điện thoại đã bật app camera, "
            f"đã bấm Start chưa, và điện thoại + máy tính có cùng WiFi không."
        )
    return f"Lỗi kết nối {where}: {exc}"


def _err_status(url: str, code: int) -> str:
    """Thông báo khi máy chủ trả mã lỗi HTTP."""
    parts = urlsplit(url)
    hint = {
        401: " (app camera yêu cầu mật khẩu)",
        403: " (bị từ chối, kiểm tra mật khẩu)",
        404: " — sai đường dẫn, thử /video hoặc /videofeed",
    }.get(code, "")
    return f"{url} trả lỗi HTTP {code}{hint}"


def _err_not_media(url: str) -> str:
    """Máy chủ trả 200 nhưng ra trang web thay vì luồng video.

    Hay gặp với DroidCam/IP Webcam: app chỉ phục vụ **một** client tại một thời
    điểm, nên khi server đã giữ luồng thì yêu cầu tiếp theo nhận về HTML.
    """
    return (
        f"{url} trả về trang web chứ không phải luồng video. Thường do app camera "
        f"chỉ cho phép 1 thiết bị kết nối tại một lúc — hãy đóng các ứng dụng/tab "
        f"khác đang xem cùng luồng này rồi bấm “Kết nối lại”."
    )


class PhoneCamera:
    def __init__(self, on_frame=None, read_timeout: float = 8.0):
        self.on_frame = on_frame
        self.read_timeout = read_timeout
        self._url = ""
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._state = {
            "url": "",
            "connected": False,
            "error": "",
            "fps": 0.0,
            "resolution": "",
            "last_frame_ts": 0.0,
            "frame_count": 0,
        }

    # ------------------------------------------------------------------ public
    def set_url(self, url: str, restart: bool = False):
        url = (url or "").strip()
        if url == self._url and not restart:
            return
        self._url = url
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3)
        self._stop.clear()
        with self._lock:
            self._state.update(url=url, connected=False, error="", fps=0.0, resolution="")
        if url:
            self._thread = threading.Thread(target=self._loop, daemon=True, name="phone-camera")
            self._thread.start()

    def stop(self):
        self._stop.set()

    def status(self):
        with self._lock:
            st = dict(self._state)
        st["last_frame_age"] = (
            round(time.time() - st["last_frame_ts"], 2) if st["last_frame_ts"] else None
        )
        return st

    # ----------------------------------------------------------------- private
    def _mark(self, **kw):
        with self._lock:
            self._state.update(kw)

    def _push_jpeg(self, jpeg: bytes, min_interval: float = 1.0 / 15.0):
        """Giải JPEG -> frame cho hub, nhưng BỎ khung dồn để giảm lag.

        Luồng MJPEG của ESP32/điện thoại có thể tới 20-30fps trong khi
        process_loop chỉ tiêu thụ ~10-12fps (mỗi khung tốn 80-150ms inference).
        Giải mã mọi khung trong thread mạng vừa tốn CPU (tranh với engine) vừa
        để hub luôn bị ghi đè -> age dao động. Chỉ nhận tối đa ~15fps, khung đến
        sớm hơn thì bỏ (camera mới nhất sẽ tới ngay sau, không mất gì).
        """
        if not jpeg:
            return
        now = time.time()
        with self._lock:
            if now - self._state["last_frame_ts"] < min_interval:
                return  # bỏ khung dồn — khung mới nhất sẽ tới ngay sau
        frame = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if frame is None:
            return
        h, w = frame.shape[:2]
        with self._lock:
            self._state["last_frame_ts"] = time.time()
            self._state["frame_count"] += 1
            self._state["resolution"] = f"{w}x{h}"
            self._state["connected"] = True
            self._state["error"] = ""
        if self.on_frame:
            self.on_frame(frame)

    def _loop(self):
        url = self._url
        while not self._stop.is_set() and url == self._url:
            try:
                if url.isdigit() or url.startswith("rtsp://"):
                    self._run_capture(url)
                else:
                    self._run_http(url)
                self._mark(connected=False, error="Luồng kết nối đã ngắt")
            except Exception as exc:  # noqa: BLE001 - muốn hiển thị lỗi cho người dùng
                self._mark(connected=False, error=str(exc))
            if not self._stop.is_set():
                time.sleep(2)

    def _connect(self, url: str):
        """Mở luồng HTTP, tự sửa 3 lỗi cấu hình hay gặp nhất.

        1. Dán nhầm ``https://`` trong khi app camera (DroidCam, IP Webcam,
           ESP32-CAM) chỉ chạy ``http://`` -> thử lại bằng http.
        2. Sai đường dẫn -> dò lần lượt các path phổ biến (kể cả /stream,
           /capture của ESP32), nhưng chỉ khi đã nói chuyện được với máy chủ.
        3. ESP32-CAM chạy 2 cổng: web :80, stream :81. Nhập http://<ip-esp32>/
           (cổng 80) sẽ ra trang HTML — tự thử thêm http://<ip-esp32>:81/stream.

        Trả về ``(response, url_dang_dung)``.
        """
        headers = {"User-Agent": "Mozilla/5.0 (face-server)"}

        # (1) thử các scheme, giữ nguyên path
        order = [url]
        if url.startswith("https://"):
            order.append("http://" + url[len("https://"):])
        elif url.startswith("http://"):
            order.append("https://" + url[len("http://"):])

        first_error = None
        reachable = None  # scheme đã nói chuyện được -> dùng để dò path
        for candidate in order:
            try:
                resp = requests.get(
                    candidate, stream=True,
                    timeout=(5, self.read_timeout), headers=headers,
                )
            except requests.exceptions.SSLError:
                first_error = first_error or _err_scheme(candidate)
                continue
            except requests.exceptions.RequestException as exc:
                first_error = first_error or _err_net(candidate, exc)
                continue
            if resp.status_code < 400:
                # 200 nhưng ra HTML = app camera đã bận (chỉ nhận 1 client).
                # Luồng video thật luôn là multipart hoặc ảnh, không bao giờ là text/*.
                ctype = (resp.headers.get("Content-Type") or "").lower()
                if ctype.startswith("text/"):
                    resp.close()
                    reachable = reachable or candidate
                    first_error = first_error or _err_not_media(candidate)
                    continue
                self._mark(url=candidate, connected=True, error="")
                return resp, candidate
            code = resp.status_code
            resp.close()
            reachable = reachable or candidate
            first_error = first_error or _err_status(candidate, code)

        # (2) máy chủ sống nhưng path sai -> dò các path phổ biến.
        # Chỉ dò khi đã nói chuyện được với máy chủ; nếu không (IP sai, mất
        # WiFi) thì báo lỗi mạng ngay, đừng chờ thêm mấy chục giây vô ích.
        if reachable:
            base = urlsplit(reachable)
            for path in FALLBACK_PATHS:
                candidate = urlunsplit((base.scheme, base.netloc, path, "", ""))
                try:
                    resp = requests.get(
                        candidate, stream=True,
                        timeout=(5, self.read_timeout), headers=headers,
                    )
                except requests.exceptions.RequestException:
                    continue
                if resp.status_code < 400:
                    ctype = (resp.headers.get("Content-Type") or "").lower()
                    if ctype.startswith("text/"):
                        resp.close()
                        first_error = first_error or _err_not_media(candidate)
                        continue
                    self._mark(url=candidate, connected=True, error="")
                    return resp, candidate
                resp.close()

            # (3) ESP32-CAM: web ở :80, stream ở :81. Người dùng hay nhập
            # http://<ip-esp32>/ (ra trang HTML) — thử sang :81/stream.
            try:
                host = (base.hostname or "")
                if host and base.port in (None, 80):
                    esp_url = f"{base.scheme}://{host}:81/stream"
                    try:
                        resp = requests.get(
                            esp_url, stream=True,
                            timeout=(5, self.read_timeout), headers=headers,
                        )
                    except requests.exceptions.RequestException:
                        resp = None
                    if resp is not None and resp.status_code < 400:
                        ctype = (resp.headers.get("Content-Type") or "").lower()
                        if not ctype.startswith("text/"):
                            self._mark(url=esp_url, connected=True, error="")
                            return resp, esp_url
                        resp.close()
                        first_error = first_error or (
                            f"{esp_url} trả về trang web. ESP32-CAM dùng cổng 81 "
                            f"cho stream — hãy nhập đúng: {esp_url}")
            except Exception:  # noqa: BLE001
                pass

        raise RuntimeError(first_error or f"Không tìm thấy luồng video tại {url}")

    def _run_http(self, url: str):
        # _connect tự ghi URL đã sửa vào trạng thái (https -> http, sai path -> /video)
        resp, _ = self._connect(url)
        with resp:
            ctype = (resp.headers.get("Content-Type") or "").lower()

            if "multipart" in ctype or "x-mixed-replace" in ctype:
                self._mark(connected=True, error="")
                self._read_mjpeg(resp)
            elif ctype.startswith("image"):
                self._mark(connected=True, error="")
                self._push_jpeg(resp.content)
                time.sleep(0.15)
            else:
                # Thử parse như MJPEG trước (một số server không khai báo Content-Type).
                self._mark(connected=True, error="")
                self._read_mjpeg(resp)

    def _read_mjpeg(self, resp):
        buf = b""
        recent = []
        for chunk in resp.iter_content(chunk_size=16384):
            if self._stop.is_set():
                break
            if not chunk:
                continue
            buf += chunk
            # Chặn body rác phình RAM khi stream lỗi (không thấy JPEG lâu).
            if len(buf) > 512 * 1024:
                buf = buf[-4096:]
                continue
            while True:
                start = buf.find(b"\xff\xd8")  # JPEG SOI
                if start < 0:
                    buf = buf[-1:] if buf.endswith(b"\xff") else b""
                    break
                end = buf.find(b"\xff\xd9", start + 2)  # JPEG EOI
                if end < 0:
                    buf = buf[start:]
                    # Giữ tối đa 1 khung dở (tránh cộng dồn nhiều khung cũ gây lag
                    # cảm nhận: luôn ưu tiên khung MỚI nhất, bỏ khung giữa).
                    if len(buf) > 160 * 1024:
                        buf = buf[-160 * 1024:]
                    break
                self._push_jpeg(buf[start : end + 2])
                buf = buf[end + 2 :]
                now = time.time()
                recent = [t for t in recent if now - t < 2.0]
                recent.append(now)
                self._mark(fps=round(len(recent) / 2.0, 1))

    def _run_capture(self, source: str):
        idx = int(source) if source.isdigit() else source
        cap = cv2.VideoCapture(idx)
        if not cap.isOpened():
            cap.release()
            raise RuntimeError(f"Không mở được camera: {source}")
        self._mark(connected=True, error="")
        fails = 0
        while not self._stop.is_set():
            ok, frame = cap.read()
            if not ok:
                fails += 1
                if fails >= 15:
                    cap.release()
                    raise RuntimeError(f"Camera {source} mất dữ liệu")
                time.sleep(0.05)
                continue
            fails = 0
            h, w = frame.shape[:2]
            ok_jpeg, buf = cv2.imencode(".jpg", frame)
            if ok_jpeg:
                self._push_jpeg(buf.tobytes())
                with self._lock:
                    self._state["fps"] = 30.0
                    self._state["resolution"] = f"{w}x{h}"
            time.sleep(0.01)
        cap.release()
