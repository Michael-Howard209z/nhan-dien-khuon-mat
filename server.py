"""Server nhận diện khuôn mặt + điểm danh theo buổi.

ĐĂNG NHẬP DUY NHẤT (một form user + password, cổng PORT):
  * admin     -> trang quản trị   : thêm/sửa lớp, lịch học theo buổi, học sinh,
                                    tài khoản giáo viên, giám sát hệ thống.
  * teacher   -> trang giáo viên  : đăng ký học sinh bằng UPLOAD ảnh, danh sách
                                    hôm nay (vắng/buổi), báo cáo ngày/tuần/tháng.
  * developer -> trang debug       : preview camera, cài đặt ESP32-CAM, cấu hình
                                    engine, nhật ký — CHỈ vai trò này xem được
                                    camera preview (cổng CAMERA_PORT).

ĐIỂM DANH THEO BUỔI:
  * Học sinh điểm danh bằng cách BẤM NÚT trên ESP32-CAM: cam chụp 1 ảnh (kèm
    flash) rồi POST tới /api/esp32/frame?capture=1 — server nhận diện, ghi điểm
    danh 1 lần cho buổi hiện tại (sáng/chiều theo lịch của lớp). Bấm bao nhiêu
    lần trong buổi cũng chỉ có 1 điểm danh buổi đó; buổi chiều tính lại.
  * Xem camera (preview) CHỈ là công cụ debug của developer — không tự điểm danh.

Hai cổng (cùng tiến trình, chung engine + DB):
  * PORT        (mặc định 5000) — đăng nhập + trang admin/giáo viên.
  * CAMERA_PORT (mặc định 5001) — trang debug + API của ESP32 (POST khung hình).

Chạy:  python server.py   ->  http://localhost:5000 (đăng nhập)
                              http://localhost:5001 (debug, cần tài khoản developer)
"""
import base64
import csv
import io
import os
import secrets
import shutil
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path

import cv2
import numpy as np
import requests
from dotenv import load_dotenv
from flask import Flask, Response, jsonify, redirect, render_template, request, session, url_for
from urllib.parse import urlsplit
from werkzeug.exceptions import HTTPException
from werkzeug.serving import make_server

import auth
import database as db
from face_engine import DEFAULTS, FaceEngine, draw_text
from stream import PhoneCamera

BASE_DIR = Path(__file__).resolve().parent
ENV_PATH = BASE_DIR / ".env"
# utf-8-sig: nuốt BOM nếu .env được lưu bằng Notepad/PowerShell. Nếu không, khoá
# đầu tiên sẽ thành "﻿STREAM_URL" và mọi cấu hình dòng đầu bị bỏ qua.
load_dotenv(ENV_PATH, encoding="utf-8-sig")

db.init_db()

STARTED_AT = time.time()

# Hai ứng dụng Flask dùng chung tiến trình (chung engine, camera, cơ sở dữ liệu).
# Cookie phiên đăng nhập ký bằng cùng SECRET_KEY và cookie không phân biệt cổng,
# nên đăng nhập ở cổng portal thì cổng debug cũng "nhớ" vai trò.
cam_app = Flask("camera", root_path=str(BASE_DIR))    # CAMERA_PORT: debug developer
portal = Flask("portal", root_path=str(BASE_DIR))     # PORT: đăng nhập + admin/GV
for _a in (cam_app, portal):
    _a.json.ensure_ascii = False
    _a.config["MAX_CONTENT_LENGTH"] = 20 * 1024 * 1024  # 20 MB


# ------------------------------------------------------------------- .env
def save_env(**updates):
    """Ghi biến cấu hình vào file .env (giữ nguyên các biến/dòng khác)."""
    # Đọc bằng utf-8-sig để bỏ BOM, ghi bằng utf-8 (không BOM) cho sạch.
    lines = (
        ENV_PATH.read_text(encoding="utf-8-sig").splitlines()
        if ENV_PATH.exists()
        else ["# Cau hinh server nhan dien khuon mat"]
    )

    def fmt(key, value):
        # bool -> 1/0, số nguyên -> 3 (không phải 3.0) cho .env dễ đọc
        if isinstance(value, bool):
            return 1 if value else 0
        if isinstance(value, float) and value.is_integer():
            return int(value)
        return value

    found = {k: False for k in updates}
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key = stripped.split("=", 1)[0].strip()
        if key in updates:
            lines[i] = f"{key} = {fmt(key, updates[key])}"
            found[key] = True
    for key, value in updates.items():
        if not found[key]:
            lines.append(f"{key} = {fmt(key, value)}")
    ENV_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


# Khoá phiên đăng nhập: giữ lại giữa các lần khởi động lại server
_secret = (os.getenv("SECRET_KEY") or "").strip()
if not _secret:
    _secret = secrets.token_hex(32)
    save_env(SECRET_KEY=_secret)
    print("[config] Da sinh SECRET_KEY moi va ghi vao .env")
# Cùng một khoá cho cả hai cổng -> đăng nhập ở cổng nào thì cổng kia cũng đọc
# được cookie (cookie không phân biệt cổng).
for _a in (cam_app, portal):
    _a.secret_key = _secret
    _a.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Lax")


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
    "camera_port": _env_number("CAMERA_PORT", 5001, int),
    "process_fps": _env_number("PROCESS_FPS", 12),
    "log_interval": _env_number("LOG_INTERVAL", 8.0),
    "match_threshold": _env_number("MATCH_THRESHOLD", DEFAULTS["match_threshold"]),
    "jpeg_quality": _env_number("JPEG_QUALITY", 85, int),
    "detect_score": _env_number("DETECT_SCORE", DEFAULTS["detect_score"]),
    "min_face_size": _env_number("MIN_FACE_SIZE", DEFAULTS["min_face_size"], int),
    "match_margin": _env_number("MATCH_MARGIN", DEFAULTS["match_margin"]),
    "track_confirm": _env_number("TRACK_CONFIRM", DEFAULTS["track_confirm"], int),
    "track_max_age": _env_number("TRACK_MAX_AGE", DEFAULTS["track_max_age"], int),
    "track_smooth": _env_number("TRACK_SMOOTH", DEFAULTS["track_smooth"]),
    "track_backend": (os.getenv("TRACK_BACKEND")
                      or DEFAULTS["track_backend"]).strip().lower(),
    "track_enabled": str(os.getenv("TRACK_ENABLED", "1")).strip().lower()
    not in ("0", "false", "no", "off"),
}
# Hai cổng phải khác nhau thì mới chạy được cả hai ứng dụng trên một máy.
if SETTINGS["camera_port"] == SETTINGS["port"]:
    SETTINGS["camera_port"] = SETTINGS["port"] + 1
    print(f"[config] PORT trung CAMERA_PORT -> chuyen trang debug sang "
          f"co {SETTINGS['camera_port']}")

# Engine khởi tạo trước để biết đơn vị ngưỡng (cosin hay LBPH) trước khi chuẩn hoá
engine = FaceEngine(threshold=SETTINGS["match_threshold"])

# Ngưỡng LBPH của bản cũ (thang ~0..150) không dùng được cho khoảng cách cosin
# (thang ~0..2). Nếu .env còn giá trị kiểu cũ, quy đổi sang ngưỡng mặc định.
threshold_migrated = False
if engine.matcher.metric == "cosine" and SETTINGS["match_threshold"] > 1.5:
    print(
        f"[config] MATCH_THRESHOLD={SETTINGS['match_threshold']:g} la nguong LBPH "
        f"(ban cu) -> chuyen sang {DEFAULTS['match_threshold']:g} cho khoang cach cosin"
    )
    SETTINGS["match_threshold"] = DEFAULTS["match_threshold"]
    threshold_migrated = True

engine.configure(
    detect_score=SETTINGS["detect_score"],
    min_face_size=SETTINGS["min_face_size"],
    match_margin=SETTINGS["match_margin"],
    track_confirm=SETTINGS["track_confirm"],
    track_max_age=SETTINGS["track_max_age"],
    track_enabled=SETTINGS["track_enabled"],
)
if threshold_migrated:
    engine.threshold = SETTINGS["match_threshold"]


