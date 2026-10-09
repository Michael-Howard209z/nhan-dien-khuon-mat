"""Đo ngưỡng khớp an toàn cho bộ nhận diện hiện tại rồi gợi ý giá trị đặt vào .env.

Cách dùng
---------
    # 1) Kiểm tra offline bằng chính các mẫu đã đăng ký (không cần camera)
    python tune_threshold.py

    # 2) Đo thêm trên luồng camera thật (server phải đang chạy)
    python tune_threshold.py --live --frames 40

Nguyên lý
---------
Với SFace, đơn vị là KHOẢNG CÁCH COSIN (0 = giống hệt, 2 = đối lập):
    - cùng một người  -> khoảng cách nhỏ
    - hai người khác   -> khoảng cách lớn
Ngưỡng nên đặt GIỮA khoảng cách lớn nhất của chính người đó và khoảng cách
nhỏ nhất giữa hai người khác nhau.

Với LBPH (trường hợp thiếu model SFace) đơn vị là ĐIỂM (càng nhỏ càng giống),
chiều ngược lại: cùng người ~60, người khác ~89.
"""
import argparse
import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

import database as db                                        # noqa: E402
from face_engine import DEFAULTS, FaceEngine                # noqa: E402


# --------------------------------------------------------------------- tiện ích
def stats(values, name, unit):
    if not values:
        print(f"  {name:<22} (không có dữ liệu)")
        return None
    v = np.asarray(values, np.float32)
    print(f"  {name:<22} n={len(v):<5} min={v.min():.3f}  TB={v.mean():.3f}  "
          f"p95={np.percentile(v, 95):.3f}  max={v.max():.3f}  {unit}")
    return v


def cosine_distance(a, b):
    return float(1.0 - np.dot(a, b))


# ------------------------------------------------------------------ offline
def tune_offline(engine):
    """Khoảng cách nội bộ (cùng người) và liên người, tính từ mẫu đã lưu."""
    m = engine.matcher
    print(f"\n=== OFFLINE: dùng {m.info()['identities']} người / "
          f"{m.info()['faces_loaded']} mẫu ({m.name}, đơn vị {m.metric}) ===")

    if m.metric != "cosine":
        print("  LBPH không lấy được vector nên không đo được phân bố khoảng cách.")
        print("  Hãy tải model SFace để có ngưỡng chính xác "
              "(xem README, mục 'Model tự động tải').")
        return None

    intra = []          # cùng người, khác mẫu
    for sid, mat in m.gallery.items():
        n = len(mat)
        if n < 2:
            continue
        iu = np.triu_indices(n, 1)
        for d in (1.0 - (mat @ mat.T)[iu]):
            intra.append(float(d))
    intra_max = max(intra) if intra else None

    inter = []          # hai người khác nhau
    sids = sorted(m.gallery)
    for i in range(len(sids)):
        for j in range(i + 1, len(sids)):
            a, b = m.gallery[sids[i]], m.gallery[sids[j]]
            sim = a @ b.T
            inter.append(float(1.0 - sim.max()))       # cặp gần nhất giữa 2 người
    inter_min = min(inter) if inter else None

    unit = "khoảng cách cosin"
    v_intra = stats(intra, "cùng người", unit)
    v_inter = stats(inter, "khác người", unit)

    # để xem độ phủ của ngưỡng đang dùng lên dữ liệu offline
    print(f"\n  ngưỡng hiện tại: {m.threshold:.3f}")
    if v_intra is not None:
        print(f"    nhận đúng (khoảng cách <= ngưỡng): "
              f"{100.0 * np.mean(v_intra <= m.threshold):.1f}% mẫu cùng người")
    if v_inter is not None:
        print(f"    nhận nhầm (khoảng cách <= ngưỡng): "
              f"{100.0 * np.mean(v_inter <= m.threshold):.1f}% cặp khác người")

    if intra_max is None or inter_min is None:
        print("\n  Cần ít nhất 2 người, mỗi người >= 2 mẫu để gợi ý ngưỡng.")
        return None

    suggest = (intra_max + inter_min) / 2.0
    margin = inter_min - intra_max
    print(f"\n  xấp xỉ lớn nhất (cùng người): {intra_max:.3f}")
    print(f"  xấp xỉ nhỏ nhất (khác người): {inter_min:.3f}")
    print(f"  khoảng hở giữa hai nhóm   : {margin:+.3f}"
          f"{'  (AN TOÀN)' if margin > 0 else '  (CHẠM NHAU - cần thêm mẫu)'}")
    if margin > 0:
        print(f"\n  => MATCH_THRESHOLD gợi ý: {suggest:.2f}")
        print(f"     (giữa {intra_max:.3f} và {inter_min:.3f}; "
              f"giữ nguyên {m.threshold:.2f} nếu hiện tại đang nhận tốt)")
    else:
        print("\n  => Hai nhóm chạm nhau: hãy chụp thêm mẫu đa dạng hơn "
              "(nhiều góc, nhiều ánh sáng, gần + xa).")
    return suggest


