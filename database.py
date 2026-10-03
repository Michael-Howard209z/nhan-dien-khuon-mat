"""Lưu trữ thông tin người (họ tên, ngày sinh, lớp) và log nhận diện bằng SQLite."""
import shutil
import sqlite3
import threading
from datetime import datetime
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "students.db"
FACE_DIR = DATA_DIR / "faces"

_lock = threading.Lock()


def _conn():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=15)
    conn.row_factory = sqlite3.Row
    return conn


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
        conn.commit()


def now_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def create_student(full_name: str, date_of_birth: str, class_name: str) -> int:
    with _lock, _conn() as conn:
        cur = conn.execute(
            "INSERT INTO students (full_name, date_of_birth, class_name, samples, created_at)"
            " VALUES (?, ?, ?, 0, ?)",
            (full_name, date_of_birth, class_name, now_str()),
        )
        conn.commit()
        return int(cur.lastrowid)


def get_student(student_id: int):
    with _lock, _conn() as conn:
        row = conn.execute("SELECT * FROM students WHERE id = ?", (student_id,)).fetchone()
        return dict(row) if row else None


def list_students():
    with _lock, _conn() as conn:
        rows = conn.execute("SELECT * FROM students ORDER BY id DESC").fetchall()
        return [dict(r) for r in rows]


def count_students() -> int:
    with _lock, _conn() as conn:
        row = conn.execute("SELECT COUNT(*) AS n FROM students").fetchone()
        return int(row["n"])


def update_student(student_id: int, full_name=None, date_of_birth=None, class_name=None) -> bool:
    current = get_student(student_id)
    if not current:
        return False
    with _lock, _conn() as conn:
        conn.execute(
            "UPDATE students SET full_name = ?, date_of_birth = ?, class_name = ? WHERE id = ?",
            (
                full_name if full_name is not None else current["full_name"],
                date_of_birth if date_of_birth is not None else current["date_of_birth"],
                class_name if class_name is not None else current["class_name"],
                student_id,
            ),
        )
        conn.commit()
    return True


def delete_student(student_id: int) -> bool:
    with _lock, _conn() as conn:
        cur = conn.execute("DELETE FROM students WHERE id = ?", (student_id,))
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


def add_log(student_id, full_name, class_name, confidence):
    with _lock, _conn() as conn:
        conn.execute(
            "INSERT INTO recognition_log (student_id, full_name, class_name, confidence, seen_at)"
            " VALUES (?, ?, ?, ?, ?)",
            (student_id, full_name, class_name, confidence, now_str()),
        )
        conn.execute(
            "DELETE FROM recognition_log WHERE id NOT IN"
            " (SELECT id FROM recognition_log ORDER BY id DESC LIMIT 500)"
        )
        conn.commit()


def recent_log(limit: int = 30):
    with _lock, _conn() as conn:
        rows = conn.execute(
            "SELECT * FROM recognition_log ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in rows]