# ------------------------------------------------------ tài khoản hệ thống seed
ADMIN_PASSWORD = (os.getenv("ADMIN_PASSWORD") or "admin").strip()
DEV_PASSWORD = (os.getenv("DEV_PASSWORD") or "developer").strip()
DEV_USERNAME = (os.getenv("DEV_USERNAME") or "developer").strip().lower()


def _sync_system_account(username: str, password: str, full_name: str, role: str):
    """Đảm bảo tài khoản admin/developer tồn tại; .env là nguồn chân lý mật khẩu."""
    user = db.get_user(username)
    if not user:
        db.ensure_user(username, auth.hash_password(password), full_name, role)
        print(f"[auth] Da tao tai khoan {role}: '{username}'")
    elif not auth.verify_password(password, user["password_hash"]):
        db.update_user_password(username, auth.hash_password(password))
        print(f"[auth] Da cap nhat mat khau {role} '{username}' theo .env")


_sync_system_account("admin", ADMIN_PASSWORD, "Quản trị viên", "admin")
if DEV_USERNAME not in ("admin",):
    _sync_system_account(DEV_USERNAME, DEV_PASSWORD, "Nhà phát triển", "developer")
if ADMIN_PASSWORD in ("admin", "dev", "developer") or DEV_PASSWORD in ("admin", "dev"):
    print("[auth] CANH BAO: van dung mat khau mac dinh cho admin/developer "
          "-> doi trong .env (ADMIN_PASSWORD, DEV_PASSWORD)")


@cam_app.errorhandler(Exception)
@portal.errorhandler(Exception)
def _handle_error(exc):
    """Lỗi không mong muốn ở /api/* trả JSON (thay vì trang HTML 500)."""
    if isinstance(exc, HTTPException):
        if request.path.startswith("/api/"):
            return jsonify({"ok": False, "error": exc.description or str(exc)}), exc.code
        return exc
    request.app.logger.exception("Lỗi tại %s %s", request.method, request.path)
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": f"Lỗi server: {exc}"}), 500
    return "Lỗi server", 500


# ------------------------------------------------- đường dẫn qua lại giữa 2 cổng
@cam_app.context_processor
@portal.context_processor
def _inject_links():
    """Mọi trang đều biết địa chỉ cổng portal và cổng debug để gắn liên kết.

    Giữ nguyên tên máy/địa chỉ IP mà người dùng đang truy cập, chỉ đổi cổng — nhờ
    vậy từ 192.168.1.9 bấm sang tab khác vẫn chạy trong mạng nội bộ.
    """
    host = (request.host or "localhost").split(":")[0] or "localhost"
    prefix = f"{request.scheme}://{host}"
    return {
        "portal_url": f"{prefix}:{SETTINGS['port']}",
        "camera_url": f"{prefix}:{SETTINGS['camera_port']}",
        "portal_path": _home_path(),
        "role_label": auth.ROLE_LABELS.get(auth.current_role(), ""),
    }


def _home_path() -> str:
    """Đường dẫn portal hợp lệ với vai trò hiện tại (dùng cho link 'Quản lý')."""
    role = auth.current_role()
    if role in ("admin", "teacher"):
        return "/quan-ly"
    return "/"


# ------------------------------------------------------------------ frame hub
hub_lock = threading.Lock()
hub = {"frame": None, "ts": 0.0, "source": "none"}
out_lock = threading.Lock()
output = {"jpeg": None, "ts": 0.0, "results": [], "source": "none", "age": 0.0}

# Thống kê ESP32 PUSH (POST /api/esp32/frame). Không cần lock riêng vì chỉ đọc/ghi
# số + thời gian (GIL đảm bảo nguyên tử cho gán đơn).
esp32_state = {"last_ts": 0.0, "count": 0, "last_count": 0}


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


def reset_identity_state():
    """Xoá track khi đổi người trong khung hoặc đổi danh sách đăng ký.

    Nếu không, tên đã chốt của track cũ có thể tồn đọng vài khung sau khi ảnh
    đã đổi hoàn toàn -> hiển thị nhầm tên của người vừa rời khung.
    """
    engine.reset_tracker()


def record_results(results):
    """Ghi điểm danh cho các kết quả khớp. Chỉ dùng khi CHẤM ĐIỂM DANH (capture).

    Trả về dict kết quả đầu tiên để phản hồi cho ESP32:
      {'matched', 'already', 'full_name', 'class_name', 'session', 'session_name'}
    """
    summary = {"matched": 0, "already": False, "full_name": "", "class_name": "",
               "session": 0, "session_name": ""}
    for r in results:
        if not r.get("match"):
            continue
        sid = r.get("student_id")
        name = (r.get("full_name") or "").strip()
        # Chặn ghi rác: thiếu id hoặc tên rỗng sẽ vi phạm NOT NULL của SQLite.
        if not sid or not name:
            continue
        try:
            info = db.add_log(sid, name, r.get("class_name"), r.get("confidence"))
        except Exception as exc:  # noqa: BLE001 - lỗi DB không được giết request
            print(f"[log] khong ghi diem danh cho id={sid}: {exc}")
            continue
        if not summary["matched"]:
            summary.update(
                matched=1,
                already=not info.get("first", False),
                full_name=name,
                class_name=r.get("class_name") or "",
                session=int(info.get("session") or 0),
                session_name=db.SESSION_NAMES.get(int(info.get("session") or 0), ""),
            )
    return summary


def process_loop():
    """Vòng xử lý nhận diện cho PREVIEW (debug). KHÔNG điểm danh ở đây —
    điểm danh chỉ xảy ra khi capture (bấm nút ESP32) để xem camera không tự chấm."""
    while True:
        try:
            t0 = time.time()
            frame, ts, source = get_hub()
            if frame is None or time.time() - ts > 5.0:
                time.sleep(0.2)
                continue
            # track=True: ô vuông mượt, giữ ID qua các khung mất dặt -> nhãn ổn định
            results, annotated = engine.process(frame.copy(), draw=True, track=True)
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
            # Ngủ bù đúng nhịp PROCESS_FPS (trừ thời gian đã xử lý) để không cộng
            # dồn trễ: trước đây ngủ cố định 1/FPS sau khi đã tốn 100-150ms
            # inference -> nhịp thực chỉ ~6fps mà hub thì tươi -> age tăng dần.
            elapsed = time.time() - t0
            wait = 1.0 / max(1.0, SETTINGS["process_fps"]) - elapsed
            if wait > 0:
                time.sleep(wait)
        except Exception as exc:  # noqa: BLE001
            # Không bao giờ để thread chết: nếu chết, nhận diện sẽ đứng im lặng
            # (camera vẫn "kết nối" nhưng frame_source=none).
            print(f"[process] {exc}")
            time.sleep(0.5)


