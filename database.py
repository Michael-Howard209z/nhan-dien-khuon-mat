"""Lưu trữ thông tin người (họ tên, ngày sinh, lớp) và log nhận diện bằng SQLite.

Ngoài danh sách học sinh còn quản lý: lớp học (+ lịch học theo buổi), tài khoản
người dùng (quản trị / phát triển / giáo viên), điểm danh theo (học sinh, ngày,
buổi) và ghi chú nghỉ.

Điểm danh theo BUỔI (quan trọng):
  * Mỗi lớp khai báo số buổi/ngày (1 hoặc 2) + khung giờ từng buổi trong bảng
    `classes.schedule` (JSON). Học sinh chỉ được điểm danh TỐI ĐA 1 lần/buổi —
    bấm nút nhận diện bao nhiêu lần trong buổi sáng thì sáng vẫn chỉ có 1 điểm
    danh, buổi chiều tính lại từ đầu.
  * Vì vậy khoá chính của `attendance` là (student_id, day, session), không phải
    (student_id, day) như bản cũ.

`recognition_log` chỉ giữ 500 dòng gần nhất nên không dùng để thống kê — dùng
bảng `attendance` (không bao giờ bị cắt).
"""
import json
import shutil
import sqlite3
import threading
from datetime import datetime, time as dtime, timedelta
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "students.db"
FACE_DIR = DATA_DIR / "faces"

# RLock (không phải Lock) vì có nơi gọi hàm khác trong lúc đang giữ khoá
# (ví dụ list_teachers -> teacher_class_names); Lock thường sẽ treo vô hạn ở đó.
_lock = threading.RLock()

# Giới hạn số dòng của nhật ký sự kiện gần nhất (bảng attendance thì không bị cắt)
LOG_KEEP = 500

# --------------------------------------------------------------- lịch học / buổi
SESSION_NAMES = {1: "Buổi sáng", 2: "Buổi chiều"}

# Lịch mặc định cho lớp chưa cấu hình: 2 buổi — check-in trước 12:00 tính buổi
# sáng, từ 12:00 tính buổi chiều. Admin đổi được từng lớp (cột Lớp -> Lịch học).
DEFAULT_SCHEDULE = {
    "sessions": 2,
    "sang_start": "05:00",
    "sang_end": "11:59",
    "chieu_start": "12:00",
    "chieu_end": "23:59",
}


def _conn():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=15)
    conn.row_factory = sqlite3.Row
    return conn


def today_str() -> str:
    return datetime.now().strftime("%Y-%m-%d")


def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _columns(conn, table):
    return {r["name"] for r in conn.execute(f"PRAGMA table_info({table})")}


def _tables(conn):
    return {r["name"] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}


# ================================================================== KHỞI TẠO DB
def init_db():
    with _lock, _conn() as conn:
        conn.execute(
            """CREATE TABLE IF NOT EXISTS students (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                full_name TEXT NOT NULL,
                date_of_birth TEXT NOT NULL,
                class_name TEXT NOT NULL,
                samples INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL
            )"""
        )
        conn.execute(
            """CREATE TABLE IF NOT EXISTS recognition_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                student_id INTEGER,
                full_name TEXT NOT NULL,
                class_name TEXT,
                confidence REAL,
                seen_at TEXT NOT NULL
            )"""
        )
        # --- Lớp học: danh mục chính thức + lịch học theo buổi (JSON trong schedule)
        conn.execute(
            """CREATE TABLE IF NOT EXISTS classes (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE,
                created_at TEXT NOT NULL,
                schedule TEXT NOT NULL DEFAULT ''
            )"""
        )
        # --- Người dùng GỘP: admin / developer / teacher (đăng nhập 1 chỗ)
        conn.execute(
            """CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT NOT NULL UNIQUE,
                password_hash TEXT NOT NULL,
                full_name TEXT NOT NULL,
                role TEXT NOT NULL DEFAULT 'teacher',
                created_at TEXT NOT NULL
            )"""
        )
        # --- Gán lớp cho giáo viên (teacher_id = users.id)
        conn.execute(
            """CREATE TABLE IF NOT EXISTS teacher_classes (
                teacher_id INTEGER NOT NULL,
                class_id INTEGER NOT NULL,
                PRIMARY KEY (teacher_id, class_id)
            )"""
        )
        # --- Điểm danh theo (học sinh, ngày, BUỔI): không bao giờ bị cắt
        conn.execute(
            """CREATE TABLE IF NOT EXISTS attendance (
                student_id INTEGER NOT NULL,
                day TEXT NOT NULL,
                session INTEGER NOT NULL DEFAULT 1,
                first_seen TEXT NOT NULL,
                last_seen TEXT NOT NULL,
                hits INTEGER NOT NULL DEFAULT 1,
                PRIMARY KEY (student_id, day, session)
            )"""
        )
        # --- Ghi chú học sinh nghỉ (lý do)
        conn.execute(
            """CREATE TABLE IF NOT EXISTS absences (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                student_id INTEGER NOT NULL,
                day TEXT NOT NULL,
                reason TEXT NOT NULL DEFAULT '',
                created_by TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL,
                UNIQUE (student_id, day)
            )"""
        )
        conn.commit()

        _migrate_students(conn)
        _migrate_classes(conn)
        _migrate_users(conn)
        _migrate_attendance(conn)
        _seed_classes(conn)
        _backfill_attendance(conn)
        _cleanup_orphans(conn)


