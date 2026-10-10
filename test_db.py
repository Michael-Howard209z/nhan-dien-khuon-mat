"""Kiểm tra tầng dữ liệu: lớp + lịch học theo buổi, điểm danh theo buổi,
vắng/nghỉ, giáo viên/tài khoản, báo cáo, tổng hợp. (Không cần server.)"""
import shutil
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(HERE))

import database as db  # noqa: E402

TMP = HERE / "data" / "_test_db"
if TMP.exists():
    shutil.rmtree(TMP, ignore_errors=True)
TMP.mkdir(parents=True)
db.DATA_DIR = TMP
db.DB_PATH = TMP / "t.db"
db.FACE_DIR = TMP / "faces"

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ok = True


def check(label, cond, extra=""):
    global ok
    ok = ok and bool(cond)
    print(f"{'PASS' if cond else 'FAIL'}  {label}{(' | ' + str(extra)) if extra else ''}")


def raises(label, fn, *need):
    try:
        fn()
        check(label, False, "không báo lỗi")
    except ValueError as e:
        check(label, all(n in str(e).lower() for n in need), str(e))


db.init_db()

# --- lớp
c1 = db.add_class(" 12a1 ")
check("thêm lớp ' 12a1 ' -> chuẩn hoá 12A1", db.get_class("12A1") is not None)
raises("lặp lại lớp phải báo lỗi", lambda: db.add_class("12A1"), "tồn tại")

# --- học sinh (CHỈ họ tên/ngày sinh/lớp — không còn SĐT phụ huynh)
a = db.create_student("Nguyễn Thế Hoàng", "2008-01-02", "12a1")
b = db.create_student("Trần Thị Bình", "2008-03-04", "12A1")
c = db.create_student("Lê Văn Cường", "2008-05-06", "12A2")
check("lớp học sinh được chuẩn hoá", db.get_student(a)["class_name"] == "12A1",
      db.get_student(a)["class_name"])
check("không còn cột parent_phone", "parent_phone" not in db.get_student(a),
      list(db.get_student(a)))
try:
    db.create_student("X", "2008-01-01", "12A1", "0901112222")
    check("hàm create_student không nhận SĐT phụ huynh nữa", False, "không TypeError")
except TypeError:
    check("hàm create_student không nhận SĐT phụ huynh nữa", True)
check("không còn hàm phụ huynh", not hasattr(db, "children_of_parent"))

db.add_class("12A2")

# --- lịch học theo buổi
sched = db.get_schedule("12A1")
check("lớp mới có lịch mặc định 2 buổi", sched["sessions"] == 2, sched)
db.set_schedule("12A1", {"sessions": 1, "sang_start": "07:00", "sang_end": "11:00"})
check("đặt lịch 1 buổi/ngày", db.get_schedule("12A1")["sessions"] == 1)
check("lớp 1 buổi -> luôn tính buổi 1",
      db.current_session("12A1", datetime(2026, 10, 5, 14, 0)) == 1)
raises("số buổi phải 1 hoặc 2", lambda: db.set_schedule("12A1", {"sessions": 3}),
       "buổi")
raises("giờ sai định dạng", lambda: db.set_schedule("12A1", {"sang_start": "7h"}),
       "sai định dạng")
raises("buổi sáng phải kết thúc sau khi bắt đầu",
       lambda: db.set_schedule("12A1", {"sang_start": "11:00", "sang_end": "07:00"}),
       "buổi sáng")
raises("lớp chưa khai báo lỗi",
       lambda: db.set_schedule("KHONGCO", {"sessions": 1}), "tìm thấy lớp")
db.set_schedule("12A1", {"sessions": 2, "sang_start": "05:00", "sang_end": "11:59",
                         "chieu_start": "12:00", "chieu_end": "23:59"})
check("lớp 2 buổi: 8h -> buổi 1, 13h -> buổi 2",
      db.current_session("12A1", datetime(2026, 10, 5, 8, 0)) == 1
      and db.current_session("12A1", datetime(2026, 10, 5, 13, 0)) == 2)

# --- điểm danh THEO BUỔI (mỗi buổi tối đa 1 dòng, bấm lại chỉ tăng hits)
d1 = db.add_log(a, "Nguyễn Thế Hoàng", "12A1", 0.1,
                when=datetime(2026, 10, 5, 8, 0))
d2 = db.add_log(a, "Nguyễn Thế Hoàng", "12A1", 0.2,
                when=datetime(2026, 10, 5, 9, 30))
d3 = db.add_log(a, "Nguyễn Thế Hoàng", "12A1", 0.1,
                when=datetime(2026, 10, 5, 13, 30))
check("buổi sáng: first=True session=1",
      d1.get("session") == 1 and d1.get("first") is True, d1)
check("bấm lại trong cùng buổi: first=False, vẫn session=1",
      d2.get("session") == 1 and d2.get("first") is False, d2)
check("buổi chiều: session=2, first=True",
      d3.get("session") == 2 and d3.get("first") is True, d3)