def on_phone_frame(frame):
    """Nhận khung từ luồng PULL (điện thoại hoặc ESP32 :81/stream).

    Ưu tiên nguồn ESP32 PUSH: nếu ESP32 vừa POST < 5s thì bỏ khung phone để
    không giẫm lên khung ESP32 (2 nguồn chạy cùng lúc sẽ tranh nhau trong hub).
    Nguồn PULL trỏ tới ESP32 (:81/stream) thì vẫn ghi nhãn 'esp32' cho đúng.
    """
    url = (SETTINGS.get("stream_url") or "").lower()
    # Heuristic đơn giản mà đủ đúng: STREAM_URL có ':81/stream' hầu như luôn là
    # ESP32-CAM (điện thoại dùng :8080/video hoặc :4747/video).
    is_esp32_pull = ":81/stream" in url or ":81/capture" in url
    source = "esp32" if is_esp32_pull else "phone"
    with hub_lock:
        # ESP32 PUSH đang tươi (< 5s) mà nguồn PULL lại là phone -> nhường.
        if source == "phone" and hub["source"] == "esp32":
            if time.time() - esp32_state.get("last_ts", 0.0) < 5.0:
                return
        hub["frame"] = frame
        hub["ts"] = time.time()
        hub["source"] = source


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
    """Lấy danh sách ảnh từ multipart (files) hoặc JSON (images/base64).

    Chấp nhận mọi kiểu gửi phổ biến:
      - multipart/form-data kèm file -> files[images|image|file|frame|upload|capture]
        (ESP32-CAM gửi field "file", web gửi "images")
      - application/json            -> {"images": [b64, ...]}
      - form-urlencoded             -> request.form
    """
    FILE_FIELDS = ("images", "image", "file", "frame", "upload", "capture")
    images = []
    payload = {}
    if request.files:
        for key in FILE_FIELDS:
            for f in request.files.getlist(key):
                if not f:
                    continue
                try:
                    img = decode_image(f.read())
                except Exception:  # noqa: BLE001
                    continue
                if img is not None:
                    images.append(img)
                if len(images) >= limit:
                    break
            if len(images) >= limit:
                break
        payload = request.form.to_dict() if request.form else {}
    if not images:
        # không có file -> thử JSON, rồi tới form-urlencoded
        if request.is_json:
            payload = request.get_json(silent=True) or {}
        elif request.form:
            payload = request.form.to_dict()
        items = []
        for key in ("images", "image", "file", "frame"):
            v = payload.get(key)
            if v:
                items = v
                break
        if request.form:
            for key in FILE_FIELDS:
                lst = request.form.getlist(key)
                if lst:
                    items = lst
                    break
    else:
        items = []

    if isinstance(items, str):
        items = [items]
    for it in items[:limit]:
        img = decode_image(it)
        if img is not None:
            images.append(img)
    return images, payload


def extract_jpegs(raw: bytes):
    """Quét body MJPEG thô -> list bytes JPEG (tìm FFD8..FFD9).

    Dùng khi ESP32 POST cả cụm MJPEG (multipart/x-mixed-replace) thay vì 1 ảnh.
    Không phụ thuộc boundary nên chạy với mọi firmware.
    """
    if not raw:
        return []
    out, i = [], 0
    while True:
        s = raw.find(b"\xff\xd8", i)
        if s < 0:
            break
        e = raw.find(b"\xff\xd9", s + 2)
        if e < 0:
            break
        out.append(raw[s:e + 2])
        i = e + 2
        if len(out) >= 30:  # chặn body quá lớn (tránh ngốn RAM)
            break
    return out


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


def _valid_date(value, fallback=None):
    d = normalize_date(value or "")
    return d or (fallback or db.today_str())


# ================================================================= CỔNG DEBUG
# Chỉ TÀI KHOẢN DEVELOPER mới xem/điều khiển được trang camera (preview là công
# cụ debug, không phải luồng điểm danh). Ngoại lệ: /api/ping (sức khoẻ) và
# /api/esp32/frame (ESP32 gửi ảnh — thiết bị không có phiên đăng nhập).
_OPEN_CAM_PATHS = ("/api/ping", "/api/esp32/frame")


@cam_app.before_request
def _cam_gate():
    if request.path in _OPEN_CAM_PATHS:
        return None
    if auth.is_developer():
        return None
    if request.path.startswith("/api/") or request.path.startswith("/video_feed"):
        return auth.deny_json(
            "Chức năng debug camera chỉ dành cho tài khoản Phát triển (developer)", 401
        )
    host = (request.host or "localhost").split(":")[0] or "localhost"
    return redirect(f"{request.scheme}://{host}:{SETTINGS['port']}/")


@cam_app.get("/")
def index():
    """Trang debug của developer: preview + cấu hình engine + cài ESP32."""
    return render_template("index.html")


@cam_app.get("/video_feed")
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
            time.sleep(0.05)  # 20fps đủ cho mắt người

    resp = Response(
        gen(), mimetype="multipart/x-mixed-replace; boundary=frame"
    )
    # Chặn proxy/buffer trung gian dồn khung gây lag cảm nhận.
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    resp.headers["Pragma"] = "no-cache"
    resp.headers["X-Accel-Buffering"] = "no"
    return resp


@cam_app.get("/api/ping")
def ping():
    return jsonify({"ok": True, "pong": True})


@cam_app.get("/api/frame.jpg")
def api_raw_frame():
    """Khung hình NGUYÊN GỐC từ camera (chưa khoanh vùng) — để debug."""
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
    "detect_score",
    "min_face_size",
    "match_margin",
    "track_confirm",
    "track_max_age",
    "track_smooth",
    "track_backend",
    "track_enabled",
)

# key nào đưa xuống engine (phần còn lại chỉ ảnh hưởng vòng lặp/log)
ENGINE_KEYS = (
    "match_threshold",
    "detect_score",
    "min_face_size",
    "match_margin",
    "track_confirm",
    "track_max_age",
    "track_smooth",
    "track_backend",
    "track_enabled",
)


@cam_app.get("/api/config")
def get_config():
    return jsonify(
        {
            "ok": True,
            "env_file": str(ENV_PATH),
            "defaults": DEFAULTS,
            "engine": engine.info(),
            **{k: SETTINGS[k] for k in CONFIG_KEYS},
        }
    )


@cam_app.post("/api/config")
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
        "detect_score": float,
        "min_face_size": int,
        "match_margin": float,
        "track_confirm": float,
        "track_max_age": int,
        "track_smooth": float,
        "track_enabled": bool,
    }
    for key, cast in casts.items():
        if key not in data or data.get(key) in (None, ""):
            continue
        try:
            value = cast(data[key])
        except (TypeError, ValueError):
            return err(f"Giá trị {key} không hợp lệ")
        if key == "track_enabled" and isinstance(data[key], str):
            value = data[key].strip().lower() in ("1", "true", "yes", "on")
        if value != SETTINGS[key]:
            SETTINGS[key] = value
            updated[key.upper()] = value
    if "track_backend" in data and data["track_backend"]:
        backend = str(data["track_backend"]).strip().lower()
        if backend != SETTINGS["track_backend"]:
            SETTINGS["track_backend"] = backend
            updated["TRACK_BACKEND"] = backend

    # đẩy xuống engine (đổi ngưỡng/nguyên tắc khớp thì huấn luyện lại ngưỡng riêng)
    engine_changed = engine.configure(
        **{k: SETTINGS[k] for k in ENGINE_KEYS if k in SETTINGS}
    )
    if "match_threshold" in updated and "match_threshold" not in engine_changed:
        engine.threshold = SETTINGS["match_threshold"]  # cần retrain lại ngưỡng riêng
    if "track_enabled" in updated and not SETTINGS["track_enabled"]:
        engine.reset_tracker()
    if updated:
        save_env(**updated)

    return jsonify(
        {
            "ok": True,
            "updated": sorted(updated),
            "engine_changed": sorted(engine_changed),
            "engine": engine.info(),
            **{k: SETTINGS[k] for k in CONFIG_KEYS},
        }
    )


# ---------------------------------------------------------------------- status
@cam_app.get("/api/status")
def status():
    st = camera.status()
    with out_lock:
        results = [dict(r) for r in output["results"]]
        src = output["source"]
        age = output["age"]
        fresh = time.time() - output["ts"] < 3.0 if output["ts"] else False
    now = time.time()
    esp_last = esp32_state.get("last_ts", 0.0)
    return jsonify(
        {
            "ok": True,
            "camera": st,
            "frame_source": src if fresh else "none",
            "frame_age": age if fresh else None,
            "results": results,
            "students": db.count_students(),
            "trained": engine.trained,
            "threshold": engine.threshold,
            "engine": engine.info(),
            "faces": len(results),
            "matched": sum(1 for r in results if r.get("match")),
            "esp32": {
                "push_alive": bool(esp_last and now - esp_last < 5.0),
                "push_last_age": round(now - esp_last, 2) if esp_last else None,
                "push_count": esp32_state.get("count", 0),
            },
        }
    )