# ------------------------------------------------------------------- live
def tune_live(engine, url, frames, interval, owner_center):
    """Đo trên luồng camera: in khoảng cách tới ứng viên gần nhất và thứ hai."""
    import requests

    print(f"\n=== LIVE: lấy {frames} khung từ {url} ===")
    lower = engine.matcher.threshold
    engine.matcher.threshold = 10.0           # tháo ngưỡng để thấy mọi ứng viên
    per_face = []
    try:
        for k in range(frames):
            r = requests.get(f"{url}/api/frame.jpg", timeout=10)
            if not r.ok:
                print(f"  [{k + 1}/{frames}] {r.status_code}: {r.text[:80]}")
                return None
            img = cv2.imdecode(np.frombuffer(r.content, np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                print(f"  [{k + 1}/{frames}] không đọc được ảnh")
                return None
            h, w = img.shape[:2]
            dets = engine._detect(img)
            for d in dets:
                if d.landmarks is None:
                    continue
                emb = engine.matcher.embed_frame(img, d.landmarks)
                ranked = engine.matcher.rank_for(emb) if emb is not None else []
                if not ranked:
                    continue
                best_sid, best_d = ranked[0]
                second_d = ranked[1][1] if len(ranked) > 1 else float("nan")
                name = (db.get_student(best_sid) or {}).get("full_name", f"#{best_sid}")
                per_face.append((best_d, second_d, name, d.box[2]))
                tag = ""
                if owner_center and engine.matcher.metric == "cosine":
                    cx = (d.box[0] + d.box[2] / 2) / max(1, w)
                    tag = " [giữa khung]" if 0.35 < cx < 0.65 else " [lệch]"
                print(f"  [{k + 1:>3}/{frames}] mặt {int(d.box[2])}px  "
                      f"{name:<22} gần nhất={best_d:.3f}  "
                      f"thứ hai={second_d:.3f}{tag}")
            time.sleep(interval)
    except requests.RequestException as exc:
        print(f"  Không lấy được khung hình: {exc}")
        return None
    finally:
        engine.matcher.threshold = lower

    if not per_face:
        print("  Không thấy mặt nào khớp mẫu nào.")
        return None
    best = stats([b for b, _s, _n, _w in per_face], "mọi mặt (gần nhất)", "cosin")
    second = stats([s for _b, s, _n, _w in per_face if np.isfinite(s)],
                   "mọi mặt (thứ hai)", "cosin")
    gaps = [s - b for b, s, _n, _w in per_face if np.isfinite(s)]
    stats(gaps, "cách biệt người 1-2", "cosin")
    print(f"\n  MATCH_MARGIN gợi ý: {min(gaps) if gaps else float('nan'):.3f} "
          f"(giá trị nhỏ nhất trên luồng)")
    print(f"  MATCH_THRESHOLD hiện tại: {lower:.3f}")
    if best is not None:
        print(f"    nhận đúng {100.0 * np.mean(best <= lower):.1f}% số lần nhận diện")
    if second is not None:
        print(f"    nhận nhầm {100.0 * np.mean(second <= lower):.1f}% nếu 2 người giống nhau")
    return best.max() if best is not None else None


# ---------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(
        description="Đo và gợi ý ngưỡng khớp (MATCH_THRESHOLD / MATCH_MARGIN)")
    ap.add_argument("--live", action="store_true",
                    help="đo thêm trên luồng camera (cần server đang chạy)")
    ap.add_argument("--url", default="http://127.0.0.1:5001",
                    help="địa chỉ cổng CAMERA (xem CAMERA_PORT trong .env)")
    ap.add_argument("--frames", type=int, default=20, help="số khung lấy từ camera")
    ap.add_argument("--interval", type=float, default=0.8, help="giây nghỉ giữa 2 khung")
    ap.add_argument("--owner-center", action="store_true",
                    help="đánh dấu mặt ở giữa khung (người đang đăng ký)")
    args = ap.parse_args()

    db.init_db()
    engine = FaceEngine()
    info = engine.info()
    print("=" * 64)
    print("  ĐO NGƯỠNG NHẬN DIỆN")
    print(f"  detector   : {info['detector']}")
    print(f"  recognizer : {info['recognizer']} (đơn vị: {info['metric']})")
    print(f"  ngưỡng     : {info['threshold']}  |  biên: {info['threshold_margin']}")
    print(f"  mặc định   : MATCH_THRESHOLD={DEFAULTS['match_threshold']} "
          f"MATCH_MARGIN={DEFAULTS['match_margin']}")
    print("=" * 64)

    if not engine.trained:
        print("\nChưa có mẫu nào trong data/faces/ — hãy đăng ký ít nhất 2 người "
              "trước khi đo.")
        return 1

    tune_offline(engine)
    if args.live:
        tune_live(engine, args.url, args.frames, args.interval, args.owner_center)
    print("\nGhi vào .env:  MATCH_THRESHOLD = <giá trị gợi ý>\n"
          "Hoặc đổi trực tiếp:  curl -X POST /api/config "
          "-H 'Content-Type: application/json' -d '{\"match_threshold\": 0.42}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
