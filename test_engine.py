"""Kiểm thử face_engine: đa người, chống nhầm, tracking, đăng ký mẫu.

Chạy độc lập (không cần server, không cần camera):
    python test_engine.py

Dùng 2 ảnh có sẵn trong thư mục dự án:
    data_test_face2.jpg (người A)   data_test_face.jpg (người B)
Ảnh được chuẩn hoá về cùng cỡ mặt trước khi test, vì mặt quá nhỏ sẽ bị
MIN_FACE_SIZE lọc mất (đó là hành vi đúng, chỉ là không dùng được cho test).
"""
import sys
import time
from pathlib import Path

import cv2
import numpy as np

BASE = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE))
import database as db                                    # noqa: E402
import face_engine as fe                                 # noqa: E402

try:  # console Windows hay dùng cp1258, in tiếng Việt bị lỗi
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass
TMP = BASE / "data" / "_test_out"

# Test chạy trên DB RIÊNG (data/_test_out) để không đụng dữ liệu người dùng thật.
TMP.mkdir(parents=True, exist_ok=True)
db.DATA_DIR = TMP
db.DB_PATH = TMP / "students.db"
db.FACE_DIR = TMP / "faces"


A_RAW = cv2.imread(str(BASE / "data_test_face2.jpg"))
B_RAW = cv2.imread(str(BASE / "data_test_face.jpg"))
PASS = FAIL = 0