s = db.attendance_summary(a)
check("2 buổi trong 1 ngày vẫn tính 1 ngày công", s["days"] == 1, s["days"])
check("chi tiết ngày gộp đủ buổi [1, 2]",
      s["detail"][0]["sessions"] == [1, 2], s["detail"][0]["sessions"])
check("hits = 1+1+1 (buổi sáng gõ 2 lần -> 2, buổi chiều 1)",
      s["detail"][0]["hits"] == 3, s["detail"][0]["hits"])
db.add_log(b, "Trần Thị Bình", "12A1", 0.1, when=datetime(2026, 10, 5, 8, 0))
check("chưa điểm danh hôm nay -> has_attendance_today=False",
      db.has_attendance_today(c) is False)
db.add_log(a, "Nguyễn Thế Hoàng", "12A1", 0.1)      # điểm danh "hôm nay"
check("đã điểm danh hôm nay -> has_attendance_today=True",
      db.has_attendance_today(a) is True)

# nhật ký bị cắt 500 dòng nhưng attendance phải giữ nguyên
for _ in range(600):
    db.add_log(a, "Nguyễn Thế Hoàng", "12A1", 0.1,
               when=datetime(2026, 10, 5, 8, 0))
with db._conn() as conn:
    hits_s1 = conn.execute(
        "SELECT hits FROM attendance WHERE student_id = ?"
        " AND day = '2026-10-05' AND session = 1", (a,)).fetchone()["hits"]
check("attendance KHÔNG bị cắt khi nhật ký đầy (hits=602)", hits_s1 == 602, hits_s1)
check("nhật ký vẫn giới hạn 500 dòng", len(db.recent_log(1000)) == 500,
      len(db.recent_log(1000)))

# --- ma trận điểm danh
m = db.attendance_matrix([a, b], "2020-01-01", "2030-01-01")
check("ma trận điểm danh có dữ liệu",
      "2026-10-05" in m.get(a, {}) and b in m, list(m.keys()))

# --- vắng / ghi chú
db.set_absence(b, "2026-10-01", "Sốt, xin phép nghỉ", "cô Lan")
check("ghi chú nghỉ lưu được",
      (b, "2026-10-01") in db.absence_map([b], "2026-09-01", "2026-10-31"))
db.set_absence(b, "2026-10-01", "Không nói rõ", "cô Lan")
am = db.absence_map([b], "2026-09-01", "2026-10-31")
check("ghi chú ghi đè được (1 ngày 1 lý do)",
      len(am) == 1 and am[(b, "2026-10-01")]["reason"] == "Không nói rõ", am)
raises("không lý do phải báo lỗi", lambda: db.set_absence(b, "2026-10-02", ""),
       "lý do")
db.clear_absence(b, "2026-10-01")
check("xoá được ghi chú", not db.absence_map([b], "2026-09-01", "2026-10-31"))

# --- xoá/sửa lớp
raises("xoá lớp còn học sinh phải bị chặn", lambda: db.delete_class("12A1"), "còn")
r = db.delete_class("12A1", move_to="12A2")
check("xoá lớp kèm chuyển học sinh", db.get_student(a)["class_name"] == "12A2", r)
check("lớp đã bị xoá khỏi danh mục", db.get_class("12A1") is None)
db.delete_class("12A2", move_to="12A3")
check("chuyển sang lớp chưa có vẫn tạo lớp đích", db.get_class("12A3") is not None)
check("cả 3 học sinh đã về 12A3",
      {db.get_student(x)["class_name"] for x in (a, b, c)} == {"12A3"})

# --- báo cáo theo buổi: đủ / thiếu / nghỉ / vắng
d4 = db.create_student("Bé Chưa Điểm Danh", "2009-09-09", "12A3")
db.add_log(a, "Nguyễn Thế Hoàng", "12A3", 0.1, when=datetime(2026, 10, 6, 8, 0))
db.add_log(a, "Nguyễn Thế Hoàng", "12A3", 0.1, when=datetime(2026, 10, 6, 14, 0))
db.add_log(b, "Trần Thị Bình", "12A3", 0.1, when=datetime(2026, 10, 6, 8, 30))
db.set_absence(c, "2026-10-06", "Nghỉ ốm", "cô Lan")
rep = db.day_report("12A3", "2026-10-06")
st = {row["id"]: row for row in rep["rows"]}
check("ngày đủ 2 buổi -> full",
      st[a]["status"] == "full" and st[a]["sessions_present"] == 2, st[a]["status"])
check("chỉ buổi sáng -> partial", st[b]["status"] == "partial", st[b]["status"])
check("có ghi chú nghỉ -> leave",
      st[c]["status"] == "leave" and st[c]["absence"]["reason"] == "Nghỉ ốm",
      st[c]["status"])
check("không điểm danh, không ghi chú -> absent", st[d4]["status"] == "absent",
      st[d4]["status"])