# ------------------------------------------------------- APIs dùng chung PORTAL
# (sinh viên/học sinh, lớp, giáo viên...) — đăng nhập theo vai trò, không mở
# trên cổng debug.
@portal.get("/api/students")
@auth.require_teacher
def api_list_students():
    cls = (request.args.get("class") or "").strip()
    if auth.is_teacher() and cls and not auth.can_see_class(cls):
        return auth.deny_json("Bạn chỉ được xem học sinh lớp của mình", 403)
    if auth.is_teacher() and not cls:
        # giáo viên: chỉ học sinh các lớp được gán
        rows = []
        for c in auth.teacher_class_names():
            rows.extend(db.list_students(class_name=c))
        return jsonify({"ok": True, "students": rows})
    return jsonify({"ok": True, "students": db.list_students(class_name=cls or None)})


def _sample_hint(diag: dict) -> str:
    """Lý do ảnh không làm mẫu được, kèm số đo cụ thể để người dùng tự sửa
    (chụp gần hơn / giữ máy chắc / đủ sáng) thay vì đoán mò."""
    n = int(diag.get("n") or 0)
    if n <= 0:
        return ("không thấy khuôn mặt nào — hãy chụp chính diện, đủ sáng, "
                "mặt ở giữa khung")
    size, need = int(diag.get("max_size") or 0), int(diag.get("need_size") or 0)
    if size < need:
        return (f"thấy mặt nhưng quá nhỏ (lớn nhất {size}px, cần ≥ {need}px) — "
                "đưa máy lại gần hơn để mặt chiếm phần lớn khung")
    blur, need_blur = float(diag.get("blur") or 0), float(diag.get("need_blur") or 0)
    if blur < need_blur:
        return (f"mặt bị mờ (độ nét {blur}, cần ≥ {need_blur}) — giữ máy chắc tay, "
                "đủ sáng, bảo người chụp đứng yên")
    return "mặt chưa đủ rõ để lấy mẫu — thử ảnh khác gần hơn và sáng hơn"


@portal.post("/api/students")
@auth.require_teacher
def api_create_student():
    """Đăng ký học sinh mới — CHỈ bằng upload ảnh (không chụp tại camera)."""
    images, data = collect_images(limit=8)
    full_name = (data.get("full_name") or "").strip()
    date_of_birth = normalize_date(data.get("date_of_birth"))
    class_name = db.normalize_class(data.get("class_name") or "")

    if not full_name:
        return err("Thiếu họ và tên")
    if not date_of_birth:
        return err("Ngày sinh không hợp lệ (dd/mm/yyyy hoặc yyyy-mm-dd)")
    if not class_name:
        return err("Thiếu lớp")
    if auth.is_teacher() and not auth.can_see_class(class_name):
        return auth.deny_json(f"Bạn không được đăng ký học sinh cho lớp {class_name}", 403)
    if not images:
        return err("Chưa có ảnh khuôn mặt — hãy chọn ảnh tải lên")

    student_id = db.create_student(full_name, date_of_birth, class_name)
    out = engine.add_samples(student_id, images)
    if out["saved"] == 0:
        db.delete_student(student_id)
        if out["faces"]:
            return err(
                f"{len(images)} ảnh đã gửi đều trùng với mẫu của người này — "
                "hãy thêm ảnh ở góc nghiêng hoặc khoảng cách khác",
                422,
            )
        hint = _sample_hint(engine.diagnose_sample(images[0])) if images else ""
        return err(
            f"Không dùng được {len(images)} ảnh đã gửi ({hint})",
            422,
        )
    reset_identity_state()
    return jsonify(
        {
            "ok": True,
            "id": student_id,
            "samples": out["saved"],
            "skipped": out["skipped"],
            "student": db.get_student(student_id),
        }
    )


