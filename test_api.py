"""Kiểm thử tự động toàn bộ API của server nhận diện khuôn mặt.

Chạy server trước:  python server.py
Rồi chạy test:      python test_api.py

Cần có 2 file ảnh mặt trong thư mục này (xem README):
  - data_test_face2.jpg  (mặt A - sẽ đăng ký)
  - data_test_face.jpg   (mặt B - người lạ, dùng để kiểm tra chống nhận nhầm)

Phân cổng sau redesign:
  - PORTAL (PORT, mặc định 5000): API học sinh/lớp/giáo viên  -> cần admin
  - DEBUG  (CAMERA_PORT 5001)   : recognize, config, status, log, video
                                  -> cần tài khoản developer (trừ /api/ping
                                  và /api/esp32/frame của thiết bị)

Ghi chú: ảnh trong repo có kích thước rất khác nhau, nên test chuẩn hoá lại
(kho/cắt quanh mặt + phóng to) trước khi gửi — nếu không, mặt quá nhỏ sẽ bị
bộ lọc MIN_FACE_SIZE bỏ qua và test cho kết quả sai.
"""
import base64
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import cv2
import numpy as np
import requests
from dotenv import load_dotenv

try:  # console Windows hay dùng cp1258, in tiếng Việt bị lỗi
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = Path(__file__).parent
load_dotenv(HERE / ".env", encoding="utf-8-sig")
PORTAL = f"http://127.0.0.1:{int(os.getenv('PORT') or 5000)}"
CAM = f"http://127.0.0.1:{int(os.getenv('CAMERA_PORT') or 5001)}"
DEV_USER = os.getenv("DEV_USERNAME") or "developer"
DEV_PASS = os.getenv("DEV_PASSWORD") or "developer"
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


# --------------------------------------------------------------- chuẩn hoá ảnh
def _largest_face_box(img):
    """Tìm mặt lớn nhất bằng YuNet (model chính), Haar dự phòng."""
    model = HERE / "data" / "models" / "face_detection_yunet_2023mar.onnx"
    if model.exists():
        det = cv2.FaceDetectorYN_create(
            str(model), "", (img.shape[1], img.shape[0]), 0.4, 0.3, 5000
        )
        det.setInputSize((img.shape[1], img.shape[0]))
        _n, faces = det.detect(img)
        if faces is not None and len(faces):
            return max(faces, key=lambda f: f[2] * f[3])[:4]
    cascade = cv2.CascadeClassifier(
        cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    faces = cascade.detectMultiScale(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY), 1.1, 5)
    return max(faces, key=lambda f: f[2] * f[3]) if len(faces) else None


def _prep(raw: bytes, face_px=150, gain=1.0, box_ratio=3.2, angle=0.0) -> bytes:
    """Cắt ô vuông quanh mặt rồi phóng cho mặt đủ lớn (>= MIN_FACE_SIZE).

    Bỏ qua bước này thì ảnh mặt nhỏ bị bộ lọc MIN_FACE_SIZE loại mất -> test sai.
    `angle` xoay nhẹ ảnh để tạo mẫu KHÁC HẨN (cùng một góc chụp sẽ bị engine
    coi là trùng và bỏ qua, vì mặt đã được căn về 112x112).
    """
    img = cv2.imdecode(np.frombuffer(raw, np.uint8), cv2.IMREAD_COLOR)
    box = _largest_face_box(img)
    if box is not None:
        x, y, w, h = (int(v) for v in box)
        side = max(32, int(max(w, h) * box_ratio))
        H, W = img.shape[:2]
        x0 = int(max(0, min(W - side, x + w / 2 - side / 2)))
        y0 = int(max(0, min(H - side, y + h / 2 - side / 2)))
        img = img[y0:y0 + side, x0:x0 + side]
        k = face_px / float(max(w, h))              # phóng để mặt đúng face_px
        if abs(k - 1.0) > 0.05:
            img = cv2.resize(img, (0, 0), fx=k, fy=k, interpolation=cv2.INTER_CUBIC)
    if angle:
        H, W = img.shape[:2]
        m = cv2.getRotationMatrix2D((W / 2, H / 2), angle, 1.0)
        img = cv2.warpAffine(img, m, (W, H), borderMode=cv2.BORDER_REPLICATE)
    if gain != 1.0:
        img = np.clip(img.astype(np.float32) * gain, 0, 255).astype(np.uint8)
    return cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 92])[1].tobytes()


