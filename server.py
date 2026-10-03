"""Server nhận diện khuôn mặt.

- Nguồn khung hình: điện thoại làm camera IP (MJPEG) HOẶC ESP32 POST JPEG
  (endpoint /api/esp32/frame - dùng sau này khi có ESP32).
- Chuỗi xử lý: nhận khung hình -> detect + recognize -> vẽ -> phát trên
  /video_feed (MJPEG) và trả JSON qua các API.
- Thông tin người: họ và tên, ngày sinh, lớp (lưu SQLite).

Chạy:  python server.py   ->  http://localhost:5000
"""
import base64
import os
import threading
import time
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
from dotenv import load_dotenv
from flask import Flask, Response, jsonify, render_template, request
from werkzeug.exceptions import HTTPException

import database as db
from face_engine import FaceEngine, draw_text
from stream import PhoneCamera

BASE_DIR = Path(__file__).resolve().parent
ENV_PATH = BASE_DIR / ".env"
load_dotenv(ENV_PATH)

db.init_db()

app = Flask(__name__)
app.json.ensure_ascii = False
app.config["MAX_CONTENT_LENGTH"] = 20 * 1024 * 1024  # 20 MB


# ------------------------------------------------------------------- .env
def _env_number(name, default, cast=float):
    raw = (os.getenv(name) or "").strip()
    try:
        return cast(raw) if raw else cast(default)
    except (TypeError, ValueError):
        return cast(default)


# Toàn bộ cấu hình nằm trong file .env (xem .env.example)
SETTINGS = {
    "stream_url": (os.getenv("STREAM_URL") or "").strip(),
    "host": (os.getenv("HOST") or "").strip() or "0.0.0.0",
    "port": _env_number("PORT", 5000, int),
    "process_fps": _env_number("PROCESS_FPS", 12),
    "log_interval": _env_number("LOG_INTERVAL", 8.0),
    "match_threshold": _env_number("MATCH_THRESHOLD", 75),
    "jpeg_quality": _env_number("JPEG_QUALITY", 85, int),
}

engine = FaceEngine(threshold=SETTINGS["match_threshold"])


def save_env(**updates):
    """Ghi biến cấu hình vào file .env (giữ nguyên các biến/dòng khác)."""
    lines = (
        ENV_PATH.read_text(encoding="utf-8").splitlines()
        if ENV_PATH.exists()
        else ["# Cau hinh server nhan dien khuon mat"]
    )
    found = {k: False for k in updates}
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key = stripped.split("=", 1)[0].strip()
        if key in updates:
            lines[i] = f"{key}={updates[key]}"
            found[key] = True
    for key, value in updates.items():
        if not found[key]:
            lines.append(f"{key}={value}")
    ENV_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


@app.errorhandler(Exception)
def _handle_error(exc):
    """Lỗi không mong muốn ở /api/* trả JSON (thay vì trang HTML 500)."""
    if isinstance(exc, HTTPException):
        if request.path.startswith("/api/"):
            return jsonify({"ok": False, "error": exc.description or str(exc)}), exc.code
        return exc
    app.logger.exception("Lỗi tại %s %s", request.method, request.path)
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": f"Lỗi server: {exc}"}), 500
    return "Lỗi server", 500


# ------------------------------------------------------------------ frame hub
hub_lock = threading.Lock()
hub = {"frame": None, "ts": 0.0, "source": "none"}
out_lock = threading.Lock()
output = {"jpeg": None, "ts": 0.0, "results": [], "source": "none", "age": 0.0}
_last_log: dict = {}


def set_frame(frame, source: str):
    with hub_lock:
        hub["frame"] = frame
        hub["ts"] = time.time()
        hub["source"] = source


def get_hub():
    with hub_lock:
        return hub["frame"], hub["ts"], hub["source"]


