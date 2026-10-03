"""Đọc khung hình từ điện thoại làm camera IP (MJPEG/HTTP) hoặc webcam local.

Hỗ trợ:
  - IP Webcam (Android):   http://<ip-phone>:8080/video
  - DroidCam:              http://<ip-phone>:4747/video
  - Ảnh chụp một lần:      http://<ip-phone>:8080/shot.jpg
  - Webcam local / RTSP:   "0", "1", "rtsp://..."
"""
import threading
import time

import cv2
import numpy as np
import requests


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

    def _push_jpeg(self, jpeg: bytes):
        if not jpeg:
            return
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

    def _run_http(self, url: str):
        headers = {"User-Agent": "Mozilla/5.0 (face-server)"}
        with requests.get(
            url, stream=True, timeout=(5, self.read_timeout), headers=headers
        ) as resp:
            resp.raise_for_status()
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
        for chunk in resp.iter_content(chunk_size=8192):
            if self._stop.is_set():
                break
            if not chunk:
                continue
            buf += chunk
            while True:
                start = buf.find(b"\xff\xd8")  # JPEG SOI
                if start < 0:
                    buf = buf[-1:] if buf.endswith(b"\xff") else b""
                    break
                end = buf.find(b"\xff\xd9", start + 2)  # JPEG EOI
                if end < 0:
                    buf = buf[start:]
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