def _side_by_side(a: bytes, b: bytes) -> bytes:
    """Ghép 2 ảnh cạnh nhau -> khung hình có 2 người."""
    ia = cv2.imdecode(np.frombuffer(a, np.uint8), cv2.IMREAD_COLOR)
    ib = cv2.imdecode(np.frombuffer(b, np.uint8), cv2.IMREAD_COLOR)
    H = max(ia.shape[0], ib.shape[0])
    W = ia.shape[1] + ib.shape[1] + 40
    out = np.full((H, W, 3), 45, np.uint8)
    out[:ia.shape[0], :ia.shape[1]] = ia
    x = ia.shape[1] + 40
    out[:ib.shape[0], x:x + ib.shape[1]] = ib
    return cv2.imencode(".jpg", out, [cv2.IMWRITE_JPEG_QUALITY, 92])[1].tobytes()


# ---------------------------------------------- MJPEG server giả lập điện thoại
class FakePhone(BaseHTTPRequestHandler):
    """Trả ảnh đã chuẩn hoá, xen kẽ 1 khung tối để mô phỏng chớp sáng/mất dặt."""

    def __init__(self, *a, **kw):
        self._img = kw.pop("img", FACE_A)
        self._dark = kw.pop("dark", FACE_A)
        super().__init__(*a, **kw)

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        n = 0
        try:
            while True:
                n += 1
                # cứ 10 khung lại chặn 1 khung -> kiểm tra ô vuông có giữ được không
                img = self._img if n % 10 else self._dark
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + img + b"\r\n")
                time.sleep(0.08)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