def _cleanup_orphans(conn):
    """Xoá bản ghi của học sinh ĐÃ bị xoá (dọn mỗi lần khởi động).

    Thời kỳ cũ xoá học sinh không xoá kèm điểm danh -> bản ghi cũ còn nằm lại
    với student_id đã không còn. Vì id_students dùng AUTOINCREMENT sau khi migrate,
    id mới có thể TRÙNG id mồ côi đó và học sinh mới sẽ thừa lịch sử "ma"
    (điểm danh những ngày họ chưa hề tới trường).
    """
    for table in ("attendance", "absences", "recognition_log"):
        if "student_id" in _columns(conn, table):
            conn.execute(
                f"DELETE FROM {table} WHERE student_id IS NOT NULL"
                " AND student_id NOT IN (SELECT id FROM students)")
    conn.commit()


def _migrate_students(conn):
    """Thêm cột mới cho bản .db cũ; bỏ hẳn cột phụ huynh (chức năng đã xoá)."""
    cols = _columns(conn, "students")
    for col in ("created_at",):
        if col not in cols:
            conn.execute(f"ALTER TABLE students ADD COLUMN {col} TEXT NOT NULL DEFAULT ''")
    if "parent_phone" in cols:
        # SQLite không có DROP COLUMN (bản cũ) -> dựng bảng mới không còn cột này.
        conn.execute(
            """CREATE TABLE students_new (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                full_name TEXT NOT NULL,
                date_of_birth TEXT NOT NULL,
                class_name TEXT NOT NULL,
                samples INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL
            )"""
        )
        conn.execute(
            """INSERT INTO students_new (id, full_name, date_of_birth, class_name,
                                         samples, created_at)
               SELECT id, full_name, date_of_birth, class_name, samples, created_at
                 FROM students"""
        )
        conn.execute("DROP TABLE students")
        conn.execute("ALTER TABLE students_new RENAME TO students")
    conn.commit()


def _migrate_classes(conn):
    cols = _columns(conn, "classes")
    if "schedule" not in cols:
        conn.execute("ALTER TABLE classes ADD COLUMN schedule TEXT NOT NULL DEFAULT ''")
    conn.commit()


def _migrate_users(conn):
    """Bảng teachers cũ (nếu có) -> users với role='teacher', GIỮ NGUYÊN id.

    teacher_classes.teacher_id trỏ theo id nên phải giữ nguyên id khi chép sang.
    """
    if "teachers" not in _tables(conn):
        return
    rows = conn.execute(
        "SELECT id, username, password_hash, full_name, created_at FROM teachers"
    ).fetchall()
    for r in rows:
        conn.execute(
            """INSERT OR IGNORE INTO users (id, username, password_hash, full_name,
                                            role, created_at)
               VALUES (?, ?, ?, ?, 'teacher', ?)""",
            (r["id"], r["username"], r["password_hash"], r["full_name"], r["created_at"]),
        )
    conn.execute("DROP TABLE teachers")
    conn.commit()


def _migrate_attendance(conn):
    """Thêm cột session + đổi khoá chính sang (student_id, day, session)."""
    cols = _columns(conn, "attendance")
    if "session" in cols:
        return
    conn.execute(
        """CREATE TABLE attendance_new (
            student_id INTEGER NOT NULL,
            day TEXT NOT NULL,
            session INTEGER NOT NULL DEFAULT 1,
            first_seen TEXT NOT NULL,
            last_seen TEXT NOT NULL,
            hits INTEGER NOT NULL DEFAULT 1,
            PRIMARY KEY (student_id, day, session)
        )"""
    )
    conn.execute(
        """INSERT INTO attendance_new (student_id, day, session, first_seen, last_seen, hits)
           SELECT student_id, day, 1, first_seen, last_seen, hits FROM attendance"""
    )
    conn.execute("DROP TABLE attendance")
    conn.execute("ALTER TABLE attendance_new RENAME TO attendance")
    conn.commit()


def _seed_classes(conn):
    """Đưa lớp của học sinh đang có vào danh mục lớp (nếu admin chưa tạo)."""
    rows = conn.execute(
        "SELECT DISTINCT class_name FROM students WHERE class_name <> ''"
    ).fetchall()
    for r in rows:
        name = (r["class_name"] or "").strip()
        if name:
            conn.execute(
                "INSERT OR IGNORE INTO classes (name, created_at) VALUES (?, ?)",
                (name, now_str()),
            )
    conn.commit()


def _backfill_attendance(conn):
    """Dựng lịch điểm danh từ nhật ký cũ, để không mất số ngày đã đi học.

    Dữ liệu cũ không biết buổi nào -> gán buổi 1 (mặc định).
    """
    conn.execute(
        """INSERT OR IGNORE INTO attendance (student_id, day, session, first_seen,
                                             last_seen, hits)
           SELECT student_id,
                  substr(seen_at, 1, 10),
                  1,
                  MIN(seen_at),
                  MAX(seen_at),
                  COUNT(*)
             FROM recognition_log
            WHERE student_id IS NOT NULL
            GROUP BY student_id, substr(seen_at, 1, 10)"""
    )
    conn.commit()


def normalize_class(name: str) -> str:
    """Chuẩn hoá tên lớp: bỏ khoảng trắng thừa, viết hoa (vd ' 12 a1 ' -> '12A1')."""
    parts = (name or "").strip().upper().split()
    return "".join(parts)


# ============================================================ LỊCH HỌC THEO BUỔI
def _parse_time(value, fallback):
    try:
        hh, mm = str(value or "").strip().split(":")
        return dtime(int(hh) % 24, int(mm) % 60)
    except (ValueError, AttributeError, TypeError):
        return fallback