@portal.get("/api/students/<int:sid>")
@auth.require_teacher
def api_get_student(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền xem học sinh này", 403)
    return jsonify({"ok": True, "student": st})


@portal.put("/api/students/<int:sid>")
@auth.require_teacher
def api_update_student(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền sửa học sinh này", 403)
    data = request.get_json(silent=True) or {}
    full_name = (data.get("full_name") or "").strip() or None
    class_name = (data.get("class_name") or "").strip() or None
    date_of_birth = normalize_date(data.get("date_of_birth")) if data.get("date_of_birth") else None
    if data.get("date_of_birth") and not date_of_birth:
        return err("Ngày sinh không hợp lệ")
    if class_name is not None and not auth.can_see_class(class_name):
        return auth.deny_json(f"Bạn không được chuyển học sinh sang lớp {class_name}", 403)
    if not db.update_student(sid, full_name, date_of_birth, class_name):
        return err("Không tồn tại", 404)
    reset_identity_state()
    return jsonify({"ok": True, "student": db.get_student(sid)})


@portal.delete("/api/students/<int:sid>")
@auth.require_teacher
def api_delete_student(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền xoá học sinh này", 403)
    if not db.delete_student(sid):
        return err("Không tồn tại", 404)
    engine.retrain()
    reset_identity_state()
    return jsonify({"ok": True})


@portal.post("/api/students/<int:sid>/samples")
@auth.require_teacher
def api_add_samples(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền sửa học sinh này", 403)
    images, _ = collect_images(limit=8)
    if not images:
        return err("Chưa có ảnh")
    out = engine.add_samples(sid, images)
    if out["saved"] == 0:
        if out["faces"]:
            return jsonify(
                {
                    "ok": True,
                    "added": 0,
                    "skipped": out["skipped"],
                    "note": "Ảnh gần như trùng với mẫu đã có — hãy thêm ảnh ở "
                            "góc nghiêng hoặc khoảng cách khác",
                    "student": db.get_student(sid),
                }
            )
        hint = _sample_hint(engine.diagnose_sample(images[0])) if images else ""
        return err(
            f"Không dùng được {len(images)} ảnh đã gửi ({hint})",
            422,
        )
    reset_identity_state()
    return jsonify(
        {"ok": True, "added": out["saved"], "skipped": out["skipped"],
         "student": db.get_student(sid)}
    )


@portal.post("/api/students/<int:sid>/reset-face")
@auth.require_teacher
def api_reset_face(sid):
    """Xoá mẫu khuôn mặt cũ để đăng ký lại bằng upload."""
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền sửa học sinh này", 403)
    reset_student_face(sid)
    return jsonify({"ok": True, "student": db.get_student(sid)})


# -------------------------------------------------------------------- recognize
@cam_app.post("/api/recognize")
def api_recognize():
    """Nhận diện MỘT ảnh độc lập (debug): không tracking -> kết quả tái lập.

    ?capture=1 -> đồng thời CHẤM ĐIỂM DANH (dùng để test luồng nút bấm ESP32).
    """
    images, _ = collect_images(limit=1)
    if not images:
        return err("Thiếu ảnh")
    results, _frame = engine.process(images[0], draw=False, track=False)
    resp = {"ok": True, "results": results, "count": len(results)}
    if request.args.get("capture") in ("1", "true", "yes"):
        resp.update(record_results(results))
    return jsonify(resp)


# ------------------------------------------------------------------ esp32 frame
@cam_app.post("/api/esp32/frame")
def api_esp32_frame():
    """Nhận khung hình từ ESP32-CAM.

    * ?capture=1 (CHẾ ĐỘ NÚT NHẤN — mặc định của firmware mới): nhận diện đồng
      bộ và GHI ĐIỂM DANH cho buổi hiện tại, trả JSON kèm kết quả điểm danh
      ('matched', 'already', 'full_name', 'session'...) để ESP32 nháy đèn báo.
    * POST liên tục (không capture): chỉ cập nhật hub cho preview, trả kết quả
      CACHED của process_loop (5ms) — không inference, không điểm danh.

    Hỗ trợ 3 cách gửi của firmware (xem PUSH_MODE_MJPEG):
      1. image/jpeg / application/octet-stream : body = 1 ảnh JPEG thô.
      2. multipart/form-data, field "file"     : PUSH_MODE_MJPEG=0 (khuyên dùng).
      3. multipart/x-mixed-replace             : PUSH_MODE_MJPEG=1 (lũy MJPEG).
    """
    ctype = (request.content_type or "").lower()
    img = None
    if "image/jpeg" in ctype or "application/octet-stream" in ctype:
        img = decode_image(request.get_data())
    elif "x-mixed-replace" in ctype or "multipart/mixed" in ctype:
        for chunk in reversed(extract_jpegs(request.get_data() or b"")):
            img = decode_image(chunk)
            if img is not None:
                break
    else:
        images, _ = collect_images(limit=1)
        img = images[0] if images else None
        # Fallback cuối: body thô có thể vẫn là JPEG dù header sai.
        if img is None:
            try:
                raw = request.get_data() or b""
                if raw[:2] == b"\xff\xd8":
                    img = decode_image(raw)
            except Exception:  # noqa: BLE001
                pass
    if img is None:
        return err("Không đọc được ảnh JPEG (cần image/jpeg, "
                   "multipart field 'file'/'images', hoặc MJPEG)")
    now = time.time()
    prev_ts = esp32_state.get("last_ts", 0.0)
    set_frame(img, "esp32")
    esp32_state["last_ts"] = now
    esp32_state["count"] += 1
    # Nhớ IP ESP32 (để panel cài đặt suy ra địa chỉ web :80 khi chỉ dùng PUSH).
    if request.remote_addr:
        esp32_state["ip"] = request.remote_addr

    want_capture = request.args.get("capture") in ("1", "true", "yes")
    mode = (request.args.get("mode") or request.args.get("fast") or "").strip().lower()
    want_sync = request.args.get("sync") == "1" or mode in ("sync", "0")
    want_fast = request.args.get("fast") == "1" or mode in ("fast", "1")
    streaming = (now - prev_ts) < 1.0 if prev_ts else False

    if not want_capture and (want_fast or (streaming and not want_sync)):
        # PUSH liên tục: trả cached, process_loop lo inference cho preview.
        with out_lock:
            cached = [dict(r) for r in output["results"]]
        esp32_state["last_count"] = len(cached)
        return jsonify({"ok": True, "results": cached, "count": len(cached),
                        "cached": True, "matched": 0, "already": False,
                        "full_name": "", "class_name": "", "session": 0,
                        "session_name": ""})

    # Nhận diện đồng bộ cho đúng ảnh vừa nhận; capture=1 -> ghi điểm danh.
    results, _annotated = engine.process(img, draw=False, track=False)
    esp32_state["last_count"] = len(results)
    resp = {"ok": True, "results": results, "count": len(results), "cached": False}
    if want_capture:
        resp.update(record_results(results))
    return jsonify(resp)


@cam_app.get("/api/esp32/status")
def api_esp32_status():
    """Trạng thái nguồn ESP32: lần PUSH cuối, tổng khung, nguồn PULL hiện tại."""
    now = time.time()
    last = esp32_state.get("last_ts", 0.0)
    with hub_lock:
        _f, hub_ts, hub_src = hub["frame"], hub["ts"], hub["source"]
    return jsonify({
        "ok": True,
        "push_last_age": round(now - last, 2) if last else None,
        "push_alive": bool(last and now - last < 5.0),
        "push_count": esp32_state.get("count", 0),
        "push_last_faces": esp32_state.get("last_count", 0),
        "hub_source": hub_src,
        "hub_age": round(now - hub_ts, 2) if hub_ts else None,
        "stream_url": SETTINGS.get("stream_url", ""),
        "esp32_ip": esp32_state.get("ip"),
        "cam_base": esp32_base_url(),
        "hint_capture": "Nút nhấn ESP32 -> POST /api/esp32/frame?capture=1",
        "hint_pull": "PULL (debug): STREAM_URL=http://<ip-esp32>:81/stream",
    })


# ------------------------------------------------- proxy cài đặt ESP32-CAM
def esp32_base_url():
    """Địa chỉ web của ESP32-CAM (luôn cổng 80, dạng http://<ip>/) hoặc None.

    Suy từ STREAM_URL PULL (http://<ip>:81/stream -> http://<ip>/) hoặc IP của
    lần PUSH gần nhất. Trang debug dùng proxy này để chỉnh cam một chỗ.
    """
    url = (SETTINGS.get("stream_url") or "").strip()
    if url and not url.isdigit() and not url.startswith("rtsp://"):
        try:
            p = urlsplit(url if "://" in url else "http://" + url)
        except ValueError:
            p = None
        if p is not None and p.hostname:
            path = (p.path or "").strip("/").lower()
            if p.port == 81 or path in ("stream", "capture", "jpg", "cam", "mjpeg"):
                return f"{p.scheme or 'http'}://{p.hostname}/"
    ip = esp32_state.get("ip")
    if ip:
        return f"http://{ip}/"
    return None


def _esp32_base_or_err():
    """Base ESP32 từ ?base= (ưu tiên) hoặc suy tự động. Trả (base, None) hoặc
    (None, response_lỗi)."""
    base = (request.args.get("base") or "").strip().rstrip("/") + "/"
    if base == "/":
        base = esp32_base_url()
    elif not base.lower().startswith(("http://", "https://")):
        return None, err("Tham số base phải là http(s)://...", 400)
    if not base:
        return None, err("Chưa thấy ESP32 (STREAM_URL chưa trỏ :81/stream và "
                         "chưa nhận POST nào)", 503)
    return base, None


@cam_app.get("/api/esp32/cam_status")
def api_esp32_cam_status():
    """Đọc /status của ESP32 (độ phân giải, chất lượng, cam đang bật/tắt...)."""
    base, e = _esp32_base_or_err()
    if e:
        return e
    try:
        r = requests.get(base + "status", timeout=5,
                         headers={"User-Agent": "face-server"})
    except Exception as exc:  # noqa: BLE001
        return err(f"Không tới được ESP32 ({base}): {exc}", 502)
    if r.status_code >= 400:
        return err(f"ESP32 trả lỗi HTTP {r.status_code}", 502)
    try:
        data = r.json()
    except ValueError:
        return err("ESP32 trả về không phải JSON", 502)
    now = time.time()
    esp_last = esp32_state.get("last_ts", 0.0)
    return jsonify({"ok": True, "base": base, "esp32": data,
                    "push_alive": bool(esp_last and now - esp_last < 5.0),
                    "push_count": esp32_state.get("count", 0)})


@cam_app.get("/api/esp32/control")
def api_esp32_control():
    """Chuyển lệnh chỉnh cam: /api/esp32/control?var=framesize&val=8."""
    var = (request.args.get("var") or "").strip()
    val = (request.args.get("val") or "").strip()
    if not var.isidentifier() or val == "":
        return err("Thiếu hoặc sai var/val (vd var=framesize&val=8)")
    base, e = _esp32_base_or_err()
    if e:
        return e
    try:
        r = requests.get(base + "control", params={"var": var, "val": val},
                         timeout=5, headers={"User-Agent": "face-server"})
    except Exception as exc:  # noqa: BLE001
        return err(f"Không tới được ESP32 ({base}): {exc}", 502)
    if r.status_code >= 400:
        return err(f"ESP32 trả lỗi HTTP {r.status_code}", 502)
    return jsonify({"ok": True, "var": var, "val": val})


@cam_app.get("/api/esp32/flash")
def api_esp32_flash():
    """Bật/tắt đèn flash ESP32: /api/esp32/flash?state=1."""
    state = (request.args.get("state") or "1").strip()
    if state not in ("0", "1"):
        return err("state phải là 0 hoặc 1")
    base, e = _esp32_base_or_err()
    if e:
        return e
    try:
        r = requests.get(base + "flash", params={"state": state},
                         timeout=5, headers={"User-Agent": "face-server"})
    except Exception as exc:  # noqa: BLE001
        return err(f"Không tới được ESP32 ({base}): {exc}", 502)
    if r.status_code >= 400:
        return err(f"ESP32 trả lỗi HTTP {r.status_code}", 502)
    return jsonify({"ok": True, "flash": int(state)})


@cam_app.get("/api/esp32/live")
def api_esp32_live():
    """Bật/tắt camera ESP32 cho mục đích debug: /api/esp32/live?state=1.

    Firmware chế độ nút nhấn giữ cam TẮT khi nghỉ; developer muốn xem stream
    thì bật cam lại từ đây (hoặc mở /stream trên chính ESP32).
    """
    state = (request.args.get("state") or "1").strip()
    if state not in ("0", "1"):
        return err("state phải là 0 hoặc 1")
    base, e = _esp32_base_or_err()
    if e:
        return e
    try:
        r = requests.get(base + "live", params={"state": state},
                         timeout=8, headers={"User-Agent": "face-server"})
    except Exception as exc:  # noqa: BLE001
        return err(f"Không tới được ESP32 ({base}): {exc}", 502)
    if r.status_code >= 400:
        return err(f"ESP32 trả lỗi HTTP {r.status_code}", 502)
    try:
        data = r.json()
    except ValueError:
        data = {"ok": 1}
    return jsonify({"ok": True, "state": int(state), "esp32": data})


# -------------------------------------------------------------------------- log
@cam_app.get("/api/log")
def api_log():
    return jsonify({"ok": True, "log": db.recent_log(request.args.get("limit", 30, type=int))})


# ============================================================ TIỆN ÍCH DÙNG CHUNG
def month_range(month: str):
    """'2026-10' -> (ngày đầu, ngày cuối, danh sách ngày). Sai định dạng -> tháng này."""
    month = (month or "").strip()
    try:
        first = datetime.strptime(month + "-01", "%Y-%m-%d").replace(day=1)
        nxt = (first.replace(day=28) + timedelta(days=4)).replace(day=1)
    except ValueError:
        today = datetime.now()
        first = today.replace(day=1)
        nxt = (first.replace(day=28) + timedelta(days=4)).replace(day=1)
    last = nxt - timedelta(days=1)
    days = [
        (first + timedelta(days=i)).strftime("%Y-%m-%d")
        for i in range((last - first).days + 1)
    ]
    return first.strftime("%Y-%m-%d"), last.strftime("%Y-%m-%d"), days


def week_range(day_str: str):
    """Ngày -> (thứ Hai, chủ nhật) của tuần chứa ngày đó."""
    d = datetime.strptime(day_str, "%Y-%m-%d")
    monday = d - timedelta(days=d.weekday())
    sunday = monday + timedelta(days=6)
    return monday.strftime("%Y-%m-%d"), sunday.strftime("%Y-%m-%d")


def reset_student_face(sid: int):
    """Xoá toàn bộ mẫu khuôn mặt của 1 học sinh rồi dựng lại mô hình."""
    folder = db.FACE_DIR / str(sid)
    if folder.exists():
        shutil.rmtree(folder, ignore_errors=True)
    db.set_sample_count(sid, 0)
    engine.retrain()
    reset_identity_state()


def _rss_mb():
    """Bộ nhớ đang dùng của tiến trình (MB) — Windows; None nếu không lấy được."""
    try:
        import ctypes
        from ctypes import wintypes

        class PMC(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        buf = PMC()
        buf.cb = ctypes.sizeof(PMC)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(
            ctypes.windll.kernel32.GetCurrentProcess(), ctypes.byref(buf), buf.cb
        )
        return round(buf.WorkingSetSize / (1024 * 1024), 1) if ok else None
    except Exception:  # noqa: BLE001
        return None


def system_info():
    """Số liệu giám sát hệ thống cho trang admin."""
    now = time.time()
    st = camera.status()
    with out_lock:
        fresh = time.time() - output["ts"] < 3.0 if output["ts"] else False
        age = output["age"]
        src = output["source"] if fresh else "none"
    esp_last = esp32_state.get("last_ts", 0.0)
    db_path = Path(db.DB_PATH)
    info = engine.info()
    return {
        "uptime_sec": int(now - STARTED_AT),
        "started_at": datetime.fromtimestamp(STARTED_AT).strftime("%Y-%m-%d %H:%M:%S"),
        "threads": threading.active_count(),
        "rss_mb": _rss_mb(),
        "frame_source": src,
        "frame_age": age if fresh else None,
        "camera": st,
        "engine": info,
        "counts": {
            "students": db.count_students(),
            "classes": len(db.list_classes()),
            "teachers": len(db.list_users(role="teacher")),
            "users": len(db.list_users()),
            "log_rows": len(db.recent_log(500)),
        },
        "esp32": {
            "push_alive": bool(esp_last and now - esp_last < 5.0),
            "push_last_age": round(now - esp_last, 2) if esp_last else None,
            "push_count": esp32_state.get("count", 0),
            "ip": esp32_state.get("ip"),
        },
        "db": {
            "path": str(db_path),
            "size_mb": round(db_path.stat().st_size / (1024 * 1024), 2)
            if db_path.exists() else 0,
        },
        "ports": {"portal": SETTINGS["port"], "camera": SETTINGS["camera_port"]},
        "stream_url": SETTINGS.get("stream_url", ""),
    }


# ========================================================= PORTAL: ĐĂNG NHẬP
ROLE_TABS = {
    # (khoá tab, biểu tượng, nhãn) — thứ tự hiển thị trên thanh tab
    "admin": [
        ("tong-quan", "📊", "Tổng quan"),
        ("dang-ky", "🖼️", "Đăng ký khuôn mặt"),
        ("hom-nay", "📋", "Hôm nay"),
        ("diem-danh", "🗓️", "Điểm danh"),
        ("bao-cao", "📈", "Báo cáo"),
        ("lop", "🏫", "Lớp & lịch học"),
        ("giao-vien", "👩‍🏫", "Giáo viên"),
        ("he-thong", "🖥️", "Hệ thống"),
    ],
    "teacher": [
        ("dang-ky", "🖼️", "Đăng ký khuôn mặt"),
        ("hom-nay", "📋", "Hôm nay"),
        ("bao-cao", "📈", "Báo cáo"),
    ],
}


@portal.get("/")
def landing():
    """Trang đăng nhập DUY NHẤT: nhập user + password, server tự chọn trang."""
    if auth.is_admin() or auth.is_teacher():
        return redirect(url_for("quan_ly"))
    if auth.is_developer():
        return redirect(url_for("to_debug"))
    return render_template("landing.html", error="", notice="", mode="login")


@portal.post("/dang-nhap")
def dang_nhap():
    """Xác thực 1 lần cho mọi vai trò -> điều hướng theo role của tài khoản."""
    username = (request.form.get("username") or "").strip()
    password = request.form.get("password") or ""
    error = ""
    notice = ""
    user = db.get_user(username) if username else None
    if user and auth.verify_password(password, user["password_hash"]):
        auth.login_user(user)
        if user["role"] == "developer":
            return redirect(url_for("to_debug"))
        return redirect(url_for("quan_ly"))
    if not username or not password:
        error = "Vui lòng nhập tên đăng nhập và mật khẩu"
    else:
        error = "Sai tên đăng nhập hoặc mật khẩu"
    return render_template("landing.html", error=error, notice=notice, mode="login")


@portal.route("/dang-xuat", methods=["GET", "POST"])
def dang_xuat():
    auth.logout()
    return redirect(url_for("landing"))


@portal.get("/debug")
def to_debug():
    """Điều hướng tới trang debug (cổng CAMERA_PORT) — nơi ở của developer."""
    return redirect(f"{request.scheme}://{(request.host or '').split(':')[0]}"
                    f":{SETTINGS['camera_port']}/")


# ============================================================ TRANG QUẢN LÝ
def _my_classes():
    """Danh sách lớp mà người dùng hiện tại được phép thao tác."""
    if auth.is_admin():
        return [c["name"] for c in db.list_classes()]
    return auth.teacher_class_names()


def _pick_class(my_classes, param=None):
    cls = (param or "").strip()
    if cls in my_classes:
        return cls
    return my_classes[0] if my_classes else ""


def _students_with_attendance(students):
    """Gắn thêm số ngày đã đi học + lần nhận diện cuối cho từng học sinh."""
    for s in students:
        att = db.attendance_summary(s["id"])
        s["days_present"] = att["days"]
        s["last_seen"] = att["detail"][0]["last_seen"] if att["detail"] else ""
    return students


def _attendance_tab(ctx, my_classes):
    """Lưới điểm danh tháng (admin) — ô theo ngày, ký hiệu theo buổi."""
    month = ctx["month"]
    cls = _pick_class(my_classes, request.args.get("class"))
    start, _end, days = month_range(month)
    students = _students_with_attendance(db.list_students(class_name=cls) if cls else [])
    ids = [s["id"] for s in students]
    last_day = days[-1] if days else start
    att = db.attendance_matrix(ids, start, last_day)
    absences = db.absence_map(ids, start, last_day)
    schedule = db.get_schedule(cls) if cls else dict(db.DEFAULT_SCHEDULE)
    expected = int(schedule.get("sessions", 2))
    today = db.today_str()
    for s in students:
        mine = att.get(s["id"], {})
        s["month_present"] = sum(1 for d in days if d in mine)
        s["month_full"] = sum(
            1 for d in days
            if d in mine and len(mine[d].get("sessions") or [1]) >= expected
        )
        s["month_absent"] = sum(
            1 for d in days
            if d < today and d not in mine and (s["id"], d) not in absences
        )
    present_now = sum(1 for s in students
                      if today in att.get(s["id"], {}) or (s["id"], today) in absences)
    ctx.update(classes=my_classes, cls=cls, students=students, att=att,
               absences=absences, days=days, month=month, today=today,
               schedule=schedule, expected=expected, present_now=present_now)
    return ctx


def _register_tab(ctx, my_classes):
    """Tab đăng ký khuôn mặt (UPLOAD ảnh — không chụp tại camera)."""
    cls = _pick_class(my_classes, request.args.get("class"))
    q = (request.args.get("q") or "").strip()
    students = []
    if auth.is_teacher():
        # giáo viên: luôn khuôn trong các lớp được gán
        for c in my_classes:
            students.extend(db.list_students(class_name=c, query=q or None))
    elif q:
        # admin gõ từ khoá -> tìm trong TOÀN trường (không cần nhớ lớp)
        students = db.list_students(query=q)
    else:
        students = db.list_students(class_name=cls or None)
    ctx.update(classes=my_classes, cls=cls, q=q,
               students=_students_with_attendance(students))
    return ctx


def _today_tab(ctx, my_classes):
    """Danh sách hôm nay: ai có mặt từng buổi, ai vắng (kèm ghi chú nghỉ)."""
    cls = _pick_class(my_classes, request.args.get("class"))
    report = db.day_report(cls, db.today_str()) if cls else None
    rows = report["rows"] if report else []
    n_sessions = int((report or {}).get("schedule", {}).get("sessions", 2))
    present = [r for r in rows if r["sessions_present"] >= n_sessions]
    partial = [r for r in rows
               if 0 < r["sessions_present"] < n_sessions]
    absent = [r for r in rows
              if r["sessions_present"] == 0 and r["status"] != "leave"
              and "absent" in r["session_state"].values()]
    leave = [r for r in rows if r["status"] == "leave"]
    ctx.update(classes=my_classes, cls=cls, report=report, rows=rows,
               day=db.today_str(), n_sessions=n_sessions,
               present=present, partial=partial, absent=absent, leave=leave)
    return ctx


def _report_tab(ctx, my_classes):
    """Báo cáo ngày / tuần / tháng dưới dạng bảng kiểu Excel + link CSV."""
    mode = (request.args.get("mode") or "day").strip()
    if mode not in ("day", "week", "month"):
        mode = "day"
    day = _valid_date(request.args.get("date"))
    if mode == "day":
        start = end = day
    elif mode == "week":
        start, end = week_range(day)
    else:  # month
        start, end, _d = month_range(day[:7])
    cls = _pick_class(my_classes, request.args.get("class"))
    report = db.range_report(cls, start, end) if cls else None
    ctx.update(classes=my_classes, cls=cls, mode=mode, date=day,
               start=start, end=end, report=report,
               csv_url=(url_for("api_report_csv", **{"class": cls, "start": start,
                                                      "end": end})
                        if cls else ""))
    return ctx


@portal.get("/quan-ly")
def quan_ly():
    """MỘT trang cho admin + giáo viên — nội dung đổi theo tab được phép."""
    role = auth.current_role()
    if role == "developer":
        return redirect(url_for("to_debug"))
    if not role:
        return redirect(url_for("landing"))
    tabs = ROLE_TABS[role]
    keys = [t[0] for t in tabs]
    tab = (request.args.get("tab") or keys[0]).strip()
    if tab not in keys:  # tab của vai trò khác -> về tab đầu tiên
        tab = keys[0]

    ctx = {"role": role, "tabs": tabs, "tab": tab, "me": auth.current_teacher(),
           "today": db.today_str(), "role_label": auth.ROLE_LABELS.get(role, ""),
           # `month` luôn có mặt để thanh tab giữ nguyên tháng đang xem khi chuyển tab
           "month": (request.args.get("month") or "").strip()
           or datetime.now().strftime("%Y-%m")}

    my_classes = _my_classes()

    if tab == "dang-ky":
        ctx = _register_tab(ctx, my_classes)
    elif tab == "hom-nay":
        ctx = _today_tab(ctx, my_classes)
    elif tab == "bao-cao":
        ctx = _report_tab(ctx, my_classes)
    elif tab == "diem-danh":
        ctx = _attendance_tab(ctx, my_classes)
    elif role == "admin":
        if tab == "tong-quan":
            ctx.update(overview=db.overview(), present=db.present_today(),
                       classes=my_classes)
        elif tab == "lop":
            ctx.update(classes=db.list_classes(), my_classes=my_classes)
        elif tab == "giao-vien":
            ctx.update(teachers=db.list_teachers(), classes=db.list_classes())
        elif tab == "he-thong":
            ctx.update(sysinfo=system_info())

    return render_template("portal.html", **ctx)


# ====================================================================== API LỚP
@portal.get("/api/classes")
@auth.require_teacher
def api_list_classes():
    return jsonify({"ok": True, "classes": db.list_classes(),
                    "my_classes": _my_classes()})


@portal.post("/api/classes")
@auth.require_admin
def api_add_class():
    data = request.get_json(silent=True) or request.form or {}
    try:
        row = db.add_class(data.get("name", ""))
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "class": row, "classes": db.list_classes()})


@portal.post("/api/classes/<path:name>/rename")
@auth.require_admin
def api_rename_class(name):
    data = request.get_json(silent=True) or {}
    new_name = (data.get("name") or "").strip()
    try:
        row = db.rename_class(name, new_name)
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "class": row, "classes": db.list_classes()})


@portal.post("/api/classes/<path:name>/schedule")
@auth.require_admin
def api_set_schedule(name):
    """Đặt lịch học của lớp: số buổi/ngày + khung giờ (điểm danh theo buổi)."""
    data = request.get_json(silent=True) or {}
    try:
        schedule = db.set_schedule(name, data)
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "schedule": schedule, "classes": db.list_classes()})


