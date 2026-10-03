"""Chẩn đoán camera điện thoại: thử các endpoint phổ biến trên host của STREAM_URL.

Chạy:  python probe_camera.py
Đọc URL từ file .env (STREAM_URL) rồi thử các path thường gặp của
DroidCam / IP Webcam / DroidCam / webcam local.
"""
import sys
from urllib.parse import urlsplit

import requests
from dotenv import load_dotenv

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

load_dotenv(".env")

import os  # noqa: E402

STREAM_URL = (os.getenv("STREAM_URL") or "").strip()
if not STREAM_URL.isdigit() and not STREAM_URL.startswith("rtsp://"):
    origin = "{0.scheme}://{0.netloc}".format(urlsplit(STREAM_URL))
    configured = urlsplit(STREAM_URL).path
    paths = list(dict.fromkeys([configured, "/video", "/mjpeg", "/shot.jpg", "/videofeed", "/"]))
    print(f"Host camera: {origin}\n")
    for path in paths:
        url = origin + path
        try:
            r = requests.get(url, timeout=6, stream=True, headers={"User-Agent": "Mozilla/5.0"})
            head = r.raw.read(32)
            ctype = r.headers.get("Content-Type")
            mark = "  <-- LUONG VIDEO" if "mixed-replace" in (ctype or "") or head[:2] == b"\xff\xd8" else ""
            print(f"{path or '/':12} -> {r.status_code} | {ctype} | {head!r}{mark}")
            r.close()
        except Exception as exc:  # noqa: BLE001
            print(f"{path or '/':12} -> LOI: {exc}")
else:
    print("STREAM_URL là webcam local/rtsp, không kiểm tra HTTP:", STREAM_URL)