def get_schedule(class_name: str) -> dict:
    """Lịch học của lớp: {'sessions': 1|2, khung giờ...}. Lớp chưa khai -> mặc định."""
    key = normalize_class(class_name)
    raw = ""
    if key:
        with _lock, _conn() as conn:
            row = conn.execute(
                "SELECT schedule FROM classes WHERE name = ?", (key,)
            ).fetchone()
            raw = (row["schedule"] if row else "") or ""
    data = dict(DEFAULT_SCHEDULE)
    if raw:
        try:
            loaded = json.loads(raw)
            if isinstance(loaded, dict):
                data.update({k: v for k, v in loaded.items() if v not in (None, "")})
        except (ValueError, TypeError):
            pass
    try:
        data["sessions"] = 1 if int(data.get("sessions", 2)) <= 1 else 2
    except (TypeError, ValueError):
        data["sessions"] = 2
    return data


def set_schedule(class_name: str, data: dict) -> dict:
    """Lưu lịch học của lớp. Ném ValueError nếu sai dữ liệu."""
    key = normalize_class(class_name)
    if not key:
        raise ValueError("Thiếu tên lớp")
    schedule = get_schedule(key)
    if "sessions" in data and data["sessions"] not in (None, ""):
        try:
            sessions = int(data["sessions"])
        except (TypeError, ValueError):
            raise ValueError("Số buổi phải là 1 hoặc 2")
        if sessions not in (1, 2):
            raise ValueError("Số buổi phải là 1 hoặc 2")
        schedule["sessions"] = sessions
    for field in ("sang_start", "sang_end", "chieu_start", "chieu_end"):
        if data.get(field):
            v = str(data[field]).strip()
            if len(v) < 4 or ":" not in v:
                raise ValueError(f"Giờ {field} sai định dạng (HH:MM, vd 07:00)")
            schedule[field] = v
    if _parse_time(schedule["sang_start"], dtime(5, 0)) >= \
       _parse_time(schedule["sang_end"], dtime(11, 59)):
        raise ValueError("Giờ buổi sáng phải trước giờ kết thúc buổi sáng")
    if _parse_time(schedule["chieu_start"], dtime(12, 0)) >= \
       _parse_time(schedule["chieu_end"], dtime(23, 59)):
        raise ValueError("Giờ buổi chiều phải trước giờ kết thúc buổi chiều")
    with _lock, _conn() as conn:
        cur = conn.execute(
            "UPDATE classes SET schedule = ? WHERE name = ?",
            (json.dumps(schedule, ensure_ascii=False), key),
        )
        conn.commit()
        if cur.rowcount == 0:
            raise ValueError(f"Không tìm thấy lớp {key}")
    return schedule


def current_session(class_name: str, when=None) -> int:
    """Buổi tương ứng với mốc thời gian hiện tại (mặc định: bây giờ).

    Lớp 1 buổi -> luôn 1. Lớp 2 buổi: trong khung chiều -> 2, còn lại -> 1
    (trước khi tới khung sáng cũng tính buổi sáng — học sinh tới sớm vẫn điểm
    danh được).
    """
    sched = get_schedule(class_name)
    if sched["sessions"] < 2:
        return 1
    now = when or datetime.now()
    t = now.time()
    if _parse_time(sched["chieu_start"], dtime(12, 0)) <= t <= \
       _parse_time(sched["chieu_end"], dtime(23, 59)):
        return 2
    return 1


# ================================================================= HỌC SINH
def create_student(full_name: str, date_of_birth: str, class_name: str) -> int:
    full_name = (full_name or "").strip()
    class_name = normalize_class(class_name)
    with _lock, _conn() as conn:
        cur = conn.execute(
            "INSERT INTO students (full_name, date_of_birth, class_name, samples,"
            " created_at) VALUES (?, ?, ?, 0, ?)",
            (full_name, date_of_birth, class_name, now_str()),
        )
        conn.commit()
        return int(cur.lastrowid)


def get_student(student_id: int):
    with _lock, _conn() as conn:
        row = conn.execute("SELECT * FROM students WHERE id = ?", (student_id,)).fetchone()
        return dict(row) if row else None


def list_students(class_name: str = None, query: str = None):
    """Danh sách học sinh, lọc theo lớp và/hoặc tên/mã."""
    sql = "SELECT * FROM students WHERE 1=1"
    args = []
    if class_name:
        sql += " AND class_name = ?"
        args.append(normalize_class(class_name))
    if query:
        sql += " AND (full_name LIKE ? OR CAST(id AS TEXT) = ?)"
        like = f"%{query.strip()}%"
        args += [like, query.strip()]
    sql += " ORDER BY class_name, full_name"
    with _lock, _conn() as conn:
        return [dict(r) for r in conn.execute(sql, args).fetchall()]


def count_students() -> int:
    with _lock, _conn() as conn:
        row = conn.execute("SELECT COUNT(*) AS n FROM students").fetchone()
        return int(row["n"])


def update_student(student_id: int, full_name=None, date_of_birth=None,
                   class_name=None) -> bool:
    current = get_student(student_id)
    if not current:
        return False
    new_class = normalize_class(class_name) if class_name is not None else current["class_name"]
    with _lock, _conn() as conn:
        conn.execute(
            "UPDATE students SET full_name = ?, date_of_birth = ?, class_name = ?"
            " WHERE id = ?",
            (
                full_name if full_name is not None else current["full_name"],
                date_of_birth if date_of_birth is not None else current["date_of_birth"],
                new_class,
                student_id,
            ),
        )
        conn.commit()
    return True


