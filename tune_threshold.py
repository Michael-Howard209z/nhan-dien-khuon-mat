"""Đo phân bố điểm LBPH theo VỊ TRÍ: người đăng ký (phải-giữa khung) vs người khác.

In thống kê và gợi ý ngưỡng an toàn.
"""
import sys
import time

import cv2
import numpy as np
import requests

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
BASE = "http://127.0.0.1:5000"
FACE = (200, 200)

import database as db

casc = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
rec = cv2.face.LBPHFaceRecognizer_create(radius=1, neighbors=8, grid_x=8, grid_y=8)

imgs, labels = [], []
for p in sorted((db.FACE_DIR / "15").glob("*.jpg")):
    img = cv2.imread(str(p), cv2.IMREAD_GRAYSCALE)
    if img is not None:
        imgs.append(cv2.resize(img, FACE))
        labels.append(15)
rec.update(np.array(imgs), np.array(labels))
print(f"model: {len(imgs)} mau")

owner, other = [], []
for i in range(15):
    frame = cv2.imdecode(
        np.frombuffer(requests.get(f"{BASE}/api/frame.jpg", timeout=10).content, np.uint8),
        cv2.IMREAD_COLOR,
    )
    gray = cv2.equalizeHist(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY))
    for (x, y, w, h) in casc.detectMultiScale(gray, 1.1, 5, minSize=(40, 40)):
        cx = x + w / 2
        roi = cv2.resize(gray[y : y + h, x : x + w], FACE)
        _, d = rec.predict(roi)
        # người đăng ký ngồi giữa-phải; người khác ở mép trái/phải
        if 0.35 * frame.shape[1] < cx < 0.95 * frame.shape[1]:
            owner.append((round(float(d), 1), w))
        else:
            other.append((round(float(d), 1), w))
    time.sleep(0.8)

o = [d for d, _ in other]
u = [d for d, _ in owner]
print(f"\nnguoi dang ky ({len(u)} lan, mat {min(w for _, w in owner) if owner else 0}-"
      f"{max(w for _, w in owner) if owner else 0}px): "
      f"min={min(u):.1f} max={max(u):.1f} TB={np.mean(u):.1f}" if u else "nguoi dang ky: khong thay")
print(f"nguoi khac   ({len(o)} lan): " +
      (f"min={min(o):.1f} max={max(o):.1f} TB={np.mean(o):.1f}" if o else "khong thay"))
if u and o:
    print(f"\n=> nguong goi y: {(max(u) + min(o)) / 2:.1f} "
          f"(cach nguoi dang ky {((max(u) + min(o)) / 2 - max(u)):.1f}, "
          f"cach nguoi khac {(min(o) - (max(u) + min(o)) / 2):.1f})")