@portal.delete("/api/classes/<path:name>")
@auth.require_admin
def api_delete_class(name):
    move_to = request.args.get("move_to") or (
        (request.get_json(silent=True) or {}).get("move_to")
    )
    try:
        out = db.delete_class(name, move_to)
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "deleted": out, "classes": db.list_classes()})


# ================================================================== API GIÁO VIÊN
@portal.post("/api/teachers")
@auth.require_admin
def api_create_teacher():
    """Admin tạo tài khoản giáo viên (mật khẩu do admin đặt)."""
    data = request.get_json(silent=True) or {}
    password = data.get("password", "")
    if not auth.password_strength_ok(password):
        return err("Mật khẩu phải có ít nhất 4 ký tự")
    try:
        tid = db.create_teacher(
            data.get("username", ""), auth.hash_password(password),
            data.get("full_name", ""), data.get("classes") or [],
        )
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "id": tid, "teachers": db.list_teachers()})


@portal.put("/api/teachers/<int:tid>/classes")
@auth.require_admin
def api_set_teacher_classes(tid):
    data = request.get_json(silent=True) or {}
    try:
        names = db.set_teacher_classes(tid, data.get("classes") or [])
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "classes": names, "teachers": db.list_teachers()})


@portal.get("/api/teachers")
@auth.require_admin
def api_list_teachers():
    return jsonify({"ok": True, "teachers": db.list_teachers()})