def main():
    # ---------------- phiên đăng nhập: admin (cổng PORTAL) + developer (DEBUG)
    admin = requests.Session()
    admin.post(PORTAL + "/dang-nhap", data={"username": "admin",
                                            "password": os.getenv("ADMIN_PASSWORD") or "admin"},
               timeout=15, allow_redirects=False)
    dev = requests.Session()
    r = dev.post(PORTAL + "/dang-nhap",
                 data={"username": DEV_USER, "password": DEV_PASS},
                 timeout=15, allow_redirects=False)
    check("đăng nhập developer (mở khoá cổng debug)",
          r.status_code == 302, r.status_code)

    print("\n--- 1. Cơ bản ---")
    r = requests.get(f"{CAM}/api/ping")
    check("GET /api/ping (không cần đăng nhập)", r.ok and r.json().get("pong"))

    r = requests.get(f"{CAM}/", allow_redirects=False)
    check("cổng debug CHƯA đăng nhập -> chuyển về trang đăng nhập",
          r.status_code in (302, 303), r.status_code)
    r = dev.get(CAM + "/")
    portal_port = int(os.getenv("PORT") or 5000)
    check("developer mở được trang debug (preview + cấu hình)",
          r.ok and 'id="live"' in r.text and "/video_feed" in r.text
          and "topnav" in r.text, r.status_code)
    check("thanh nav trỏ sang cổng quản lý", f":{portal_port}" in r.text, portal_port)
    check("trang debug không còn bảng đăng ký học sinh",
          "Đăng ký học sinh mới" not in r.text)

    orig_url = dev.get(f"{CAM}/api/config").json().get("stream_url", "")
    # Số người có sẵn trong DB (DB có thể đã có dữ liệu thật, không được giả định trống)
    base_count = len(admin.get(f"{PORTAL}/api/students").json()["students"])

    st = dev.get(f"{CAM}/api/status").json()
    engine = st.get("engine", {})
    print(f"  engine: {engine.get('detector')} + {engine.get('recognizer')} "
          f"({engine.get('metric')}), ngưỡng {engine.get('threshold')}")
    check("/api/status có thông tin engine",
          bool(engine.get("detector")) and bool(engine.get("recognizer")), engine)
    check("admin KHÔNG gọi được API cổng debug",
          admin.get(f"{CAM}/api/status").status_code == 401)

    A = [_prep(FACE_A), _prep(FACE_A, gain=1.2), _prep(FACE_A, face_px=110),
         _prep(FACE_A, face_px=100, angle=14)]
    B = [_prep(FACE_B), _prep(FACE_B, gain=0.85), _prep(FACE_B, face_px=110)]

    print("\n--- 2. Đăng ký (JSON base64, cổng PORTAL) ---")
    payload = {
        "full_name": "Nguyễn Văn An",
        "date_of_birth": "12/03/2005",
        "class_name": "10A1",
        "images": [b64(x) for x in A[:3]],
    }
    r = admin.post(f"{PORTAL}/api/students", json=payload)
    d = r.json()
    check("Đăng ký thành công", r.ok and d.get("ok") and d.get("samples", 0) >= 1, str(d))
    sid = d.get("id")
    check("Thông tin đúng", d.get("student", {}).get("date_of_birth") == "2005-03-12", str(d))

    r = admin.post(f"{PORTAL}/api/students", json={
        "full_name": "", "date_of_birth": "x", "class_name": "10A1", "images": [b64(FACE_A)]})
    check("Từ chối thiếu họ tên", r.status_code == 400, str(r.status_code))
    r = admin.post(f"{PORTAL}/api/students", json={
        "full_name": "Mat Nho", "date_of_birth": "2008-01-01",
        "class_name": "10A1", "images": [b64(FACE_A)]})
    check("Ảnh gốc mặt nhỏ bị từ chối 422 KÈM số đo chẩn đoán",
          r.status_code == 422 and "px" in r.text and "gần" in r.text,
          f"{r.status_code} {r.text[:160]}")
    check("chưa đăng nhập thì không đăng ký được",
          requests.post(f"{PORTAL}/api/students", json=payload,
                        allow_redirects=False).status_code in (302, 401))

    print("\n--- 3. Nhận diện (cổng DEBUG, developer) ---")
    r = dev.post(f"{CAM}/api/recognize", json={"images": [b64(A[0])]})
    res = r.json().get("results", [])
    check("Nhận ra người đã đăng ký", len(res) == 1 and res[0]["match"], json.dumps(res))
    if res:
        check("Đúng họ tên",
              res[0].get("full_name") == "Nguyễn Văn An", str(res[0]))
        check("Đúng ngày sinh",
              res[0].get("date_of_birth") == "2005-03-12", str(res[0]))
        check("Đúng lớp",
              res[0].get("class_name") == "10A1", str(res[0]))

    r = dev.post(f"{CAM}/api/recognize", json={"images": [b64(B[0])]})
    res_b = r.json().get("results", [])
    check("Người lạ KHÔNG bị nhận nhầm (match=false)",
          len(res_b) == 1 and res_b[0]["match"] is False, json.dumps(res_b))

    blank = cv2.imencode(".jpg", np.full((300, 300, 3), 128, np.uint8))[1].tobytes()
    r = dev.post(f"{CAM}/api/recognize", json={"images": [b64(blank)]})
    check("Ảnh không có mặt -> 0 kết quả", r.json().get("results") == [], r.text)

    print("\n--- 4. Sửa / thêm mẫu / xoá (cổng PORTAL) ---")
    r = admin.put(f"{PORTAL}/api/students/{sid}",
                  json={"class_name": "10A2"})
    check("Đổi lớp", r.ok and r.json()["student"]["class_name"] == "10A2", r.text)

    r = admin.post(f"{PORTAL}/api/students/{sid}/samples",
                   files=[("images", ("a.jpg", A[3], "image/jpeg"))])
    check("Thêm mẫu (multipart)", r.ok and r.json().get("added", 0) >= 1, r.text)

    r = admin.post(f"{PORTAL}/api/students/{sid}/samples",
                   files=[("images", ("a.jpg", A[0], "image/jpeg"))])
    check("Ảnh trùng mẫu cũ -> không lưu nhưng KHÔNG báo lỗi",
          r.ok and r.json().get("added") == 0, r.text)

    r = admin.post(f"{PORTAL}/api/students",
                   data={"full_name": "Test MB", "date_of_birth": "2005-01-01",
                         "class_name": "9A"},
                   files=[("images", ("b.jpg", B[0], "image/jpeg"))])
    check("Đăng ký (multipart form)", r.ok and r.json().get("samples", 0) >= 1, r.text)
    sid_b = r.json().get("id")

    r = admin.get(f"{PORTAL}/api/students")
    check("Danh sách có 2 người (sau khi thêm 2)",
          len(r.json()["students"]) == base_count + 2, r.text)

    print("\n--- 4b. HAI NGƯỜI TRONG CÙNG MỘT KHUNG (chống nhận nhầm) ---")
    r = dev.post(f"{CAM}/api/recognize",
                 json={"images": [b64(_side_by_side(A[0], B[0]))]})
    res = r.json().get("results", [])
    got = sorted(x["full_name"] for x in res if x.get("match"))
    print("  ", [(x.get("full_name"), x.get("confidence")) for x in res])
    check("thấy đủ 2 mặt trong một khung", len(res) == 2, json.dumps(res))
    check("mỗi người một ô vuông, không trùng ID",
          len({x["student_id"] for x in res}) == 2, json.dumps(res))
    check("KHÔNG lẫn tên giữa 2 người",
          got == ["Nguyễn Văn An", "Test MB"], got)
    check("mỗi ô vuông đều vuông (w == h)",
          all(abs(x["square"][2] - x["square"][3]) <= 2 for x in res), json.dumps(res))

    print("\n--- 5. Endpoint ESP32 (mở cho thiết bị, không cần phiên) ---")
    r = requests.post(f"{CAM}/api/esp32/frame", data=A[0],
                      headers={"Content-Type": "image/jpeg"})
    d = r.json()
    check("ESP32 POST JPEG -> JSON", r.ok and "results" in d, r.text)
    check("ESP32 thấy khuôn mặt đã đăng ký",
          len(d.get("results", [])) == 1 and d["results"][0]["match"], json.dumps(d))
    check("ESP32 không bị nhận nhầm người khác",
          len(d.get("results", [])) == 1, json.dumps(d))

    # --- capture=1: điểm danh thật + phản hồi JSON cho firmware
    day0 = time.strftime("%Y-%m-%d")
    att0 = admin.get(f"{PORTAL}/api/students/{sid}/attendance").json()["attendance"]
    check("recognize thường (không capture) KHÔNG tự điểm danh",
          all(x["day"] != day0 for x in att0["detail"]), json.dumps(att0["detail"][:3]))
    r = requests.post(f"{CAM}/api/esp32/frame?capture=1", data=A[0],
                      headers={"Content-Type": "image/jpeg"})
    d = r.json()
    check("capture=1 trả JSON điểm danh (matched/already/session)",
          r.ok and d.get("matched") == 1 and d.get("full_name") == "Nguyễn Văn An"
          and d.get("session") in (1, 2), json.dumps({k: d.get(k) for k in
                                                      ('matched', 'already', 'full_name',
                                                       'session', 'session_name')}))
    check("lần đầu -> already=false", d.get("already") is False, str(d.get("already")))
    r = requests.post(f"{CAM}/api/esp32/frame?capture=1", data=A[0],
                      headers={"Content-Type": "image/jpeg"})
    d2 = r.json()
    check("bấm lại CÙNG buổi -> already=true (không tính thêm)",
          r.ok and d2.get("already") is True, json.dumps(d2))
    att1 = admin.get(f"{PORTAL}/api/students/{sid}/attendance").json()["attendance"]
    check("2 lần capture vẫn chỉ 1 dòng điểm danh hôm nay",
          len([x for x in att1["detail"] if x["day"] == day0]) == 1,
          json.dumps([x for x in att1["detail"] if x["day"] == day0]))

    print("\n--- 6. Camera điện thoại (MJPEG giả lập) ---")
    dark = cv2.imencode(".jpg", np.full(cv2.imdecode(np.frombuffer(A[0], np.uint8),
                                                     1).shape, 12, np.uint8))[1].tobytes()
    srv = ThreadingHTTPServer(("127.0.0.1", 8090),
                              lambda *a, **kw: FakePhone(*a, img=A[0], dark=dark, **kw))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    r = dev.post(f"{CAM}/api/config", json={
        "stream_url": "http://127.0.0.1:8090/video", "restart": True})
    check("Lưu cấu hình camera", r.ok, r.text)
    check("STREAM_URL được ghi vào file .env",
          "127.0.0.1:8090" in (HERE / ".env").read_text(encoding="utf-8"),
          (HERE / ".env").read_text(encoding="utf-8"))

    # Ngưỡng dùng đơn vị khác nhau tuỳ mô hình: cosin (0..2) hay LBPH (0..150)
    metric = engine.get("metric", "lbph")
    new_thr = 0.33 if metric == "cosine" else 65
    orig_thr = dev.get(f"{CAM}/api/config").json()["match_threshold"]
    r = dev.post(f"{CAM}/api/config", json={"match_threshold": new_thr})
    d = dev.get(f"{CAM}/api/config").json()
    st = dev.get(f"{CAM}/api/status").json()
    env_txt = (HERE / ".env").read_text(encoding="utf-8")
    check("Đổi MATCH_THRESHOLD qua API + .env",
          r.ok and d["match_threshold"] == new_thr and st["threshold"] == new_thr
          and f"MATCH_THRESHOLD={new_thr}" in env_txt.replace(" ", ""),
          json.dumps(d))
    dev.post(f"{CAM}/api/config", json={"match_threshold": orig_thr})

    phone_ok = False
    for _ in range(30):
        time.sleep(0.5)
        st = dev.get(f"{CAM}/api/status").json()
        if st["camera"]["connected"] and st["frame_source"] == "phone":
            phone_ok = True
            break
    check("Kết nối được luồng MJPEG", phone_ok,
          json.dumps(st.get("camera", {})))
    check("Có kết quả nhận diện từ luồng",
          phone_ok and any(x["match"] for x in st.get("results", [])),
          json.dumps(st.get("results", [])))

    with dev.get(f"{CAM}/video_feed", stream=True, timeout=10) as v:
        chunk = next(v.iter_content(chunk_size=65536))
    check("GET /video_feed trả MJPEG có JPEG",
          b"\xff\xd8" in chunk and b"Content-Type: image/jpeg" in chunk,
          str(len(chunk)))
    check("video_feed KHÔNG mở cho người lạ",
          requests.get(f"{CAM}/video_feed", stream=True,
                       allow_redirects=False).status_code in (302, 401))

    print("\n--- 6b. Theo dõi mặt trên luồng (ô vuông + ID ổn định) ---")
    track_ids, boxes, named, any_face = set(), [], 0, False
    for _ in range(40):
        time.sleep(0.25)
        st = dev.get(f"{CAM}/api/status").json()
        for x in st.get("results", []):
            any_face = True
            if x.get("track_id") is not None:
                track_ids.add(x["track_id"])
                boxes.append(tuple(x["square"]))
            if x.get("match"):
                named += 1
    print(f"  {len(track_ids)} ID track, {named} khung đã nhận tên, "
          f"{st.get('engine', {}).get('timing_ms')}")
    check("Luồng video có mặt để theo dõi", any_face)
    check("mọi kết quả có ID track", len(boxes) > 5, len(boxes))
    check("ID track ổn định (không nhân bản ẩu)", len(track_ids) <= 2, track_ids)
    check("ô vuông luôn vuông kể cả lúc mất dặt",
          all(abs(b[2] - b[3]) <= 2 for b in boxes),
          [b for b in boxes if abs(b[2] - b[3]) > 2][:3])
    check("ô vuông bám người (không đứng yên ở 0,0)",
          any(b[0] > 2 and b[1] > 2 for b in boxes), boxes[:3])
    check("tên hiện ra và giữ nguyên sau khi mất dặt", named > 5, named)

    print("\n--- 7. Nhật ký + dọn dẹp ---")
    time.sleep(1)
    r = dev.get(f"{CAM}/api/log")
    check("Nhật ký có dữ liệu", len(r.json()["log"]) >= 1, r.text)

    # Khôi phục cấu hình .env như ban đầu (không xoá URL camera thật)
    dev.post(f"{CAM}/api/config", json={"stream_url": orig_url})
    check("Khôi phục STREAM_URL trong .env",
          orig_url in (HERE / ".env").read_text(encoding="utf-8"),
          orig_url)
    srv.shutdown()

    # Xoá học sinh khi khuôn mặt vẫn đang hiện trên khung: kết quả cũ có thể
    # còn student_id đã xoá. Thread xử lý phải sống sót, điểm danh vẫn ghi được.
    r = admin.delete(f"{PORTAL}/api/students/{sid}")
    check("Xoá người A", r.ok, r.text)
    r = admin.delete(f"{PORTAL}/api/students/{sid_b}")
    check("Xoá người B", r.ok, r.text)
    r = admin.get(f"{PORTAL}/api/students")
    check("Danh sách trở lại như ban đầu",
          len(r.json()["students"]) == base_count, r.text)

    # Chờ camera thật lại và kiểm tra vòng lặp xử lý còn chạy
    alive = False
    for _ in range(40):
        time.sleep(0.5)
        st = dev.get(f"{CAM}/api/status").json()
        if st.get("frame_source") not in (None, "", "none"):
            alive = True
            break
    check("Thread xử lý vẫn sống sau khi xoá học sinh giữa lúc điểm danh", alive,
          json.dumps(st.get("frame_source")))
    r = dev.get(f"{CAM}/api/status")
    check("/api/status vẫn trả 200 sau dọn dẹp", r.ok)

    print("\n--- 8. Điểm danh tối đa 1 lần/buổi ---")
    # Gọi thẳng hàm record_results của server (tiến trình test import module
    # server, không ảnh hưởng server đang chạy): 3 lần cùng buổi -> 1 dòng
    # điểm danh, lần 2/3 trả already=true.
    sys.path.insert(0, str(HERE))
    import database as db  # noqa: E402
    import server as server_mod  # noqa: E402
    r = admin.post(f"{PORTAL}/api/students", json={
        "full_name": "Test Một Lần", "date_of_birth": "2008-06-06",
        "class_name": "ZZONCE", "images": [b64(A[0])]})
    sid_once = r.json().get("id")
    check("tạo học sinh thử cho luật 1 lần/buổi", r.ok and sid_once, r.text)
    res_once = {"match": True, "student_id": sid_once, "full_name": "Test Một Lần",
                "class_name": "ZZONCE", "confidence": 0.1}
    n_log0 = len(dev.get(f"{CAM}/api/log?limit=1000").json()["log"])
    s1 = server_mod.record_results([dict(res_once)])
    s2 = server_mod.record_results([dict(res_once)])  # đứng lâu trước camera
    s3 = server_mod.record_results([dict(res_once)])
    check("lần đầu chấm: already=false, có buổi",
          s1["matched"] == 1 and s1["already"] is False
          and s1["session"] in (1, 2), json.dumps(s1))
    check("các lần sau trong cùng buổi: already=true",
          s2["already"] is True and s3["already"] is True,
          json.dumps([s2, s3]))
    att = db.attendance_summary(sid_once)
    check("3 lần cùng buổi chỉ tạo 1 ngày điểm danh",
          att["days"] == 1, att["days"])
    check("buổi điểm đúng theo giờ hiện tại (1 buổi trong ngày)",
          att["detail"] and len(att["detail"][0]["sessions"]) == 1
          and att["detail"][0]["sessions"][0] in (1, 2),
          att["detail"])
    n_log1 = len(dev.get(f"{CAM}/api/log?limit=1000").json()["log"])
    diff = n_log1 - n_log0
    check("nhật ký ghi mỗi lần bấm nút (3 dòng) — trừ khi đã chạm giới hạn giữ",
          diff == 3 or n_log0 >= db.LOG_KEEP, diff)
    r = admin.delete(f"{PORTAL}/api/students/{sid_once}")
    check("xoá học sinh thử", r.ok, r.text)
    admin.delete(f"{PORTAL}/api/classes/ZZONCE")

    print(f"\n========== KẾT QUẢ: {_passed} PASS / {_failed} FAIL ==========")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
