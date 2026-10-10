"""Kiểm thử cổng quản lý: MỘT trang đăng nhập duy nhất + phân quyền 3 vai trò.

Chạy server trước:  python server.py
Rồi chạy test:      python test_portal.py

Vai trò:
  admin     -> /quan-ly (8 tab: tổng quan, đăng ký, hôm nay, điểm danh,
                báo cáo, lớp, giáo viên, hệ thống)
  teacher   -> /quan-ly (3 tab: đăng ký, hôm nay, báo cáo)
  developer -> trang debug ở cổng CAMERA_PORT (preview + cấu hình)

Tự dọn dẹp: xoá học sinh, lớp và tài khoản giáo viên mà test tạo ra.
"""
import os
import sys
from pathlib import Path

import requests
from dotenv import load_dotenv

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import database as db  # noqa: E402

load_dotenv(HERE / ".env", encoding="utf-8-sig")
PORT = int(os.getenv("PORT") or 5000)
CAMERA_PORT = int(os.getenv("CAMERA_PORT") or 5001)
BASE = f"http://127.0.0.1:{PORT}"
CAM = f"http://127.0.0.1:{CAMERA_PORT}"
DEV_USER = os.getenv("DEV_USERNAME") or "developer"
DEV_PASS = os.getenv("DEV_PASSWORD") or "developer"

try:  # console Windows hay dùng cp1258, in tiếng Việt bị lỗi
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass
ok = True
created_class = []


def check(label, cond, extra=""):
    global ok
    ok = ok and bool(cond)
    print(f"{'PASS' if cond else 'FAIL'}  {label}{(' | ' + str(extra)) if extra else ''}")


def html(sess, url, **kw):
    return sess.get(BASE + url, timeout=15, allow_redirects=False, **kw)


# ---------------------------------------------------------------- dữ liệu thử
db.init_db()
sid_a = db.create_student("Học Sinh Thử Ngày", "2008-01-01", "ZZTEST")
sid_b = db.create_student("Học Sinh Thử Lớp", "2008-02-02", "ZZKHAC")
db.add_class("ZZTEST")
db.add_class("ZZKHAC")
created_class = ["ZZTEST", "ZZKHAC"]
# 2 ngày có mặt (add_log luôn ghi hôm nay, nên chèn thẳng bảng attendance)
with db._lock, db._conn() as _c:
    for d in ("2026-10-01", "2026-10-02"):
        _c.execute(
            "INSERT OR REPLACE INTO attendance "
            "(student_id, day, session, first_seen, last_seen, hits)"
            " VALUES (?, ?, 1, ?, ?, 2)",
            (sid_a, d, d + " 07:10:00", d + " 07:40:00"))
    _c.commit()

admin = requests.Session()
gv = requests.Session()
dev = requests.Session()