def check(name, cond, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {extra}")


def face_width(img):
    d = fe.FaceDetector(0.5, 20)
    dets = d.detect(img)
    return float(dets[0].box[2]) if dets else 0.0


def scaled(img, target_px, ratio=3.2):
    """Phóng ảnh sao cho mặt ~target_px rồi cắt khung vuông quanh mặt."""
    d = fe.FaceDetector(0.5, 20).detect(img)
    if not d:
        return img
    d = d[0]
    k = target_px / float(d.box[2])
    big = cv2.resize(img, (0, 0), fx=k, fy=k, interpolation=cv2.INTER_CUBIC)
    H, W = big.shape[:2]
    x, y, w, h = (int(round(v * k)) for v in d.box)
    cx, cy = x + w // 2, y + h // 2
    side = max(32, int(round(target_px * ratio)))
    side = min(side, H, W)
    x0 = max(0, min(W - side, cx - side // 2))
    y0 = max(0, min(H - side, cy - side // 2))
    return np.ascontiguousarray(big[y0:y0 + side, x0:x0 + side])


A = scaled(A_RAW, 150)      # ảnh nhân vật A, mặt ~150px (thực tế khi chụp gần)
B = scaled(B_RAW, 150)
print(f"chuẩn hoá: A={A.shape} B={B.shape} (mặt ~150px)")


def scene(left, right, pad=40, canvas=(620, 1100)):
    """Dán 2 ảnh cạnh nhau -> khung có 2 người."""
    out = np.full((canvas[0], canvas[1], 3), 40, np.uint8)
    h, w = min(left.shape[0], canvas[0]), min(left.shape[1], canvas[1])
    out[:h, :w] = left[:h, :w]
    ox = w + pad
    h2, w2 = min(right.shape[0], canvas[0]), min(right.shape[1], canvas[1] - ox)
    out[:h2, ox:ox + w2] = right[:h2, :w2]
    return out


print("\n=== 1. Khởi tạo engine ===")
t0 = time.perf_counter()
eng = fe.FaceEngine()
print(f"  load {time.perf_counter() - t0:.1f}s")
info = eng.info()
print("  ", {k: v for k, v in info.items() if k != "timing_ms"})
check("dùng YuNet", info["detector"] == "yunet", info["detector"])
check("dùng SFace (vector, không phải LBPH)", info["recognizer"] == "sface", info["recognizer"])
check("có căn theo điểm mốc", info["landmark_align"] is True)
check("ngưỡng mặc định hợp lệ với cosin", 0.05 < eng.threshold < 1.0, eng.threshold)

print("\n=== 2. Phát hiện nhiều người trong 1 khung ===")
multi = scene(A, B)
dets = eng._detect(multi)
check("thấy đủ 2 khuôn mặt", len(dets) == 2, f"n={len(dets)}")
check("mỗi mặt có 5 điểm mốc",
      all(d.landmarks is not None and d.landmarks.shape == (5, 2) for d in dets))
for d in dets:
    print(f"     box={d.box} score={d.score:.3f}")

print("\n=== 3. Đăng ký 2 người, mỗi người nhiều mẫu ===")
db.init_db()
for st in db.list_students():          # DB của test, xoá sạch cho sạch sẽ
    db.delete_student(st["id"])
sid_a = db.create_student("Nguyen Van An", "2005-03-12", "10A1")
sid_b = db.create_student("Tran Thi Bich", "2006-07-01", "10A2")


def sample_canvases(img, name):
    """4 biến thể: gốc, sáng, nhỏ xa, xoay nhẹ -> giống chụp ở nhiều cự ly."""
    out = []
    for tag, target, gain, ang in (("goc", 150, 1.0, 0), ("sang", 150, 1.25, 0),
                                   ("xa", 85, 1.0, 0), ("nghieng", 140, 1.0, 8)):
        p = scaled(img, target)
        if gain != 1.0:
            p = np.clip(p.astype(np.float32) * gain, 0, 255).astype(np.uint8)
        if ang:
            p = cv2.warpAffine(p, cv2.getRotationMatrix2D((p.shape[1] / 2, p.shape[0] / 2), ang, 1.0),
                               (p.shape[1], p.shape[0]))
        out.append(np.ascontiguousarray(p))
    return out


r = eng.add_samples(sid_a, sample_canvases(A, "A"))
print(f"  A: {r}")
check("lưu được >= 3 mẫu cho A", r["saved"] >= 3, r)

r = eng.add_samples(sid_b, sample_canvases(B, "B"))
print(f"  B: {r}")
check("lưu được >= 3 mẫu cho B", r["saved"] >= 3, r)

dup = eng.add_samples(sid_a, [scaled(A, 150)])
print(f"  A thêm lại ảnh y hệt: {dup} (saved phải = 0)")
check("mẫu trùng bị loại", dup["saved"] == 0, dup)

info = eng.info()
print("  ", {k: v for k, v in info.items() if k != "timing_ms"})
check("thấy 2 người trong bộ so khớp", info["identities"] == 2, info)
check("có ngưỡng riêng từng người", info["adaptive_threshold"] is True, info)
print("   per-person thresholds:", eng.matcher.per_person)
check("số mẫu trong DB khớp", db.get_student(sid_a)["samples"] >= 3, db.get_student(sid_a))

print("\n=== 4. Nhận diện 1 người (không tracking) ===")
res, _ = eng.process(A, draw=False, track=False)
check("thấy 1 mặt", len(res) == 1, res)
check("nhận đúng người A", res and res[0]["full_name"] == "Nguyen Van An", res)
print("   ", {k: res[0][k] for k in ("full_name", "confidence", "score", "match")} if res else None)
res_b, _ = eng.process(B, draw=False, track=False)
check("nhận đúng người B", res_b and res_b[0]["full_name"] == "Tran Thi Bich", res_b)

print("\n=== 5. CHỐNG NHẦM: 2 người trong cùng khung ===")
res, _ = eng.process(multi, draw=False, track=False)
names = sorted(r["full_name"] for r in res if r["match"])
print("   ", [(r["full_name"], r["confidence"]) for r in res])
check("nhận đúng cả 2, không lẫn lộn",
      len(res) == 2 and names == ["Nguyen Van An", "Tran Thi Bich"], res)

print("\n=== 6. Người lạ KHÔNG bị nhận nhầm ===")
fake = np.full((400, 400, 3), 128, np.uint8)
cv2.circle(fake, (200, 180), 90, (150, 160, 170), -1)
res, _ = eng.process(fake, draw=False, track=False)
check("vật thể không phải mặt -> không khớp ai", all(not r["match"] for r in res), res)
# chèn B (đã đăng ký) vào khung nhỏ -> khoảng cách lớn, phải từ chối nếu vượt ngưỡng
tiny = np.full((500, 700, 3), 40, np.uint8)
tiny[: scaled(B, 60).shape[0], : scaled(B, 60).shape[1]] = scaled(B, 60)
res, _ = eng.process(tiny, draw=False, track=False)
print("   mặt rất nhỏ (~60px):", [(r["full_name"], r["confidence"], r["match"]) for r in res])
check("mặt rất nhỏ: không nhận sai người",
      all(r["full_name"] != "Nguyen Van An" for r in res), res)

print("\n=== 7. Tracking: ô vuông mượt + ID ổn định ===")
eng.reset_tracker()
big = scene(A, B)
frames = []
for k in range(24):
    M = np.float32([[1, 0, k * 3], [0, 1, k * 2]])
    frames.append(cv2.warpAffine(big, M, (big.shape[1] + 90, big.shape[0] + 60),
                                 borderMode=cv2.BORDER_REPLICATE))
boxes_per_frame = []
t0 = time.perf_counter()
for f in frames:
    res, _ = eng.process(f, draw=True, track=True)
    boxes_per_frame.append([(r["square"], r["track_id"]) for r in res])
dt = time.perf_counter() - t0
print(f"   {len(frames)} khung trong {dt:.2f}s -> {len(frames)/dt:.1f} fps, "
      f"timing={eng.info()['timing_ms']}")
check("mọi khung đều giữ 2 track", all(len(b) == 2 for b in boxes_per_frame),
      [len(b) for b in boxes_per_frame])
ids = {tuple(sorted(t for _s, t in b)) for b in boxes_per_frame if b}
check("ID track không đổi vãn", len(ids) == 1, ids)
tids = next(iter(ids))
check("2 ID khác nhau", len(set(tids)) == 2, tids)


def centers(seq):
    return [sorted((s[0] + s[2] / 2, s[1] + s[2] / 2) for s, _ in pair) for pair in seq]


c = centers(boxes_per_frame)
jumps = [abs(c[i][k][0] - c[i - 1][k][0]) + abs(c[i][k][1] - c[i - 1][k][1])
         for i in range(1, len(c)) for k in range(2)]
print(f"   trôi ô vuông/khung: max={max(jumps):.1f}px  TB={np.mean(jumps):.1f}px")
check("ô vuông mượt (không giật)", max(jumps) < 70, f"max={max(jumps):.1f}")
check("ô vuấtự di chuyển theo người", np.mean(jumps) > 3.0, np.mean(jumps))
sides = [s[2] for pair in boxes_per_frame for s, _ in pair]
check("cạnh ô vuông ổn định", max(sides) - min(sides) < 60, f"{min(sides):.0f}..{max(sides):.0f}")

print("\n=== 8. Tracking: chỉ chốt tên sau đủ khung xác nhận ===")
eng.reset_tracker()
seq = [eng.process(f, draw=False, track=True)[0] for f in frames]
first, last = seq[0], seq[-1]
print("   khung đầu:", [(r["full_name"], r["confirming"], r["match"]) for r in first])
print("   khung cuối:", [(r["full_name"], r["confirming"], r["match"]) for r in last])
check("khung đầu CHƯA chốt tên", any(r["confirming"] for r in first), first)
check("tới cuối chuỗi đã chốt đúng 2 tên",
      sorted(r["full_name"] for r in last if r["match"]) ==
      ["Nguyen Van An", "Tran Thi Bich"], last)
check("mỗi track một người, không trùng ID",
      len({r["student_id"] for r in last if r["match"]}) == 2, last)

print("\n=== 9. Mất dặt vài khung vẫn giữ ô vuông (tracker) ===")
eng.reset_tracker()
for f in frames[:4]:
    eng.process(f, draw=False, track=True)
res_before, _ = eng.process(frames[4], draw=False, track=True)
boxes_before = sorted(r["square"] for r in res_before)
dark = frames[4].copy()
dark[:, :] = 0
for _ in range(3):
    res_mid, _ = eng.process(dark, draw=False, track=True)
boxes_mid = sorted(r["square"] for r in res_mid)
print(f"   trước: {[f'{b[0]:.0f},{b[1]:.0f},{b[2]:.0f}' for b in boxes_before]}")
print(f"   mất:   {[f'{b[0]:.0f},{b[1]:.0f},{b[2]:.0f}' for b in boxes_mid]}")
check("vẫn giữ 2 ô vuông khi không detect được", len(boxes_mid) == 2, boxes_mid)
check("ô vuông không nhảy khi mất detect",
      all(abs(a[0] - b[0]) < 110 and abs(a[2] - b[2]) < 70
          for a, b in zip(boxes_before, boxes_mid)), (boxes_before, boxes_mid))
res_back, _ = eng.process(frames[5], draw=False, track=True)
print("   sau khi thấy lại:", [(r["track_id"], r["full_name"]) for r in res_back])
check("detect lại -> khớp lại đúng 2 người",
      sorted(r["full_name"] for r in res_back if r["match"]) ==
      ["Nguyen Van An", "Tran Thi Bich"], res_back)
check("ID track giữ nguyên sau khi mất dặt",
      {r["track_id"] for r in res_back} == {r["track_id"] for r in res_before},
      (res_before, res_back))

print("\n=== 10. Đăng ký trong ảnh nhiều người -> chọn người ở giữa khung ===")
combo = np.full((700, 1150, 3), 40, np.uint8)
a_s, b_s = scaled(A, 130), scaled(B, 120)
combo[20:20 + b_s.shape[0], 20:20 + b_s.shape[1]] = b_s                 # góc trái
combo[250:250 + a_s.shape[0], 480:480 + a_s.shape[1]] = a_s           # giữa
cands = eng._candidate_faces(combo)
check("thấy 2 mặt trong ảnh đăng ký", len(cands) == 2, len(cands))
best = cands[0][1]
bx = best.box[0] + best.box[2] / 2
print(f"   mặt được chọn: box={best.box}, tâm x={bx:.0f} (A ở giữa ~575)")
check("chọn người ở giữa khung (A) chứ không phải người ở góc", abs(bx - 575) < 240, bx)

print("\n=== 11. Cấu hình runtime ===")
changed = eng.configure(match_threshold=0.30, match_margin=0.2, track_confirm=2)
print("   đổi:", changed)
check("đổi ngưỡng", eng.threshold == 0.30, eng.threshold)
check("đổi biên", eng.cfg["match_margin"] == 0.2)
check("đổi số khung xác nhận", eng.cfg["track_confirm"] == 2)
check("đổi lại về mặc định", eng.configure(match_threshold=0.42) == ["match_threshold"])
check("ngưỡng riêng hợp lệ sau khi đổi",
      all(v <= 0.42 for v in eng.matcher.per_person.values()), eng.matcher.per_person)
check("tắt tracking qua cấu hình",
      "track_enabled" in eng.configure(track_enabled=False))
eng.configure(track_enabled=True)

print("\n=== 11b. Đổi tracker phụ qua cấu hình ===")
for be in ("csrt", "kcf", "none", "medianflow"):
    ch = eng.configure(track_backend=be)
    eng.reset_tracker()
    res_b2, _ = eng.process(multi, draw=False, track=True)
    ok = len(res_b2) == 2 and all(r.get("track_id") is not None for r in res_b2)
    check(f"backend={be}: vẫn nhận đúng 2 mặt", ok,
          f"{be} -> {[(r['box'], r['track_id']) for r in res_b2]}")
    check(f"backend={be}: engine nhận cấu hình",
          "track_backend" in ch and eng.cfg["track_backend"] == be, eng.cfg["track_backend"])
sm = eng.configure(track_smooth=0.8)
check("đổi độ mượt ô vuông", "track_smooth" in sm and eng.cfg["track_smooth"] == 0.8)
eng.configure(track_smooth=0.45)

print("\n=== 12. Xuất ảnh đã khoanh ===")
res, annotated = eng.process(multi, draw=True, track=False)
cv2.imwrite(str(TMP / "out_multi.jpg"), annotated)
eng.reset_tracker()
last_frame = frames[0]
for f in frames[:6]:
    _res, last_frame = eng.process(f, draw=True, track=True)
cv2.imwrite(str(TMP / "out_track.jpg"), last_frame)
print(f"   ghi out_multi.jpg ({len(res)} kết quả) và out_track.jpg")

print("\n=== 13. Học sinh bị xoá giữa chừng -> không được sinh kết quả 'ma' ===")
# Nếu track đã chốt tên rồi học sinh bị xoá, _pack phải hạ về "không nhận diện".
# Nếu không, kết quả mang student_id không tồn tại + full_name=None làm ghi điểm
# danh hỏng (sqlite NOT NULL) và giết luôn thread xử lý.
ghost = fe.FaceEngine._pack([10, 10, 40, 50], (10, 10, 50), 0.99, 999999, 0.05, True)
check("id học sinh không tồn tại -> match=False", ghost["match"] is False, ghost)
check("id học sinh không tồn tại -> student_id=None", ghost["student_id"] is None, ghost)
check("id học sinh không tồn tại -> full_name=None", ghost["full_name"] is None, ghost)
alive = fe.FaceEngine._pack([10, 10, 40, 50], (10, 10, 50), 0.99, sid_a, 0.05, True)
check("học sinh còn tồn tại -> vẫn nhận diện + có tên",
      alive["match"] and alive["student_id"] == sid_a and bool(alive["full_name"]), alive)

print(f"\n================ {PASS} PASS / {FAIL} FAIL ================")
print(f"(DB riêng của test: {db.DB_PATH}; dữ liệu thật trong data/students.db "
      f"không bị đụng tới)")
sys.exit(1 if FAIL else 0)