@portal.delete("/api/teachers/<int:tid>")
@auth.require_admin
def api_delete_teacher(tid):
    try:
        if not db.delete_user(tid):
            return err("Không tồn tại", 404)
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True, "teachers": db.list_teachers()})


# =============================================================== ĐIỂM DANH / XEM
@portal.get("/api/students/<int:sid>/attendance")
@auth.require_teacher
def api_student_attendance(sid):
    st = db.get_student(sid)
    if not st:
        return err("Không tồn tại", 404)
    if not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Không có quyền xem học sinh này", 403)
    return jsonify({"ok": True, "student": st,
                    "attendance": db.attendance_summary(sid)})


# ============================================================ GHI CHÚ VẮNG / NGHỈ
@portal.post("/api/absences")
@auth.require_teacher
def api_set_absence():
    data = request.get_json(silent=True) or {}
    sid = data.get("student_id")
    day = (data.get("day") or "").strip()
    if not sid:
        return err("Thiếu học sinh")
    st = db.get_student(int(sid))
    if not st or not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Bạn chỉ được ghi chú lớp của mình", 403)
    me = auth.current_teacher()
    created_by = me["full_name"] if me else ("Quản trị viên" if auth.is_admin() else "")
    try:
        db.set_absence(int(sid), day, data.get("reason", ""), created_by)
    except ValueError as exc:
        return err(str(exc))
    return jsonify({"ok": True})