def placeholder_frame(text="Đang chờ tín hiệu camera..."):
    img = np.zeros((480, 640, 3), dtype=np.uint8)
    img[:] = (35, 35, 40)
    w, _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.8, 2)[0]
    draw_text(img, text, ((640 - w) // 2, 225), (200, 200, 200), bg=None)
    ok, buf = cv2.imencode(".jpg", img)
    return buf.tobytes() if ok else b""


PLACEHOLDER = placeholder_frame()


def _log_results(results):
    now = time.time()
    for r in results:
        if not r.get("match"):
            continue
        sid = r["student_id"]
        if now - _last_log.get(sid, 0) < SETTINGS["log_interval"]:
            continue
        _last_log[sid] = now
        db.add_log(sid, r["full_name"], r["class_name"], r["confidence"])


def process_loop():
    while True:
        frame, ts, source = get_hub()
        if frame is None or time.time() - ts > 5.0:
            time.sleep(0.2)
            continue
        try:
            results, annotated = engine.process(frame.copy())
        except Exception as exc:  # noqa: BLE001
            print(f"[process] {exc}")
            time.sleep(0.5)
            continue
        ok, buf = cv2.imencode(
            ".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, SETTINGS["jpeg_quality"]]
        )
        if ok:
            with out_lock:
                output.update(
                    jpeg=buf.tobytes(),
                    ts=time.time(),
                    results=results,
                    source=source,
                    age=round(time.time() - ts, 2),
                )
        _log_results(results)
        time.sleep(1.0 / max(1.0, SETTINGS["process_fps"]))


def on_phone_frame(frame):
    set_frame(frame, "phone")


camera = PhoneCamera(on_frame=on_phone_frame)


# --------------------------------------------------------------------- helpers
def decode_image(payload):
    """Nhận bytes / base64 / data-URL -> ảnh BGR (None nếu lỗi)."""
    if payload is None:
        return None
    if isinstance(payload, str):
        s = payload.strip()
        if s.lower().startswith("data:") and "," in s:
            s = s.split(",", 1)[1]
        try:
            data = base64.b64decode(s)
        except Exception:  # noqa: BLE001
            return None
    else:
        data = bytes(payload)
    if not data:
        return None
    return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)


def collect_images(limit: int = 8):
    """Lấy danh sách ảnh từ multipart (files: images/image) hoặc JSON (images/base64)."""
    images = []
    if request.files:
        files = request.files.getlist("images") or request.files.getlist("image")
        for f in files:
            if not f:
                continue
            img = decode_image(f.read())
            if img is not None:
                images.append(img)
            if len(images) >= limit:
                break
        return images, (request.form.to_dict() if request.form else {})
    payload = request.get_json(silent=True) or {}
    items = payload.get("images") or ([payload.get("image")] if payload.get("image") else [])
    if isinstance(items, str):
        items = [items]
    for it in items[:limit]:
        img = decode_image(it)
        if img is not None:
            images.append(img)
    return images, payload


def normalize_date(value):
    v = (value or "").strip()
    for fmt in ("%Y-%m-%d", "%d/%m/%Y", "%d-%m-%Y", "%d.%m.%Y"):
        try:
            return datetime.strptime(v, fmt).strftime("%Y-%m-%d")
        except ValueError:
            continue
    return None


def err(message, code=400):
    return jsonify({"ok": False, "error": message}), code


# ----------------------------------------------------------------------- pages
@app.get("/")
def index():
    return render_template("index.html")


@app.get("/video_feed")
def video_feed():
    def gen():
        last = None
        while True:
            with out_lock:
                jpeg = output["jpeg"]
            if jpeg is None:
                jpeg = PLACEHOLDER
            if jpeg is not last:
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"
                )
                last = jpeg
            time.sleep(0.03)

    return Response(
        gen(), mimetype="multipart/x-mixed-replace; boundary=frame"
    )


@app.get("/api/ping")
def ping():
    return jsonify({"ok": True, "pong": True})


@app.get("/api/frame.jpg")
def api_raw_frame():
    """Khung hình NGUYÊN GỐC từ camera (chưa khoanh vùng) — dùng khi đăng ký."""
    frame, ts, _source = get_hub()
    if frame is None or time.time() - ts > 5.0:
        return err("Chưa có khung hình từ camera", 503)
    ok, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
    if not ok:
        return err("Không mã hoá được khung hình", 500)
    resp = Response(buf.tobytes(), mimetype="image/jpeg")
    resp.headers["Cache-Control"] = "no-store"
    return resp


# ------------------------------------------------------------------ cấu hình (.env)
CONFIG_KEYS = (
    "stream_url",
    "process_fps",
    "log_interval",
    "match_threshold",
    "jpeg_quality",
)


@app.get("/api/config")
def get_config():
    return jsonify(
        {"ok": True, "env_file": str(ENV_PATH), **{k: SETTINGS[k] for k in CONFIG_KEYS}}
    )


@app.post("/api/config")
def set_config():
    """Đổi cấu hình -> áp dụng ngay vào bộ nhớ + ghi lại file .env."""
    data = request.get_json(silent=True) or {}
    updated = {}

    if "stream_url" in data:
        url = (data.get("stream_url") or "").strip()
        if url != SETTINGS["stream_url"]:
            SETTINGS["stream_url"] = url
            updated["STREAM_URL"] = url
        camera.set_url(url, restart=bool(data.get("restart")))

    casts = {
        "process_fps": float,
        "log_interval": float,
        "match_threshold": float,
        "jpeg_quality": int,
    }
    for key, cast in casts.items():
        if key not in data or data.get(key) in (None, ""):
            continue
        try:
            value = cast(data[key])
        except (TypeError, ValueError):
            return err(f"Giá trị {key} không hợp lệ")
        if value != SETTINGS[key]:
            SETTINGS[key] = value
            updated[key.upper()] = value

    if "MATCH_THRESHOLD" in updated:
        engine.threshold = SETTINGS["match_threshold"]
    if updated:
        save_env(**updated)

    return jsonify(
        {"ok": True, "updated": sorted(updated), **{k: SETTINGS[k] for k in CONFIG_KEYS}}
    )