def delete_student(student_id: int) -> bool:
    with _lock, _conn() as conn:
        cur = conn.execute("DELETE FROM students WHERE id = ?", (student_id,))
        conn.execute("DELETE FROM attendance WHERE student_id = ?", (student_id,))
        conn.execute("DELETE FROM absences WHERE student_id = ?", (student_id,))
        conn.commit()
        deleted = cur.rowcount > 0
    if deleted:
        folder = FACE_DIR / str(student_id)
        if folder.exists():
            shutil.rmtree(folder, ignore_errors=True)
    return deleted


def set_sample_count(student_id: int, count: int):
    with _lock, _conn() as conn:
        conn.execute("UPDATE students SET samples = ? WHERE id = ?", (count, student_id))
        conn.commit()


# ================================================================ ĐIỂM DANH
def add_log(student_id, full_name, class_name, confidence, when=None):
    """Ghi nhật ký + điểm danh 1 lần cho buổi hiện tại.

    Trả {'session': 1|2, 'first': bool} — `first` = lần điểm danh ĐẦU TIÊN của
    buổi này (False nghĩa là học sinh đã điểm danh buổi này rồi: bấm nút again
    vẫn ghi nhận nhưng KHÔNG tạo lần điểm danh mới).
    """
    moment = when or datetime.now()
    seen = moment.strftime("%Y-%m-%d %H:%M:%S")
    day = seen[:10]
    session = current_session(class_name, moment)
    with _lock, _conn() as conn:
        conn.execute(
            "INSERT INTO recognition_log (student_id, full_name, class_name, confidence, seen_at)"
            " VALUES (?, ?, ?, ?, ?)",
            (student_id, full_name, class_name, confidence, seen),
        )
        conn.execute(
            "DELETE FROM recognition_log WHERE id NOT IN"
            " (SELECT id FROM recognition_log ORDER BY id DESC LIMIT ?)",
            (LOG_KEEP,),
        )
        first = True
        if student_id is not None:
            existing = conn.execute(
                "SELECT 1 FROM attendance WHERE student_id = ? AND day = ? AND session = ?",
                (student_id, day, session),
            ).fetchone()
            first = existing is None
            if first:
                conn.execute(
                    """INSERT INTO attendance (student_id, day, session, first_seen,
                                               last_seen, hits)
                           VALUES (?, ?, ?, ?, ?, 1)""",
                    (student_id, day, session, seen, seen),
                )
            else:
                conn.execute(
                    """UPDATE attendance SET last_seen = ?, hits = hits + 1
                        WHERE student_id = ? AND day = ? AND session = ?""",
                    (seen, student_id, day, session),
                )
        conn.commit()
    return {"session": session, "first": first, "day": day}


def has_attendance(student_id: int, day: str, session: int = None) -> bool:
    """Đã điểm danh (buổi `session`, hoặc bất kỳ buổi nào nếu để None) trong ngày."""
    with _lock, _conn() as conn:
        if session is None:
            row = conn.execute(
                "SELECT 1 FROM attendance WHERE student_id = ? AND day = ?",
                (student_id, day),
            ).fetchone()
        else:
            row = conn.execute(
                "SELECT 1 FROM attendance WHERE student_id = ? AND day = ? AND session = ?",
                (student_id, day, session),
            ).fetchone()
        return row is not None


def has_attendance_today(student_id: int) -> bool:
    """Đã được điểm danh hôm nay chưa (bất kỳ buổi nào)."""
    return has_attendance(student_id, today_str())