@portal.delete("/api/absences")
@auth.require_teacher
def api_clear_absence():
    data = request.get_json(silent=True) or request.args
    sid, day = data.get("student_id"), data.get("day")
    if not sid or not day:
        return err("Thiếu học sinh hoặc ngày")
    st = db.get_student(int(sid))
    if not st or not auth.can_see_class(st.get("class_name") or ""):
        return auth.deny_json("Bạn chỉ được sửa lớp của mình", 403)
    db.clear_absence(int(sid), day)
    return jsonify({"ok": True})


# ================================================================ BÁO CÁO / CSV
def _report_or_deny():
    """Xây report cho request báo cáo + kiểm quyền lớp. Trả (report, None) hoặc
    (None, response_lỗi)."""
    my_classes = _my_classes()
    cls = _pick_class(my_classes, request.args.get("class"))
    if not cls:
        return None, err("Bạn chưa có lớp nào", 400)
    start = _valid_date(request.args.get("start"))
    end = _valid_date(request.args.get("end"))
    if start > end:
        start, end = end, start
    return db.range_report(cls, start, end), None


@portal.get("/api/report.csv")
@auth.require_teacher
def api_report_csv():
    """Tải báo cáo điểm danh dạng CSV (mở trực tiếp bằng Excel)."""
    report, e = _report_or_deny()
    if e:
        return e
    buf = io.StringIO()
    writer = csv.writer(buf)
    n_sessions = int(report["schedule"].get("sessions", 2))
    header = ["Mã", "Họ và tên", "Lớp"]
    header += [f"Buổi {'/'.join(str(i) for i in range(1, n_sessions + 1))} {d[8:10]}/{d[5:7]}"
               for d in report["days"]]
    header += ["Ngày đủ buổi", "Ngày có mặt", "Buổi lẻ", "Nghỉ", "Vắng"]
    writer.writerow(header)
    for s in report["students"]:
        row = [s["id"], s["full_name"], s["class_name"]]
        for d in report["days"]:
            cell = report["matrix"][s["id"]][d]
            state = cell["state"]
            if state in ("full", "partial"):
                row.append("+".join(str(x) for x in (cell["sessions"] or [1])))
            elif state == "leave":
                row.append("Nghỉ")
            elif state == "absent":
                row.append("X")
            else:  # pending / future
                row.append("")
        t = report["totals"][s["id"]]
        row += [t["full"], t["attended"], t["partial"], t["leave"], t["absent"]]
        writer.writerow(row)
    # \ufeff: BOM để Excel Windows mở đúng tiếng Việt
    data = "﻿" + buf.getvalue()
    filename = f"diem-danh_{report.get('class_name') or 'lop'}_{report['start']}_{report['end']}.csv"
    return Response(
        data, mimetype="text/csv; charset=utf-8",
        headers={"Content-Disposition": f"attachment; filename=\"{filename}\""},
    )


@portal.get("/api/system")
@auth.require_admin
def api_system():
    return jsonify({"ok": True, **system_info()})


@portal.get("/api/portal/overview")
@auth.require_admin
def api_overview():
    return jsonify({"ok": True, **db.overview()})


# ----------------------------------------------------------------------- start
def main():
    """Chạy HAI máy chủ web trong cùng một tiến trình (dùng chung engine + camera)."""
    threading.Thread(target=process_loop, daemon=True, name="process").start()
    if SETTINGS["stream_url"]:
        camera.set_url(SETTINGS["stream_url"])

    host = SETTINGS["host"]
    portal_port = SETTINGS["port"]
    camera_port = SETTINGS["camera_port"]
    info = engine.info()

    # Cổng debug để riêng: xem livestream MJPEG là việc nặng, tách ra thì trang
    # quản lý vẫn mở nhanh và nhiều người dùng cùng lúc không làm chậm nhau.
    cam_srv = make_server(host, camera_port, cam_app, threaded=True)
    threading.Thread(target=cam_srv.serve_forever, daemon=True,
                     name="http-camera").start()
    portal_srv = make_server(host, portal_port, portal, threaded=True)

    print("=" * 66)
    print("  Server nhan dien khuon mat dang chay tren HAI cong")
    print("=" * 66)
    print(f"  Cau hinh     : {ENV_PATH}")
    print(f"  Dang nhap    : admin / {DEV_USERNAME} (mat khau xem .env)")
    print(f"  Camera (PULL debug): {SETTINGS['stream_url'] or '(chua dat STREAM_URL)'}")
    print(f"  Phat hien    : {info['detector']}"
          f"{' (can theo diem moc)' if info['landmark_align'] else ''}")
    print(f"  Nhan dang    : {info['recognizer']} - nguong {info['threshold']:g}"
          f" ({info['metric']})"
          f"{' + nguong rieng/nguoi' if info['adaptive_threshold'] else ''}")
    print(f"  So nguoi     : {info['identities']} ({info['faces_loaded']} mau)")
    print("-" * 66)
    print(f"  CONG DANG NHAP: http://localhost:{portal_port}"
          f"   (admin / giao vien)")
    print(f"  CONG DEBUG    : http://localhost:{camera_port}"
          f"   (chi tai khoan developer)")
    print(f"  ESP32 NUT NHAN: POST http://<ip-may>:{camera_port}/api/esp32/frame?capture=1")
    print("=" * 66)

    try:
        portal_srv.serve_forever()          # cổng portal giữ tiến trình chính
    except KeyboardInterrupt:
        print("\nDang dung server...")
    finally:
        portal_srv.server_close()
        cam_srv.shutdown()
        cam_srv.server_close()


if __name__ == "__main__":
    main()
