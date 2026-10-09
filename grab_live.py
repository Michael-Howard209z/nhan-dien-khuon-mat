"""Tải 1 khung từ video_feed (đã khoanh + gắn nhãn) để xem kết quả nhận diện."""
import sys

import requests

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

with requests.get("http://127.0.0.1:5001/video_feed", stream=True, timeout=10) as r:
    buf = b""
    for chunk in r.iter_content(4096):
        buf += chunk
        s = buf.find(b"\xff\xd8")
        e = buf.find(b"\xff\xd9", s + 2)
        if s >= 0 and e >= 0:
            open("diag_live.jpg", "wb").write(buf[s : e + 2])
            print("da luu diag_live.jpg", e + 2 - s, "bytes")
            break
