"""Kiểm thử tự động toàn bộ API của face_server.

Chạy server trước:  python server.py
Rồi chạy test:      python test_api.py

Cần có 2 file ảnh mặt trong thư mục này (tải sẵn trong README):
  - data_test_face2.jpg  (mặt A - sẽ đăng ký)
  - data_test_face.jpg   (mặt B - dùng kiểm tra "chưa nhận diện")
"""
import base64
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import requests

try:  # console Windows hay dùng cp1258, in tiếng Việt bị lỗi
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASE = "http://127.0.0.1:5000"
HERE = Path(__file__).parent
FACE_A = (HERE / "data_test_face2.jpg").read_bytes()
FACE_B = (HERE / "data_test_face.jpg").read_bytes()

_passed, _failed = 0, 0


def check(name, cond, extra=""):
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  PASS  {name}")
    else:
        _failed += 1
        print(f"  FAIL  {name}  {extra}")


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


# ---------------------------------------------- MJPEG server giả lập điện thoại
class FakePhone(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        try:
            while True:
                self.wfile.write(
                    b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + FACE_A + b"\r\n"
                )
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


def main():
    print("\n--- 1. Cơ bản ---")
    r = requests.get(f"{BASE}/api/ping")
    check("GET /api/ping", r.ok and r.json().get("pong"))
    r = requests.get(BASE + "/")
    check("GET / (trang quản lý)", r.ok and "Nhận Diện Khuôn Mặt" in r.text)
    orig_url = requests.get(f"{BASE}/api/config").json().get("stream_url", "")
    # Số người có sẵn trong DB (DB có thể đã có dữ liệu thật, không được giả định trống)
    base_count = len(requests.get(f"{BASE}/api/students").json()["students"])

    print("\n--- 2. Đăng ký (JSON base64) ---")
    payload = {
        "full_name": "Nguyễn Văn An",
        "date_of_birth": "12/03/2005",
        "class_name": "10A1",
        "images": [b64(FACE_A), b64(FACE_A)],
    }
    r = requests.post(f"{BASE}/api/students", json=payload)
    d = r.json()
    check("Đăng ký thành công", r.ok and d.get("ok") and d.get("samples", 0) >= 1, str(d))
    sid = d.get("id")
    check("Thông tin đúng", d.get("student", {}).get("date_of_birth") == "2005-03-12", str(d))

    r = requests.post(f"{BASE}/api/students", json={
        "full_name": "", "date_of_birth": "x", "class_name": "10A1", "images": [b64(FACE_A)]})
    check("Từ chối thiếu họ tên", r.status_code == 400, str(r.status_code))

    print("\n--- 3. Nhận diện ---")
    r = requests.post(f"{BASE}/api/recognize", json={"images": [b64(FACE_A)]})
    res = r.json().get("results", [])
    check("Nhận ra người đã đăng ký", len(res) == 1 and res[0]["match"], json.dumps(res))
    if res:
        check("Đúng họ tên",
              res[0].get("full_name") == "Nguyễn Văn An", str(res[0]))
        check("Đúng ngày sinh",
              res[0].get("date_of_birth") == "2005-03-12", str(res[0]))
        check("Đúng lớp", res[0].get("class_name") == "10A1", str(res[0]))

    r = requests.post(f"{BASE}/api/recognize", json={"images": [b64(FACE_B)]})
    res_b = r.json().get("results", [])
    check("Người lạ KHÔNG bị nhận nhầm (match=false)",
          len(res_b) == 1 and res_b[0]["match"] is False, json.dumps(res_b))

    import cv2
    import numpy as np

    blank = cv2.imencode(".jpg", np.full((300, 300, 3), 128, np.uint8))[1].tobytes()
    r = requests.post(f"{BASE}/api/recognize", json={"images": [b64(blank)]})
    check("Ảnh không có mặt -> 0 kết quả", r.json().get("results") == [], r.text)

    print("\n--- 4. Sửa / thêm mẫu / xoá ---")
    r = requests.put(f"{BASE}/api/students/{sid}",
                     json={"class_name": "10A2"})
    check("Đổi lớp", r.ok and r.json()["student"]["class_name"] == "10A2", r.text)

    r = requests.post(f"{BASE}/api/students/{sid}/samples",
                      files=[("images", ("a.jpg", FACE_A, "image/jpeg"))])
    check("Thêm mẫu (multipart)", r.ok and r.json().get("added", 0) >= 1, r.text)

    r = requests.post(f"{BASE}/api/students",
                      data={"full_name": "Test MB", "date_of_birth": "2005-01-01",
                            "class_name": "9A"},
                      files=[("images", ("b.jpg", FACE_B, "image/jpeg"))])
    check("Đăng ký (multipart form)", r.ok and r.json().get("samples", 0) >= 1, r.text)
    sid_b = r.json().get("id")

    r = requests.get(f"{BASE}/api/students")
    check("Danh sách có 2 người (sau khi thêm 2)",
          len(r.json()["students"]) == base_count + 2, r.text)

    print("\n--- 5. Endpoint ESP32 (dùng sau này) ---")
    r = requests.post(f"{BASE}/api/esp32/frame", data=FACE_A,
                      headers={"Content-Type": "image/jpeg"})
    d = r.json()
    check("ESP32 POST JPEG -> JSON", r.ok and "results" in d, r.text)
    check("ESP32 thấy khuôn mặt đã đăng ký",
          len(d.get("results", [])) == 1 and d["results"][0]["match"], json.dumps(d))

    print("\n--- 6. Camera điện thoại (MJPEG giả lập) ---")
    srv = ThreadingHTTPServer(("127.0.0.1", 8090), FakePhone)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    r = requests.post(f"{BASE}/api/config", json={
        "stream_url": "http://127.0.0.1:8090/video", "restart": True})
    check("Lưu cấu hình camera", r.ok, r.text)
    check("STREAM_URL được ghi vào file .env",
          "127.0.0.1:8090" in (HERE / ".env").read_text(encoding="utf-8"),
          (HERE / ".env").read_text(encoding="utf-8"))

    orig_thr = requests.get(f"{BASE}/api/config").json()["match_threshold"]
    r = requests.post(f"{BASE}/api/config", json={"match_threshold": 65})
    d = requests.get(f"{BASE}/api/config").json()
    st = requests.get(f"{BASE}/api/status").json()
    check("Đổi MATCH_THRESHOLD qua API + .env",
          r.ok and d["match_threshold"] == 65 and st["threshold"] == 65
          and "MATCH_THRESHOLD=65" in (HERE / ".env").read_text(encoding="utf-8"),
          json.dumps(d))
    requests.post(f"{BASE}/api/config", json={"match_threshold": orig_thr})

    phone_ok = False
    for _ in range(30):
        time.sleep(0.5)
        st = requests.get(f"{BASE}/api/status").json()
        if st["camera"]["connected"] and st["frame_source"] == "phone":
            phone_ok = True
            break
    check("Kết nối được luồng MJPEG", phone_ok,
          json.dumps(st.get("camera", {})))
    check("Có kết quả nhận diện từ luồng",
          phone_ok and any(x["match"] for x in st.get("results", [])),
          json.dumps(st.get("results", [])))

    with requests.get(f"{BASE}/video_feed", stream=True, timeout=10) as v:
        chunk = next(v.iter_content(chunk_size=65536))
    check("GET /video_feed trả MJPEG có JPEG",
          b"\xff\xd8" in chunk and b"Content-Type: image/jpeg" in chunk,
          str(len(chunk)))

    print("\n--- 7. Nhật ký + dọn dẹp ---")
    time.sleep(1)
    r = requests.get(f"{BASE}/api/log")
    check("Nhật ký có dữ liệu", len(r.json()["log"]) >= 1, r.text)

    # Khôi phục cấu hình .env như ban đầu (không xoá URL camera thật)
    requests.post(f"{BASE}/api/config", json={"stream_url": orig_url})
    check("Khôi phục STREAM_URL trong .env",
          orig_url in (HERE / ".env").read_text(encoding="utf-8"),
          orig_url)
    srv.shutdown()

    r = requests.delete(f"{BASE}/api/students/{sid}")
    check("Xoá người A", r.ok, r.text)
    r = requests.delete(f"{BASE}/api/students/{sid_b}")
    check("Xoá người B", r.ok, r.text)
    r = requests.get(f"{BASE}/api/students")
    check("Danh sách trở lại như ban đầu",
          len(r.json()["students"]) == base_count, r.text)

    print(f"\n========== KẾT QUẢ: {_passed} PASS / {_failed} FAIL ==========")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
