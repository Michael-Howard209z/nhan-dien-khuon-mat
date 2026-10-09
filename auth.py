"""Xác thực người dùng: quản trị viên (admin), phát triển (developer), giáo viên.

MỌI vai trò đăng nhập ở CÙNG MỘT trang (`landing`) bằng tên đăng nhập + mật
khẩu; server tự điều hướng theo vai trò của tài khoản:

  * admin     -> trang quản trị  (/quan-ly): lớp, học sinh, giáo viên, giám sát
  * teacher   -> trang giáo viên (/quan-ly): đăng ký học sinh, danh sách hôm nay,
                 báo cáo ngày/tuần/tháng
  * developer -> trang debug      (cổng CAMERA_PORT): preview, cấu hình engine,
                 cài đặt ESP32-CAM — chỉ developer mới vào được

Tài khoản admin/developer seed từ `.env` (ADMIN_PASSWORD, DEV_PASSWORD); tài
khoản giáo viên do admin tạo. Mật khẩu lưu bằng PBKDF2-SHA256 kèm muối (không
lưu bản rõ).
"""
import functools
import hashlib
import hmac
import secrets

from flask import redirect, request, session, url_for

import database as db

# Số vòng lặp PBKDF2 — đủ nhanh (vài chục ms) nhưng chậm để dò mật khẩu hàng loạt
ITERATIONS = 120_000

ROLES = ("admin", "developer", "teacher")
ROLE_LABELS = {"admin": "Quản trị", "developer": "Phát triển", "teacher": "Giáo viên"}


# ------------------------------------------------------------------- mật khẩu
def hash_password(password: str) -> str:
    """'pbkdf2$<số vòng>$<muối hex>$<băm hex>'"""
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, ITERATIONS)
    return f"pbkdf2${ITERATIONS}${salt.hex()}${digest.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        scheme, iters, salt_hex, digest_hex = stored.split("$")
        if scheme != "pbkdf2":
            return False
        expect = bytes.fromhex(digest_hex)
        got = hashlib.pbkdf2_hmac(
            "sha256", (password or "").encode("utf-8"),
            bytes.fromhex(salt_hex), int(iters),
        )
        return hmac.compare_digest(expect, got)
    except (AttributeError, TypeError, ValueError):
        return False


def check_admin_password(password: str, configured: str) -> bool:
    """So khớp mật khẩu quản trị lưu trong `.env` (dùng khi seed tài khoản)."""
    return hmac.compare_digest((password or "").strip(), (configured or "").strip())


def password_strength_ok(password: str) -> bool:
    return len(password or "") >= 4


# ------------------------------------------------------------------- phiên đăng nhập
def _wipe():
    session.clear()


def login_user(user: dict):
    """Đăng nhập theo tài khoản (bất kỳ vai trò nào) và ghi phiên."""
    role = (user.get("role") or "").strip()
    if role not in ROLES:
        raise ValueError("Tài khoản không có vai trò hợp lệ")
    session.clear()
    session["role"] = role
    session["user_id"] = int(user["id"])
    session["username"] = user["username"]
    session["full_name"] = user.get("full_name") or user["username"]
    if role == "teacher":
        names = db.teacher_class_names(int(user["id"]))
        session["teacher_id"] = int(user["id"])
        session["teacher_name"] = session["full_name"]
        session["teacher_classes"] = names


def logout():
    _wipe()


def current_role() -> str:
    return session.get("role") or ""


def is_admin() -> bool:
    return current_role() == "admin"


def is_developer() -> bool:
    return current_role() == "developer"


def is_teacher() -> bool:
    return current_role() == "teacher"


def is_staff() -> bool:
    """Đã đăng nhập bất kỳ vai trò nào."""
    return current_role() in ROLES


def current_user():
    """Tài khoản đang đăng nhập từ DB (None nếu phiên hết hạn/tài khoản bị xoá)."""
    uid = session.get("user_id")
    username = session.get("username")
    if not uid or not username:
        return None
    u = db.get_user(username)
    if not u or int(u["id"]) != int(uid):
        _wipe()  # tài khoản đã bị xoá hoặc phiên lạc -> đăng xuất luôn
        return None
    return u


def current_teacher():
    """Thông tin giáo viên đang đăng nhập (None nếu không phải giáo viên)."""
    if not is_teacher() or "teacher_id" not in session:
        return None
    data = db.get_teacher_by_id(int(session["teacher_id"]))
    if not data:
        _wipe()
        return None
    data.pop("password_hash", None)
    data["classes"] = db.teacher_class_names(int(data["id"]))
    return data


def teacher_class_names() -> list:
    """Các lớp giáo viên được xem. Rỗng nếu không phải giáo viên."""
    if not is_teacher():
        return []
    if "teacher_classes" in session:
        return list(session["teacher_classes"])
    tid = session.get("teacher_id")
    names = db.teacher_class_names(int(tid)) if tid is not None else []
    session["teacher_classes"] = names
    return names


def can_see_class(class_name: str) -> bool:
    """Quyền xem 1 lớp: admin = tất cả, giáo viên = lớp được gán, còn lại = không."""
    key = db.normalize_class(class_name)
    if not key:
        return False
    if is_admin():
        return True
    if is_teacher():
        return key in teacher_class_names()
    return False


def can_manage_students() -> bool:
    """Được đăng ký/sửa học sinh: admin + giáo viên (developer thì không)."""
    return is_admin() or is_teacher()


# ------------------------------------------------------------------ trang bảo vệ
def deny_page():
    """Đưa người dùng về đúng chỗ: chưa đăng nhập -> trang đăng nhập,
    đã đăng nhập nhưng sai vai trò -> trang của chính vai trò đó."""
    from flask import abort

    role = current_role()
    try:
        if not role:
            return redirect(url_for("landing"))
        if role == "developer":
            return redirect(url_for("to_debug"))
        return redirect(url_for("quan_ly"))
    except Exception:  # noqa: BLE001 - endpoint không có ở app hiện tại (2 cổng)
        abort(403)


def require_admin(fn):
    @functools.wraps(fn)
    def wrapper(*a, **kw):
        if not is_admin():
            return deny_page()
        return fn(*a, **kw)

    return wrapper


def require_teacher(fn):
    """Cho cả giáo viên lẫn admin đi qua (admin xem được mọi lớp)."""
    @functools.wraps(fn)
    def wrapper(*a, **kw):
        if current_role() not in ("teacher", "admin"):
            return deny_page()
        return fn(*a, **kw)

    return wrapper


def require_developer(fn):
    """Chỉ developer — trang debug/preview camera và cấu hình kỹ thuật."""
    @functools.wraps(fn)
    def wrapper(*a, **kw):
        if not is_developer():
            if wants_json():
                return deny_json("Chức năng này chỉ dành cho tài khoản Phát triển", 403)
            return deny_page()
        return fn(*a, **kw)

    return wrapper


def wants_json() -> bool:
    """API có trả JSON lỗi 401 thay vì chuyển trang khi người dùng hết phiên."""
    if request.path.startswith("/api/"):
        return True
    accept = request.accept_mimetypes
    return accept["application/json"] >= accept["text/html"]


def deny_json(message: str, code = 401):
    from flask import jsonify

    return jsonify({"ok": False, "error": message, "need_login": True}), code