def recent_log(limit: int = 30):
    with _lock, _conn() as conn:
        rows = conn.execute(
            "SELECT * FROM recognition_log ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in rows]


# ====================================================================== LỚP HỌC
def _count_present(conn, day, class_name=None):
    sql = """SELECT COUNT(DISTINCT a.student_id) AS n
               FROM attendance a JOIN students s ON s.id = a.student_id
              WHERE a.day = ?"""
    args = [day]
    if class_name:
        sql += " AND s.class_name = ?"
        args.append(normalize_class(class_name))
    return int(conn.execute(sql, args).fetchone()["n"])


def list_classes():
    """Danh mục lớp kèm số học sinh và số người có mặt hôm nay (≥1 buổi)."""
    today = today_str()
    with _lock, _conn() as conn:
        rows = conn.execute(
            """SELECT c.id, c.name, c.schedule,
                      COUNT(s.id) AS total,
                      (SELECT COUNT(*) FROM attendance a
                         JOIN students s2 ON s2.id = a.student_id
                        WHERE a.day = ? AND s2.class_name = c.name) AS present
                 FROM classes c
                 LEFT JOIN students s ON s.class_name = c.name
                GROUP BY c.id, c.name
                ORDER BY c.name""",
            (today,),
        ).fetchall()
        out = []
        for r in rows:
            item = dict(r)
            item["schedule"] = get_schedule(item["name"])
            out.append(item)
        return out


def get_class(name: str):
    key = normalize_class(name)
    with _lock, _conn() as conn:
        row = conn.execute("SELECT * FROM classes WHERE name = ?", (key,)).fetchone()
        if not row:
            return None
        item = dict(row)
        item["schedule"] = get_schedule(item["name"])
        return item


def add_class(name: str):
    """Thêm lớp. Trả dict {'id':..} ; ném ValueError nếu tên sai hoặc trùng."""
    key = normalize_class(name)
    if not key or len(key) > 20:
        raise ValueError("Tên lớp không hợp lệ (1–20 ký tự, vd 12A1)")
    with _lock, _conn() as conn:
        if conn.execute("SELECT 1 FROM classes WHERE name = ?", (key,)).fetchone():
            raise ValueError(f"Lớp {key} đã tồn tại")
        cur = conn.execute(
            "INSERT INTO classes (name, created_at) VALUES (?, ?)", (key, now_str())
        )
        conn.commit()
        return {"id": int(cur.lastrowid), "name": key}


def rename_class(name: str, new_name: str):
    """Đổi tên lớp (sửa lớp): tên mới + mọi học sinh của lớp đều được cập nhật."""
    key = normalize_class(name)
    target = normalize_class(new_name)
    if not target or len(target) > 20:
        raise ValueError("Tên lớp không hợp lệ (1–20 ký tự, vd 12A1)")
    if key == target:
        return {"id": None, "name": target}
    with _lock, _conn() as conn:
        row = conn.execute("SELECT id FROM classes WHERE name = ?", (key,)).fetchone()
        if not row:
            raise ValueError(f"Không tìm thấy lớp {key}")
        if conn.execute("SELECT 1 FROM classes WHERE name = ?", (target,)).fetchone():
            raise ValueError(f"Lớp {target} đã tồn tại")
        conn.execute("UPDATE classes SET name = ? WHERE id = ?", (target, row["id"]))
        conn.execute("UPDATE students SET class_name = ? WHERE class_name = ?",
                     (target, key))
        conn.commit()
        return {"id": int(row["id"]), "name": target}


def delete_class(name: str, move_to: str = None):
    """Xoá lớp.

    Lớp còn học sinh thì phải chuyển học sinh sang lớp khác (``move_to``), hoặc
    dừng lại và báo rõ — không xoá làm mất dữ liệu học sinh.
    """
    key = normalize_class(name)
    with _lock, _conn() as conn:
        row = conn.execute("SELECT * FROM classes WHERE name = ?", (key,)).fetchone()
        if not row:
            raise ValueError(f"Không tìm thấy lớp {key}")
        cid = int(row["id"])
        total = conn.execute(
            "SELECT COUNT(*) AS n FROM students WHERE class_name = ?", (key,)
        ).fetchone()["n"]
        moved = 0
        if total:
            target = normalize_class(move_to or "")
            if not target:
                raise ValueError(
                    f"Lớp {key} còn {total} học sinh — hãy chọn lớp để chuyển học sinh sang"
                )
            if target == key:
                raise ValueError("Lớp đích không được trùng lớp đang xoá")
            conn.execute(
                "INSERT OR IGNORE INTO classes (name, created_at) VALUES (?, ?)",
                (target, now_str()),
            )
            conn.execute("UPDATE students SET class_name = ? WHERE class_name = ?",
                         (target, key))
            moved = total
        conn.execute("DELETE FROM teacher_classes WHERE class_id = ?", (cid,))
        conn.execute("DELETE FROM classes WHERE id = ?", (cid,))
        conn.commit()
        return {"name": key, "moved": moved}


# =========================================================== TRUY VẤN ĐIỂM DANH
def attendance_summary(student_id: int):
    """Số ngày đã đi học + chi tiết từng ngày (mới nhất trước).

    Mỗi dòng: {'day', 'sessions': [1,2...], 'first_seen', 'last_seen', 'hits'}
    (gộp các buổi trong ngày).
    """
    with _lock, _conn() as conn:
        rows = conn.execute(
            """SELECT day, MIN(first_seen) AS first_seen, MAX(last_seen) AS last_seen,
                      SUM(hits) AS hits, GROUP_CONCAT(session) AS sessions
                 FROM attendance WHERE student_id = ?
                GROUP BY day ORDER BY day DESC""",
            (student_id,),
        ).fetchall()
    detail = []
    for r in rows:
        item = dict(r)
        item["sessions"] = sorted(int(s) for s in str(item.get("sessions") or "").split(",") if s)
        detail.append(item)
    return {"days": len(detail), "detail": detail}


def attendance_in_range(student_id: int, start: str, end: str):
    with _lock, _conn() as conn:
        rows = conn.execute(
            """SELECT day, MIN(first_seen) AS first_seen, MAX(last_seen) AS last_seen,
                      SUM(hits) AS hits, GROUP_CONCAT(session) AS sessions
                 FROM attendance
                WHERE student_id = ? AND day BETWEEN ? AND ?
                GROUP BY day ORDER BY day DESC""",
            (student_id, start, end),
        ).fetchall()
    out = []
    for r in rows:
        item = dict(r)
        item["sessions"] = sorted(int(s) for s in str(item.get("sessions") or "").split(",") if s)
        out.append(item)
    return out


def attendance_matrix(student_ids, start: str, end: str):
    """{student_id: {ngày: {'sessions','first_seen','last_seen','hits'}}}."""
    ids = list(student_ids)
    out = {i: {} for i in ids}
    if not ids:
        return out
    q = ",".join("?" * len(ids))
    with _lock, _conn() as conn:
        rows = conn.execute(
            f"""SELECT student_id, day, MIN(first_seen) AS first_seen,
                       MAX(last_seen) AS last_seen, SUM(hits) AS hits,
                       GROUP_CONCAT(session) AS sessions
                  FROM attendance
                 WHERE student_id IN ({q}) AND day BETWEEN ? AND ?
                 GROUP BY student_id, day""",
            (*ids, start, end),
        ).fetchall()
    for r in rows:
        out[r["student_id"]][r["day"]] = {
            "first_seen": r["first_seen"],
            "last_seen": r["last_seen"],
            "hits": r["hits"],
            "sessions": sorted(int(s) for s in str(r["sessions"] or "").split(",") if s),
        }
    return out


def present_today(class_name: str = None):
    """Danh sách học sinh đã điểm danh hôm nay (≥1 buổi) + buổi đã điểm."""
    today = today_str()
    sql = (
        """SELECT s.id, s.full_name, s.class_name,
                  MIN(a.first_seen) AS first_seen, MAX(a.last_seen) AS last_seen,
                  SUM(a.hits) AS hits, GROUP_CONCAT(a.session) AS sessions
             FROM attendance a JOIN students s ON s.id = a.student_id
            WHERE a.day = ?"""
    )
    args = [today]
    if class_name:
        sql += " AND s.class_name = ?"
        args.append(normalize_class(class_name))
    sql += " GROUP BY s.id ORDER BY s.class_name, s.full_name"
    with _lock, _conn() as conn:
        out = []
        for r in conn.execute(sql, args).fetchall():
            item = dict(r)
            item["sessions"] = sorted(
                int(x) for x in str(item.get("sessions") or "").split(",") if x
            )
            out.append(item)
        return out


def recent_attendance_days(days: int = 7):
    """[{ngày, số học sinh có mặt}] cho biểu đồ cột."""
    start = (datetime.now() - timedelta(days=days - 1)).strftime("%Y-%m-%d")
    with _lock, _conn() as conn:
        rows = conn.execute(
            """SELECT day, COUNT(DISTINCT student_id) AS n FROM attendance
                WHERE day >= ? GROUP BY day ORDER BY day""",
            (start,),
        ).fetchall()
    return [dict(r) for r in rows]


# ======================================================== GHI CHÚ VẮNG / NGHỈ
def set_absence(student_id: int, day: str, reason: str = "", created_by: str = ""):
    """Ghi hoặc sửa ghi chú 'học sinh không điểm danh ngày này'."""
    day = (day or "").strip()
    if len(day) != 10:
        raise ValueError("Ngày không hợp lệ (định dạng YYYY-MM-DD)")
    reason = (reason or "").strip()
    if not reason:
        raise ValueError("Vui lòng nhập lý do nghỉ")
    with _lock, _conn() as conn:
        if not conn.execute("SELECT 1 FROM students WHERE id = ?", (student_id,)).fetchone():
            raise ValueError("Không tìm thấy học sinh")
        conn.execute(
            """INSERT INTO absences (student_id, day, reason, created_by, created_at)
                    VALUES (?, ?, ?, ?, ?)
               ON CONFLICT(student_id, day) DO UPDATE
                    SET reason = excluded.reason,
                        created_by = excluded.created_by,
                        created_at = excluded.created_at""",
            (student_id, day, reason, created_by or "", now_str()),
        )
        conn.commit()


def clear_absence(student_id: int, day: str):
    with _lock, _conn() as conn:
        conn.execute(
            "DELETE FROM absences WHERE student_id = ? AND day = ?", (student_id, day)
        )
        conn.commit()


def absence_map(student_ids, start: str, end: str):
    """{(student_id, ngày): {'reason','created_by'}} trong khoảng ngày."""
    ids = list(student_ids)
    out = {}
    if not ids:
        return out
    q = ",".join("?" * len(ids))
    with _lock, _conn() as conn:
        rows = conn.execute(
            f"""SELECT student_id, day, reason, created_by FROM absences
                 WHERE student_id IN ({q}) AND day BETWEEN ? AND ?""",
            (*ids, start, end),
        ).fetchall()
    for r in rows:
        out[(r["student_id"], r["day"])] = {
            "reason": r["reason"],
            "created_by": r["created_by"],
        }
    return out


# ================================================= BÁO CÁO NGÀY / TUẦN / THÁNG
def _session_states(day: str, students, schedule, when=None, absences=None):
    """Trạng thái từng buổi cho danh sách `students` trong ngày `day`.

    Trả list dict: {'sessions': {1: 'present'|'absent'|'future', 2: ...}, ...}
    'future' = buổi chưa tới (không tính vắng).
    """
    now = when or datetime.now()
    today = today_str()
    n_sessions = int(schedule.get("sessions", 2))
    bounds = {
        1: (_parse_time(schedule.get("sang_start"), dtime(5, 0)),
            _parse_time(schedule.get("sang_end"), dtime(11, 59))),
        2: (_parse_time(schedule.get("chieu_start"), dtime(12, 0)),
            _parse_time(schedule.get("chieu_end"), dtime(23, 59))),
    }
    ids = [s["id"] for s in students]
    with _lock, _conn() as conn:
        rows = conn.execute(
            f"""SELECT student_id, session, first_seen, last_seen, hits FROM attendance
                 WHERE day = ? AND student_id IN ({','.join('?' * len(ids)) if ids else "''"})""",
            (day, *ids),
        ).fetchall() if ids else []
        present = {r["student_id"]: {} for r in rows}
        for r in rows:
            present[r["student_id"]][int(r["session"])] = dict(r)
    out = []
    for s in students:
        state = {}
        for ses in (1, 2):
            if ses > n_sessions:
                state[ses] = None          # lớp không có buổi này
                continue
            if ses in present.get(s["id"], {}):
                state[ses] = "present"
                continue
            start, _end = bounds[ses]
            if day > today or (day == today and now.time() < start):
                state[ses] = "future"
            else:
                state[ses] = "absent"
        item = dict(s)
        item["session_state"] = state
        item["sessions_present"] = sum(1 for v in state.values() if v == "present")
        item["detail"] = present.get(s["id"], {})
        item["absence"] = (absences or {}).get((s["id"], day))
        item["status"] = (
            "full" if n_sessions and item["sessions_present"] == n_sessions
            else "partial" if item["sessions_present"]
            else ("leave" if item["absence"] else "absent")
        )
        out.append(item)
    return out


def day_report(class_name: str, day: str):
    """Danh sách 1 ngày của 1 lớp: từng buổi present/absent/future + ghi chú nghỉ."""
    students = list_students(class_name=class_name)
    schedule = get_schedule(class_name)
    ids = [s["id"] for s in students]
    absences = absence_map(ids, day, day)
    rows = _session_states(day, students, schedule, absences=absences)
    return {"schedule": schedule, "rows": rows, "day": day,
            "class_name": normalize_class(class_name)}


def range_report(class_name: str, start: str, end: str):
    """Bảng tổng hợp cả khoảng ngày: {students, days, matrix, expected}.

    matrix[sid][day] = {'sessions': [..], 'state': 'full'|'partial'|'absent'|
    'leave'|'future'} — dựng theo lịch học của lớp (buổi chưa tới = future).
    """
    students = list_students(class_name=class_name)
    schedule = get_schedule(class_name)
    n_sessions = int(schedule.get("sessions", 2))
    days = []
    d = datetime.strptime(start, "%Y-%m-%d").date()
    last = datetime.strptime(end, "%Y-%m-%d").date()
    while d <= last:
        days.append(d.strftime("%Y-%m-%d"))
        d += timedelta(days=1)
    ids = [s["id"] for s in students]
    att = attendance_matrix(ids, start, end)
    absences = absence_map(ids, start, end)
    today = today_str()
    now = datetime.now()
    matrix = {}
    for sid in ids:
        matrix[sid] = {}
        for day in days:
            rec = att.get(sid, {}).get(day)
            if rec:
                got = len(rec.get("sessions") or [1])
                state = "full" if got >= n_sessions else "partial"
            elif (sid, day) in absences:
                state = "leave"
            elif day > today:
                state = "future"
            elif day == today:
                state = "pending"      # hôm nay chưa điểm danh — chưa vội tính vắng
            else:
                state = "absent"
            matrix[sid][day] = {
                "sessions": (rec or {}).get("sessions", []),
                "state": state,
                "first_seen": (rec or {}).get("first_seen", ""),
                "last_seen": (rec or {}).get("last_seen", ""),
            }
    # Tổng hợp theo học sinh
    totals = {}
    for sid in ids:
        days_full = sum(1 for d in days if matrix[sid][d]["state"] == "full")
        days_partial = sum(1 for d in days if matrix[sid][d]["state"] == "partial")
        days_leave = sum(1 for d in days if matrix[sid][d]["state"] == "leave")
        days_absent = sum(1 for d in days if matrix[sid][d]["state"] == "absent")
        days_pending = sum(1 for d in days if matrix[sid][d]["state"] == "pending")
        totals[sid] = {"full": days_full, "partial": days_partial,
                       "leave": days_leave, "absent": days_absent,
                       "pending": days_pending,
                       "attended": days_full + days_partial}
    return {"students": students, "days": days, "matrix": matrix,
            "totals": totals, "schedule": schedule, "absences": absences,
            "start": start, "end": end, "class_name": normalize_class(class_name)}


# ================================================================ NGƯỜI DÙNG
def _user_row(conn, username: str):
    return conn.execute(
        "SELECT * FROM users WHERE username = ?", ((username or "").strip().lower(),)
    ).fetchone()


def get_user(username: str):
    """Tài khoản bất kỳ vai trò nào theo tên đăng nhập (None nếu chưa có)."""
    with _lock, _conn() as conn:
        row = _user_row(conn, username)
        return dict(row) if row else None


def ensure_user(username: str, password_hash: str, full_name: str, role: str):
    """Tạo tài khoản nếu chưa có (dùng seed admin/developer khi khởi động). Trả id."""
    username = (username or "").strip().lower()
    if not username:
        raise ValueError("Thiếu tên đăng nhập")
    with _lock, _conn() as conn:
        row = _user_row(conn, username)
        if row:
            return int(row["id"])
        cur = conn.execute(
            "INSERT INTO users (username, password_hash, full_name, role, created_at)"
            " VALUES (?, ?, ?, ?, ?)",
            (username, password_hash, (full_name or username).strip(),
             role, now_str()),
        )
        conn.commit()
        return int(cur.lastrowid)


def update_user_password(username: str, password_hash: str) -> bool:
    with _lock, _conn() as conn:
        cur = conn.execute(
            "UPDATE users SET password_hash = ? WHERE username = ?",
            (password_hash, (username or "").strip().lower()),
        )
        conn.commit()
        return cur.rowcount > 0


def list_users(role: str = None):
    with _lock, _conn() as conn:
        if role:
            rows = conn.execute(
                "SELECT * FROM users WHERE role = ? ORDER BY id", (role,)
            ).fetchall()
        else:
            rows = conn.execute("SELECT * FROM users ORDER BY id").fetchall()
        out = []
        for r in rows:
            item = dict(r)
            item.pop("password_hash", None)
            if item["role"] == "teacher":
                item["classes"] = teacher_class_names(int(item["id"]))
            out.append(item)
        return out


def delete_user(user_id: int) -> bool:
    with _lock, _conn() as conn:
        row = conn.execute("SELECT role FROM users WHERE id = ?", (user_id,)).fetchone()
        if not row:
            return False
        if row["role"] in ("admin", "developer"):
            raise ValueError("Không thể xoá tài khoản quản trị/phát triển")
        conn.execute("DELETE FROM teacher_classes WHERE teacher_id = ?", (user_id,))
        cur = conn.execute("DELETE FROM users WHERE id = ?", (user_id,))
        conn.commit()
        return cur.rowcount > 0


# ------------------------------------------------------------- giáo viên (role)
def create_teacher(username: str, password_hash: str, full_name: str, class_names):
    """Tạo tài khoản giáo viên và gán các lớp được dạy."""
    username = (username or "").strip().lower()
    full_name = (full_name or "").strip()
    if not username or not full_name:
        raise ValueError("Thiếu tên đăng nhập hoặc họ tên")
    if username in ("admin", "developer"):
        raise ValueError(f"'{username}' là tên đăng nhập dành cho hệ thống")
    names = [normalize_class(c) for c in (class_names or []) if normalize_class(c)]
    with _lock, _conn() as conn:
        if _user_row(conn, username):
            raise ValueError(f"Tên đăng nhập '{username}' đã có người dùng")
        missing = [
            n for n in names
            if not conn.execute("SELECT 1 FROM classes WHERE name = ?", (n,)).fetchone()
        ]
        if missing:
            raise ValueError(
                "Lớp chưa có trong danh mục: " + ", ".join(missing)
                + ". Nhờ quản trị viên thêm lớp trước."
            )
        cur = conn.execute(
            "INSERT INTO users (username, password_hash, full_name, role, created_at)"
            " VALUES (?, ?, ?, 'teacher', ?)",
            (username, password_hash, full_name, now_str()),
        )
        tid = int(cur.lastrowid)
        for n in names:
            cid = conn.execute("SELECT id FROM classes WHERE name = ?", (n,)).fetchone()["id"]
            conn.execute(
                "INSERT OR IGNORE INTO teacher_classes (teacher_id, class_id) VALUES (?, ?)",
                (tid, cid),
            )
        conn.commit()
        return tid


def get_teacher(username: str):
    """Tài khoản giáo viên theo username (None nếu không tồn tại hoặc không phải GV)."""
    user = get_user(username)
    if user and user.get("role") == "teacher":
        return user
    return None


def get_teacher_by_id(teacher_id: int):
    with _lock, _conn() as conn:
        row = conn.execute(
            "SELECT * FROM users WHERE id = ? AND role = 'teacher'", (int(teacher_id),)
        ).fetchone()
        return dict(row) if row else None


def teacher_class_names(teacher_id: int):
    with _lock, _conn() as conn:
        rows = conn.execute(
            """SELECT c.name FROM teacher_classes tc JOIN classes c ON c.id = tc.class_id
                WHERE tc.teacher_id = ? ORDER BY c.name""",
            (teacher_id,),
        ).fetchall()
        return [r["name"] for r in rows]


def set_teacher_classes(teacher_id: int, class_names):
    """Đổi danh sách lớp một giáo viên được dạy."""
    names = [normalize_class(c) for c in (class_names or []) if normalize_class(c)]
    with _lock, _conn() as conn:
        if not conn.execute(
            "SELECT 1 FROM users WHERE id = ? AND role = 'teacher'", (int(teacher_id),)
        ).fetchone():
            raise ValueError("Không tìm thấy giáo viên")
        conn.execute("DELETE FROM teacher_classes WHERE teacher_id = ?", (int(teacher_id),))
        for n in names:
            row = conn.execute("SELECT id FROM classes WHERE name = ?", (n,)).fetchone()
            if not row:
                raise ValueError(f"Lớp {n} chưa có trong danh mục")
            conn.execute(
                "INSERT OR IGNORE INTO teacher_classes (teacher_id, class_id) VALUES (?, ?)",
                (int(teacher_id), int(row["id"])),
            )
        conn.commit()
    return names


def list_teachers():
    out = []
    for item in list_users(role="teacher"):
        item["classes"] = teacher_class_names(int(item["id"]))
        out.append(item)
    return out


def delete_teacher(teacher_id: int) -> bool:
    try:
        return delete_user(int(teacher_id))
    except ValueError:
        return False


# ================================================================= TỔNG HỢP
def overview():
    """Số liệu cho trang quản trị: sĩ số lớp, điểm danh hôm nay, 7 ngày gần nhất."""
    today = today_str()
    with _lock, _conn() as conn:
        total_students = conn.execute("SELECT COUNT(*) AS n FROM students").fetchone()["n"]
        total_classes = conn.execute("SELECT COUNT(*) AS n FROM classes").fetchone()["n"]
        total_teachers = conn.execute(
            "SELECT COUNT(*) AS n FROM users WHERE role = 'teacher'"
        ).fetchone()["n"]
        present_today = conn.execute(
            "SELECT COUNT(DISTINCT student_id) AS n FROM attendance WHERE day = ?", (today,)
        ).fetchone()["n"]
    classes = list_classes()
    return {
        "total_students": int(total_students),
        "total_classes": int(total_classes),
        "total_teachers": int(total_teachers),
        "present_today": int(present_today),
        "absent_today": max(0, int(total_students) - int(present_today)),
        "classes": classes,
        "week": recent_attendance_days(7),
        "today": today,
    }
