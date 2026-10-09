"""Chẩn đoán camera (điện thoại / ESP32-CAM): thử các endpoint phổ biến.

Chạy:  python probe_camera.py
Đọc URL từ file .env (STREAM_URL) rồi thử các path thường gặp của
DroidCam / IP Webcam / ESP32-CAM / webcam local.

LƯU Ý: DroidCam và nhiều app camera chỉ phục vụ **một client tại một thời điểm**.
Nếu server đang chạy và đã giữ luồng, script này sẽ không nhận được khung hình
nào dù vẫn thấy HTTP 200. Hãy tắt server trước khi chạy nếu cần đọc khung thật.
ESP32-CAM chạy 2 cổng: web :80, stream :81 — script tự thử thêm :81/stream.
"""
import sys
from urllib.parse import urlsplit

import requests
from dotenv import load_dotenv

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

load_dotenv(".env", encoding="utf-8-sig")  # utf-8-sig: bỏ BOM nếu .env có

import os  # noqa: E402

STREAM_URL = (os.getenv("STREAM_URL") or "").strip()

# Nhiều app camera (DroidCam, IP Webcam) KHÔNG bật HTTPS -> phải dùng http://
if STREAM_URL.startswith("https://"):
    print("CANH BAO: ban nhap https://, nhung app camera thuong chi chay http://.")
    print("          Hay doi thanh: " + "http://" + STREAM_URL[len("https://"):])
    print()


def has_jpeg(resp) -> bool:
    """Đọc thử vài KB để xem có thật sự nhận được khung ảnh hay không."""
    head = resp.raw.read(65536, decode_content=False)
    return b"\xff\xd8" in head and b"\xff\xd9" in head


if not STREAM_URL.isdigit() and not STREAM_URL.startswith("rtsp://"):
    parts = urlsplit(STREAM_URL)
    origin = f"{parts.scheme}://{parts.netloc}"
    configured = parts.path
    # Thử https trước nếu người dùng nhập http, để phát hiện app chỉ bật https
    paths = list(dict.fromkeys([configured, "/video", "/mjpeg", "/shot.jpg",
                                "/videofeed", "/stream", "/capture", "/"]))
    print(f"Host camera: {origin}\n")
    found_live = False
    for path in paths:
        url = origin + path
        try:
            r = requests.get(url, timeout=6, stream=True, headers={"User-Agent": "Mozilla/5.0"})
            ctype = r.headers.get("Content-Type")
            is_stream = "mixed-replace" in (ctype or "")
            # Chỉ đọc ảnh thật ở endpoint luồng; các path khác chỉ cần header
            frame = has_jpeg(r) if (is_stream or (ctype or "").startswith("image")) else False
            mark = ""
            if frame:
                found_live = True
                mark = "  <-- LUONG VIDEO (da nhan duoc khung hinh)"
            elif is_stream:
                mark = "  <-- co multipart nhung CHUA nhan duoc khung hinh"
            print(f"{path or '/':12} -> {r.status_code} | {ctype}{mark}")
            r.close()
        except Exception as exc:  # noqa: BLE001
            print(f"{path or '/':12} -> LOI: {exc}")
    # ESP32-CAM: stream ở cổng 81, web ở cổng 80 — thử thêm nếu chưa thấy luồng.
    try:
        host = parts.hostname or ""
        if host and parts.port in (None, 80) and not found_live:
            esp_url = f"{parts.scheme}://{host}:81/stream"
            print(f"\nThu them ESP32-CAM (:81/stream): {esp_url}")
            try:
                r = requests.get(esp_url, timeout=6, stream=True,
                                 headers={"User-Agent": "Mozilla/5.0"})
                ctype = r.headers.get("Content-Type")
                frame = has_jpeg(r) if ("mixed-replace" in (ctype or "")
                                        or (ctype or "").startswith("image")) else False
                print(f":81/stream   -> {r.status_code} | {ctype}"
                      f"{'  <-- LUONG VIDEO ESP32' if frame else ''}")
                r.close()
                found_live = found_live or frame
            except Exception as exc:  # noqa: BLE001
                print(f":81/stream   -> LOI: {exc}")
    except Exception:  # noqa: BLE001
        pass
    if not found_live:
        print("\nLuu y: neu server dang chay, app camera co the da ban het 'cho' cho server.")
        print("      Hay tat server roi chay lai script nay de kiem tra lai.")
else:
    print("STREAM_URL là webcam local/rtsp, không kiểm tra HTTP:", STREAM_URL)