try:
    # ============================ MỘT TRANG ĐĂNG NHẬP DUY NHẤT =================
    r = requests.get(BASE + "/", timeout=15, allow_redirects=False)
    check("cổng PORT / là trang đăng nhập DUY NHẤT (username + password)",
          r.status_code == 200 and "Đăng nhập hệ thống" in r.text
          and 'name="username"' in r.text and 'name="password"' in r.text
          and 'id="live"' not in r.text, r.status_code)
    check("trang đăng nhập không còn vai trò phụ huynh / chọn vai trò",
          "Phụ huynh" not in r.text and "Chọn vai trò" not in r.text
          and "Chọn vai trò" not in r.text)

    r = requests.get(CAM + "/", timeout=15, allow_redirects=False)
    check("công camera chưa đăng nhập -> chuyển về trang đăng nhập cổng PORT",
          r.status_code in (302, 303)
          and f":{PORT}" in r.headers.get("Location", ""),
          f"{r.status_code} {r.headers.get('Location')}")

    r = requests.get(CAM + "/quan-ly", timeout=15, allow_redirects=False)
    check("cổng camera KHÔNG có trang quản lý",
          r.status_code in (302, 303, 404), r.status_code)

    r = requests.get(BASE + "/video_feed", timeout=15, allow_redirects=False)
    check("cổng PORT KHÔNG phát video (404)", r.status_code == 404, r.status_code)

    r = requests.get(CAM + "/api/ping", timeout=15)
    check("/api/ping mở cho mọi người (sức khoẻ thiết bị)",
          r.status_code == 200 and r.json().get("pong"), r.status_code)

    r = requests.get(CAM + "/api/status", timeout=15)
    check("cổng camera API cần đăng nhập developer (401)",
          r.status_code == 401, r.status_code)

    # ================================================================= đăng nhập
    r = admin.post(BASE + "/dang-nhap", data={"username": "admin", "password": "sai"},
                   timeout=15, allow_redirects=False)
    check("sai mật khẩu bị từ chối (200 + thông báo lỗi)",
          r.status_code == 200 and "Sai tên đăng nhập hoặc mật khẩu" in r.text,
          r.status_code)

    r = admin.post(BASE + "/dang-nhap", data={"username": "", "password": ""},
                   timeout=15, allow_redirects=False)
    check("để trống -> yêu cầu nhập đủ",
          r.status_code == 200 and "Vui lòng nhập" in r.text, r.status_code)

    r = admin.post(BASE + "/dang-nhap", data={"username": "admin", "password": "admin"},
                   timeout=15, allow_redirects=False)
    check("admin/admin -> chuyển về /quan-ly",
          r.status_code == 302 and r.headers.get("Location", "").endswith("/quan-ly"),
          r.headers.get("Location"))

    r = html(admin, "/")
    check("đã đăng nhập thì / đưa thẳng tới trang của mình",
          r.status_code == 302 and "/quan-ly" in r.headers.get("Location", ""),
          f"{r.status_code} {r.headers.get('Location')}")

    # =================================================================== admin
    tabs = ("tong-quan", "dang-ky", "hom-nay", "diem-danh",
            "bao-cao", "lop", "giao-vien", "he-thong")
    bad = [t for t in tabs if html(admin, f"/quan-ly?tab={t}").status_code != 200]
    check("admin mở được đủ 8 tab", not bad, bad)

    # Hồi quy bug Jinja scope: set() trong block content nhưng dùng ở block
    # scripts -> render "var STUD = ;" làm chết toàn bộ nút bấm mọi tab.
    # Mọi tab phải render các biến JS thành mảng JSON hợp lệ.
    pages = {t: html(admin, f"/quan-ly?tab={t}").text for t in tabs}
    js_bad = [t for t, txt in pages.items()
              if "var STUD = ;" in txt or "var CLS = ;" in txt or "= ;" in txt]
    check("JS mọi tab render mảng hợp lệ (không có 'var X = ;')", not js_bad, js_bad)
    check("biến JS STUD/CLS tồn tại ở tab Lớp",
          "var STUD = [" in pages["lop"] and "var CLSS = [" in pages["lop"])

    r = html(admin, "/quan-ly?tab=tong-quan")
    check("tổng quan có ô số liệu",
          "Tổng học sinh" in r.text and "Có mặt hôm nay" in r.text)
    check("tổng quan có bảng sĩ số lớp", "Sĩ số các lớp" in r.text)
    check("tổng quan thấy 2 lớp thử", "ZZTEST" in r.text and "ZZKHAC" in r.text)
    check("thanh tab admin đủ 8 mục", all(t in r.text for t in
          ("Tổng quan", "Đăng ký khuôn mặt", "Hôm nay", "Điểm danh",
           "Báo cáo", "Lớp &amp; lịch học", "Giáo viên", "Hệ thống")))

    r = html(admin, "/quan-ly?tab=dang-ky")
    check("tab Đăng ký: chỉ UPLOAD ảnh, không có chụp camera",
          r.status_code == 200 and "Đăng ký học sinh mới" in r.text
          and 'id="live"' not in r.text, r.status_code)

    r = html(admin, "/quan-ly?tab=dang-ky&q=Thử Ngày")
    check("lọc học sinh theo tên (tab Đăng ký)", r.status_code == 200
          and "Học Sinh Thử Ngày" in r.text, r.status_code)
    check("lọc đúng 1 người (không lẫn lớp khác)", "Học Sinh Thử Lớp" not in r.text)
    check("không còn cột SĐT phụ huynh", "phụ huynh" not in r.text.lower()
          and "0905556666" not in r.text)

    r = html(admin, "/quan-ly?tab=lop")
    check("tab Lớp có nút thêm lớp + cột lịch học",
          r.status_code == 200 and "Thêm lớp mới" in r.text
          and "Lịch học" in r.text, r.status_code)

    r = html(admin, "/quan-ly?tab=he-thong")
    check("tab Hệ thống có thông số máy",
          r.status_code == 200 and "Mô hình nhận diện" in r.text
          and "Cổng dịch vụ" in r.text, r.status_code)

    r = html(admin, "/quan-ly?tab=bao-cao&class=ZZTEST&mode=month&date=2026-10-01")
    check("tab Báo cáo có bảng + chú giải + nút CSV",
          r.status_code == 200 and "Chú giải" in r.text
          and "/api/report.csv" in r.text, r.status_code)

    r = html(admin, "/quan-ly?tab=con-toi")
    check("tab lạ (của phụ huynh cũ) -> về tab đầu tiên",
          r.status_code == 200 and "Tổng học sinh" in r.text
          and "Con tôi" not in r.text)

    # ============================================================== API lớp/lịch
    r = admin.post(BASE + "/api/classes", json={"name": " zz moi "}, timeout=15)
    check("thêm lớp qua API (tự chuẩn hoá)", r.status_code == 200
          and r.json()["class"]["name"] == "ZZMOI", r.text[:120])
    created_class.append("ZZMOI")
    r = admin.post(BASE + "/api/classes", json={"name": "ZZMOI"}, timeout=15)
    check("thêm lớp trùng bị từ chối", r.status_code == 400, r.status_code)

    r = admin.delete(BASE + "/api/classes/ZZTEST", timeout=15)
    check("xoá lớp còn học sinh bị chặn",
          r.status_code == 400 and "còn" in r.text, r.text[:120])

    r = admin.post(BASE + "/api/classes/ZZTEST/schedule",
                   json={"sessions": 1, "sang_start": "07:00",
                         "sang_end": "11:00", "chieu_start": "13:00",
                         "chieu_end": "16:00"}, timeout=15)
    check("đặt lịch học 1 buổi/ngày", r.status_code == 200
          and r.json()["schedule"]["sessions"] == 1, r.text[:120])
    check("lịch học lưu vào DB", db.get_schedule("ZZTEST")["sessions"] == 1)
    r = admin.post(BASE + "/api/classes/ZZTEST/schedule",
                   json={"sessions": 2}, timeout=15)
    check("đổi lại 2 buổi/ngày", r.status_code == 200
          and db.get_schedule("ZZTEST")["sessions"] == 2, r.text[:120])

    r = admin.get(BASE + "/api/system", timeout=15)
    d = r.json() if r.status_code == 200 else {}
    check("API hệ thống trả uptime/ram", r.status_code == 200 and "uptime_sec" in d,
          r.status_code)

    # API học sinh: có ở cổng PORT, TẮT trên cổng camera (debug không đụng dữ liệu)
    r = admin.get(BASE + f"/api/students/{sid_a}", timeout=15)
    check("GET /api/students/<id> có ở cổng PORT", r.status_code == 200, r.status_code)
    r = admin.get(CAM + f"/api/students/{sid_a}", timeout=15)
    check("admin KHÔNG thấy API học sinh trên cổng camera (401)",
          r.status_code == 401, r.status_code)

    # báo cáo CSV
    r = admin.get(BASE + "/api/report.csv?class=ZZTEST&start=2026-10-01"
                  "&end=2026-10-09", timeout=15)
    check("tải CSV báo cáo (200, BOM Excel)",
          r.status_code == 200 and r.content[:3] == b"\xef\xbb\xbf"
          and "text/csv" in r.headers.get("Content-Type", ""),
          f"{r.status_code} {r.headers.get('Content-Type')}")

    # ============================================================== giáo viên
    r = admin.post(BASE + "/api/teachers",
                   json={"username": "thu_nghiem", "password": "1234",
                         "full_name": "Cô Thử Nghiệm", "classes": ["ZZTEST"]},
                   timeout=15)
    check("admin tạo tài khoản giáo viên", r.status_code == 200, r.text[:160])

    r = admin.post(BASE + "/api/teachers",
                   json={"username": "thu_nghiem", "password": "1234",
                         "full_name": "Trùng", "classes": []}, timeout=15)
    check("username trùng bị từ chối", r.status_code == 400, r.status_code)

    r = admin.post(BASE + "/api/teachers",
                   json={"username": "mat_khau_ngan", "password": "12",
                         "full_name": "Ngắn", "classes": []}, timeout=15)
    check("mật khẩu quá ngắn bị từ chối", r.status_code == 400, r.status_code)

    r = gv.post(BASE + "/dang-nhap", data={"username": "thu_nghiem", "password": "sai"},
                timeout=15, allow_redirects=False)
    check("giáo viên sai mật khẩu bị từ chối",
          r.status_code == 200 and "Sai tên đăng nhập" in r.text, r.status_code)

    r = gv.post(BASE + "/dang-nhap", data={"username": "thu_nghiem", "password": "1234"},
                timeout=15, allow_redirects=False)
    check("giáo viên đăng nhập được (cùng 1 trang đăng nhập)",
          r.status_code == 302 and r.headers.get("Location", "").endswith("/quan-ly"),
          r.headers.get("Location"))

    r = html(gv, "/quan-ly")
    check("giáo viên vào thẳng tab đầu tiên (Đăng ký khuôn mặt)",
          r.status_code == 200 and "Đăng ký học sinh mới" in r.text, r.status_code)
    check("thấy lớp được gán", "ZZTEST" in r.text)
    check("KHÔNG thấy lớp khác", "ZZKHAC" not in r.text)
    check("thấy học sinh lớp mình", "Học Sinh Thử Ngày" in r.text)
    check("không thấy tab admin", all(t not in r.text for t in
          ("Tổng quan", "Thêm lớp mới", "Tài khoản giáo viên", "Cổng dịch vụ")))

    gv_pages = {}
    for t in ("hom-nay", "bao-cao"):
        r = html(gv, f"/quan-ly?tab={t}")
        gv_pages[t] = r.text
        check(f"giáo viên mở được tab {t}",
              r.status_code == 200 and "Chú giải" in r.text, r.status_code)
    check("JS tab giáo viên không bị lỗi biến rỗng",
          not any("= ;" in txt for txt in gv_pages.values()))

    r = html(gv, "/quan-ly?tab=lop")
    check("giáo viên ép tab Lớp của admin -> về tab đầu",
          r.status_code == 200 and "Đăng ký học sinh mới" in r.text
          and "Thêm lớp mới" not in r.text)

    r = html(gv, "/quan-ly?tab=diem-danh&class=ZZKHAC")
    check("giáo viên ép tab Điểm danh/URL lớp khác -> về tab được phép",
          r.status_code == 200 and "Thêm lớp mới" not in r.text)

    r = gv.get(BASE + "/api/students?class=ZZKHAC", timeout=15)
    check("giáo viên không xem được học sinh lớp khác (403)",
          r.status_code == 403, r.status_code)
    r = gv.get(BASE + "/api/students", timeout=15)
    names = [s["full_name"] for s in r.json()["students"]]
    check("API học sinh của giáo viên chỉ trả lớp mình",
          "Học Sinh Thử Ngày" in names and "Học Sinh Thử Lớp" not in names, names)

    r = gv.post(BASE + "/api/classes", json={"name": "HACK"}, timeout=15,
                allow_redirects=False)
    check("giáo viên không tạo được lớp (bị chuyển về trang của họ)",
          r.status_code == 302 and "/quan-ly" in r.headers.get("Location", ""),
          r.headers.get("Location"))

    r = gv.delete(BASE + "/api/teachers/1", timeout=15, allow_redirects=False)
    check("giáo viên không xoá được tài khoản", r.status_code == 302, r.status_code)

    r = gv.get(BASE + "/api/system", timeout=15, allow_redirects=False)
    check("giáo viên không xem được API hệ thống", r.status_code == 302,
          r.status_code)

    # giáo viên ghi chú: lớp khác bị chặn, lớp mình được
    r = gv.post(BASE + "/api/absences", json={"student_id": sid_b, "day": "2026-10-01",
                                              "reason": "hack"}, timeout=15)
    check("ghi chú lớp người khác bị chặn (403)", r.status_code == 403, r.status_code)
    check("không ghi được ghi chú lớp khác",
          not db.absence_map([sid_b], "2026-01-01", "2026-12-31"))

    r = gv.post(BASE + "/api/absences", json={"student_id": sid_a, "day": "2026-10-03",
                                              "reason": "Sốt, xin phép nghỉ"},
                timeout=15)
    check("ghi chú lớp mình được", r.status_code == 200, r.text[:120])
    am = db.absence_map([sid_a], "2026-01-01", "2026-12-31")
    check("ghi chú lưu đúng lý do",
          am.get((sid_a, "2026-10-03"), {}).get("reason") == "Sốt, xin phép nghỉ", am)
    check("ghi chú lưu tên người ghi",
          am.get((sid_a, "2026-10-03"), {}).get("created_by") == "Cô Thử Nghiệm", am)

    r = gv.get(BASE + f"/api/students/{sid_a}/attendance", timeout=15)
    check("xem điểm danh học sinh lớp mình được",
          r.status_code == 200 and r.json()["attendance"]["days"] == 2,
          f"{r.status_code}")
    r = gv.get(BASE + f"/api/students/{sid_b}/attendance", timeout=15)
    check("xem điểm danh lớp khác bị chặn (403)", r.status_code == 403, r.status_code)

    r = gv.get(BASE + "/api/report.csv?class=ZZTEST&start=2026-10-01"
               "&end=2026-10-09", timeout=15)
    check("giáo viên tải được CSV báo cáo", r.status_code == 200, r.status_code)

    # =============================================================== developer
    r = dev.post(BASE + "/dang-nhap",
                 data={"username": DEV_USER, "password": DEV_PASS},
                 timeout=15, allow_redirects=False)
    check("developer đăng nhập -> chuyển sang cổng debug",
          r.status_code == 302 and "/debug" in r.headers.get("Location", ""),
          r.headers.get("Location"))

    r = dev.get(CAM + "/", timeout=15, allow_redirects=False)
    check("developer mở được trang debug camera (preview)",
          r.status_code == 200 and 'id="live"' in r.text, r.status_code)
    check("trang debug không còn bảng đăng ký học sinh",
          "Đăng ký học sinh mới" not in r.text)

    r = dev.get(CAM + "/api/status", timeout=15)
    check("developer gọi được API debug", r.status_code == 200, r.status_code)

    r = admin.get(CAM + "/api/status", timeout=15)
    check("admin KHÔNG gọi được API debug của cổng camera (401)",
          r.status_code == 401, r.status_code)

    r = dev.get(CAM + "/quan-ly", timeout=15, allow_redirects=False)
    check("developer trên cổng camera vẫn không có trang quản lý",
          r.status_code in (302, 303, 404), r.status_code)

    # ============================================================ không phụ huynh
    with db._conn() as _c:
        cols = [row[1] for row in _c.execute("PRAGMA table_info(students)")]
        tables = [row[0] for row in _c.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")]
    check("DB không còn cột parent_phone", "parent_phone" not in cols, cols)
    check("DB không còn bảng teachers cũ (đã gộp vào users)",
          "teachers" not in tables and "users" in tables, tables)
    check("module database không còn hàm phụ huynh",
          not any("parent" in n for n in dir(db)))

    # ================================================================ đăng xuất
    r = gv.post(BASE + "/dang-xuat", timeout=15, allow_redirects=False)
    check("đăng xuất được", r.status_code == 302)
    r = html(gv, "/quan-ly")
    check("sau đăng xuất thì bị chặn", r.status_code == 302)

finally:
    # --------------------------------------------------------------- dọn dẹp
    for s in (sid_a, sid_b):
        try:
            admin.delete(BASE + f"/api/students/{s}", timeout=15)
        except Exception as e:
            print("  cleanup student", s, "->", e)
    for t in db.list_teachers():
        if t["username"] in ("thu_nghiem", "mat_khau_ngan"):
            try:
                admin.delete(BASE + f"/api/teachers/{t['id']}", timeout=15)
            except Exception as e:
                print("  cleanup teacher", t["username"], "->", e)
    for c in created_class + ["HACK"]:
        try:
            admin.delete(BASE + f"/api/classes/{c}", timeout=15)
        except Exception as e:
            print("  cleanup class", c, "->", e)

# kiểm tra đã dọn sạch
leftover = [c["name"] for c in admin.get(BASE + "/api/classes", timeout=15)
            .json()["classes"]
            if c["name"].startswith("ZZ") or c["name"] == "HACK"]
check("đã dọn sạch lớp thử", not leftover, leftover)
check("đã dọn sạch giáo viên thử",
      not any(t["username"] in ("thu_nghiem", "mat_khau_ngan")
              for t in db.list_teachers()))
check("không còn học sinh thử",
      not any("Thử" in s["full_name"] for s in db.list_students()))

print("\n=> " + ("TAT CA PASS" if ok else "CO FAIL"))
sys.exit(0 if ok else 1)