# ---------------------------------------------------------------------- status
@app.get("/api/status")
def status():
    st = camera.status()
    with out_lock:
        results = [dict(r) for r in output["results"]]
        src = output["source"]
        age = output["age"]
        fresh = time.time() - output["ts"] < 3.0 if output["ts"] else False
    return jsonify(
        {
            "ok": True,
            "camera": st,
            "frame_source": src if fresh else "none",
            "frame_age": age if fresh else None,
            "results": results,
            "students": db.count_students(),
            "trained": engine._trained,
            "threshold": engine.threshold,
        }
    )


# --------------------------------------------------------------------- students
@app.get("/api/students")
def api_list_students():
    return jsonify({"ok": True, "students": db.list_students()})


@app.post("/api/students")
def api_create_student():
    images, data = collect_images()
    full_name = (data.get("full_name") or "").strip()
    date_of_birth = normalize_date(data.get("date_of_birth"))
    class_name = (data.get("class_name") or "").strip()

    if not full_name:
        return err("Thiếu họ và tên")
    if not date_of_birth:
        return err("Ngày sinh không hợp lệ (dd/mm/yyyy hoặc yyyy-mm-dd)")
    if not class_name:
        return err("Thiếu lớp")
    if not images:
        return err("Chưa có ảnh khuôn mặt")

    student_id = db.create_student(full_name, date_of_birth, class_name)
    saved = engine.add_samples(student_id, images)
    if saved == 0:
        db.delete_student(student_id)
        return err(
            f"Không tìm thấy khuôn mặt trong {len(images)} ảnh đã gửi — "
            "hãy đưa mặt rõ ràng, gần camera rồi chụp lại",
            422,
        )
    return jsonify({"ok": True, "id": student_id, "samples": saved, "student": db.get_student(student_id)})


@app.get("/api/students/<int:sid>")
def api_get_student(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    return jsonify({"ok": True, "student": st})


@app.put("/api/students/<int:sid>")
def api_update_student(sid):
    data = request.get_json(silent=True) or {}
    full_name = (data.get("full_name") or "").strip() or None
    class_name = (data.get("class_name") or "").strip() or None
    date_of_birth = normalize_date(data.get("date_of_birth")) if data.get("date_of_birth") else None
    if data.get("date_of_birth") and not date_of_birth:
        return err("Ngày sinh không hợp lệ")
    if not db.update_student(sid, full_name, date_of_birth, class_name):
        return err("Không tồn tại", 404)
    return jsonify({"ok": True, "student": db.get_student(sid)})


@app.delete("/api/students/<int:sid>")
def api_delete_student(sid):
    if not db.delete_student(sid):
        return err("Không tồn tại", 404)
    engine.retrain()
    return jsonify({"ok": True})


@app.post("/api/students/<int:sid>/samples")
def api_add_samples(sid):
    if not db.get_student(sid):
        return err("Không tồn tại", 404)
    images, _ = collect_images()
    if not images:
        return err("Chưa có ảnh")
    saved = engine.add_samples(sid, images)
    if saved == 0:
        return err(
            f"Không tìm thấy khuôn mặt trong {len(images)} ảnh — chụp lại khi mặt rõ hơn",
            422,
        )
    return jsonify({"ok": True, "added": saved, "student": db.get_student(sid)})


# -------------------------------------------------------------------- recognize
@app.post("/api/recognize")
def api_recognize():
    images, _ = collect_images(limit=1)
    if not images:
        return err("Thiếu ảnh")
    results, _frame = engine.process(images[0], draw=False)
    return jsonify({"ok": True, "results": results})


# ------------------------------------------------------------------ esp32 frame
@app.post("/api/esp32/frame")
def api_esp32_frame():
    """ESP32 (sau này) POST JPEG trực tiếp: Content-Type: image/jpeg, body = bytes."""
    ctype = (request.content_type or "").lower()
    if "image/jpeg" in ctype or "application/octet-stream" in ctype:
        img = decode_image(request.get_data())
    else:
        images, _ = collect_images(limit=1)
        img = images[0] if images else None
    if img is None:
        return err("Không đọc được ảnh JPEG")
    set_frame(img, "esp32")
    results, annotated = engine.process(img, draw=False)
    _log_results(results)
    return jsonify({"ok": True, "results": results, "count": len(results)})


# -------------------------------------------------------------------------- log
@app.get("/api/log")
def api_log():
    return jsonify({"ok": True, "log": db.recent_log(request.args.get("limit", 30, type=int))})


# ----------------------------------------------------------------------- start
def main():
    threading.Thread(target=process_loop, daemon=True, name="process").start()
    if SETTINGS["stream_url"]:
        camera.set_url(SETTINGS["stream_url"])
    print("=" * 60)
    print("  Server nhan dien khuon mat dang chay")
    print(f"  Cau hinh : {ENV_PATH}")
    print(f"  Camera   : {SETTINGS['stream_url'] or '(chua dat STREAM_URL)'}")
    print(f"  Trang quan ly: http://localhost:{SETTINGS['port']}")
    print(f"  API ESP32 (sau nay): POST http://<ip-may>:{SETTINGS['port']}/api/esp32/frame")
    print("=" * 60)
    app.run(
        host=SETTINGS["host"], port=SETTINGS["port"], threaded=True, use_reloader=False
    )


if __name__ == "__main__":
    main()