check("trạng thái từng buổi của người đủ", st[a]["session_state"] == {1: "present",
                                                                    2: "present"},
      st[a]["session_state"])

rr = db.range_report("12A3", "2026-10-06", "2026-10-06")
check("range_report có đúng 1 ngày", rr["days"] == ["2026-10-06"], rr["days"])
check("ma trận khoảng: full/partial/leave/absent",
      rr["matrix"][a]["2026-10-06"]["state"] == "full"
      and rr["matrix"][b]["2026-10-06"]["state"] == "partial"
      and rr["matrix"][c]["2026-10-06"]["state"] == "leave"
      and rr["matrix"][d4]["2026-10-06"]["state"] == "absent",
      {k: rr["matrix"][k]["2026-10-06"]["state"] for k in (a, b, c, d4)})
check("tổng hợp đếm ngày đủ buổi",
      rr["totals"][a]["full"] == 1 and rr["totals"][b]["partial"] == 1,
      rr["totals"][a])

# --- đổi tên lớp (sửa lớp)
out = db.rename_class("12A3", " 12a9 ")
check("đổi tên lớp chuẩn hoá", out["name"] == "12A9"
      and db.get_class("12A9") is not None and db.get_class("12A3") is None, out)
check("học sinh theo lớp mới",
      {db.get_student(x)["class_name"] for x in (a, b, c, d4)} == {"12A9"})

# --- giáo viên
pw = "hash123"
tid = db.create_teacher("lan", pw, "Cô Lan", ["12A9"])
check("tạo giáo viên + gán lớp", db.teacher_class_names(tid) == ["12A9"],
      db.teacher_class_names(tid))
check("lấy giáo viên theo username", db.get_teacher("LAN") is not None)
raises("username trùng phải báo lỗi",
       lambda: db.create_teacher("lan", pw, "Trùng", ["12A9"]), "đã có")
raises("gán lớp chưa có phải báo lỗi",
       lambda: db.create_teacher("moi", pw, "Thầy Mới", ["99Z9"]), "chưa có")
check("danh sách giáo viên không lộ mật khẩu",
      all("password_hash" not in t for t in db.list_teachers()))

# --- tài khoản người dùng (đăng nhập 1 chỗ: admin/developer/teacher)
uid = db.ensure_user("ke_toan", "hash-a", "Kế Toán", "teacher")
check("tạo tài khoản + phân quyền", db.get_user("KE_TOAN")["role"] == "teacher")
check("ensure_user idempotent (gọi lại không tạo trùng)",
      db.ensure_user("ke_toan", "hash-a", "Kế Toán", "teacher") == uid)
check("đổi mật khẩu", db.update_user_password("ke_toan", "hash-b")
      and db.get_user("ke_toan")["password_hash"] == "hash-b")
check("đổi mật khẩu tài khoản không tồn tại -> False",
      db.update_user_password("khong_ton_tai", "x") is False)
names = [u["username"] for u in db.list_users("teacher")]
check("list_users lọc theo vai trò", "ke_toan" in names and "lan" in names, names)
check("list_users không lộ mật khẩu",
      all("password_hash" not in u for u in db.list_users()))
check("xóa tài khoản", db.delete_user(uid) and db.get_user("ke_toan") is None)
db.delete_teacher(tid)
check("xoá giáo viên", db.get_teacher("lan") is None)

# --- tổng hợp (4 học sinh: a, b, c, d4 — cùng lớp 12A9)
ov = db.overview()
check("tổng hợp có số liệu",
      ov["total_students"] == 4 and ov["total_classes"] >= 1, ov)
check("đếm có mặt hôm nay >= 1", ov["present_today"] >= 1, ov["present_today"])
check("sĩ số từng lớp khớp",
      sum(cl["total"] for cl in ov["classes"]) == 4,
      [(cl["name"], cl["total"]) for cl in ov["classes"]])

# --- dọn bản ghi mồ côi: id học sinh đã xoá không được "thừa kế" lịch sử
with db._conn() as conn:
    conn.execute(
        "INSERT INTO attendance (student_id, day, session, first_seen, last_seen,"
        " hits) VALUES (99999, '2026-10-07', 1, '2026-10-07 08:00:00',"
        " '2026-10-07 08:00:00', 1)")
    conn.commit()
db.init_db()
with db._conn() as conn:
    n_orphan = conn.execute(
        "SELECT COUNT(*) AS n FROM attendance WHERE student_id = 99999").fetchone()["n"]
check("init_db dọn điểm danh mồ côi của học sinh đã xoá", n_orphan == 0, n_orphan)

# --- chạy lại init_db nhiều lần phải an toàn (migrate idempotent)
for _ in range(3):
    db.init_db()
check("init_db chạy lại nhiều lần không lỗi", db.count_students() == 4,
      db.count_students())

shutil.rmtree(TMP, ignore_errors=True)
print("\n=> " + ("TAT CA PASS" if ok else "CO FAIL"))
sys.exit(0 if ok else 1)
