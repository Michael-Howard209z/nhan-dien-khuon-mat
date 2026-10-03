"""Phát hiện và nhận dạng khuôn mặt bằng OpenCV (Haar + LBPH).

Mỗi người được đăng ký nhiều mẫu ảnh mặt; model LBPH được huấn luyện lại
mỗi khi thêm/sửa/xóa người. Nhãn vẽ lên khung dùng Pillow để hiển thị
đúng tiếng Việt (có dấu).
"""
import threading
import urllib.request
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

import database as db

FACE_SIZE = (200, 200)
# LBPH: số càng nhỏ càng giống. Đo thực tế (xem tune_lbph.py): cùng người ~60,
# người khác ~89 => 75 là điểm giữa an toàn.
DEFAULT_THRESHOLD = 75.0

if not hasattr(cv2, "face"):
    raise RuntimeError(
        "Thiếu module cv2.face. Hãy cài: pip uninstall opencv-python "
        "opencv-contrib-python && pip install opencv-contrib-python"
    )

_FONT_PATHS = [
    r"C:\Windows\Fonts\arial.ttf",
    r"C:\Windows\Fonts\segoeui.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
]
_fonts = {}


def _font(size: int):
    size = int(size)
    if size not in _fonts:
        path = next((p for p in _FONT_PATHS if Path(p).exists()), None)
        _fonts[size] = ImageFont.truetype(path, size) if path else ImageFont.load_default()
    return _fonts[size]


def _new_recognizer():
    factory = getattr(cv2.face, "LBPHFaceRecognizer_create", None)
    if factory:
        return factory()
    return cv2.face.LBPHFaceRecognizer.create()


CASCADE_NAME = "haarcascade_frontalface_default.xml"
CASCADE_URL = (
    "https://raw.githubusercontent.com/opencv/opencv/4.x/data/haarcascades/"
    + CASCADE_NAME
)


def _load_cascade():
    """Tải Haar cascade; nếu OpenCV không kèm file thì tự tải một lần về data/."""
    path = Path(cv2.data.haarcascades) / CASCADE_NAME
    if not path.exists():
        local = db.DATA_DIR / "haarcascades" / CASCADE_NAME
        if not local.exists():
            local.parent.mkdir(parents=True, exist_ok=True)
            print(f"[face] Dang tai {CASCADE_NAME} tu GitHub...")
            urllib.request.urlretrieve(CASCADE_URL, local)
        path = local
    cascade = cv2.CascadeClassifier(str(path))
    if cascade.empty():
        raise RuntimeError(f"Không tải được Haar cascade: {path}")
    return cascade


def draw_text(img, text, org, color, bg=(20, 20, 20), pad=4):
    """Vẽ text (tiếng Việt) lên ảnh BGR, có nền để dễ đọc. color là RGB."""
    x, y = int(org[0]), int(org[1])
    pil = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(pil)
    font = _font(max(14, img.shape[0] // 36))
    try:
        left, top, right, bottom = draw.textbbox((x, y), text, font=font)
    except AttributeError:
        left, top, right, bottom = (x, y, x + 8 * len(text), y + font.size)
    if bg is not None:
        draw.rectangle((left - pad, top - pad, right + pad, bottom + pad), fill=bg)
    draw.text((x, y), text, font=font, fill=tuple(color))
    out = cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)
    img[:] = out


class FaceEngine:
    def __init__(self, threshold: float = DEFAULT_THRESHOLD):
        self._lock = threading.RLock()
        # cv2.CascadeClassifier KHÔNG thread-safe: mọi lời gọi detectMultiScale
        # (vòng xử lý luồng nền + luồng request đăng ký/nhận diện) phải qua khóa này.
        self._detect_lock = threading.Lock()
        self.threshold = float(threshold)
        self.cascade = _load_cascade()
        self.recognizer = _new_recognizer()
        self._trained = False
        self.retrain()

    # ------------------------------------------------------------- detection
    def detect_faces(self, gray):
        try:
            with self._detect_lock:
                # minSize 40px: bắt được cả mặt xa (60px là ~3m với camera 720p)
                return self.cascade.detectMultiScale(
                    gray, scaleFactor=1.1, minNeighbors=5, minSize=(40, 40)
                )
        except cv2.error as exc:  # phòng ngừa lỗi nội bộ của OpenCV
            print(f"[detect] {exc}")
            return ()

    def _largest_face_gray(self, bgr):
        gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
        gray = cv2.equalizeHist(gray)
        faces = self.detect_faces(gray)
        if len(faces) == 0:
            return None
        x, y, w, h = max(faces, key=lambda f: f[2] * f[3])
        roi = gray[y : y + h, x : x + w]
        return cv2.resize(roi, FACE_SIZE)

    # -------------------------------------------------------------- samples
    def add_samples(self, student_id: int, frames) -> int:
        """Lưu mẫu mặt từ các khung hình. Trả về số mẫu đã lưu."""
        folder = db.FACE_DIR / str(student_id)
        folder.mkdir(parents=True, exist_ok=True)
        saved = 0
        for frame in frames:
            face = self._largest_face_gray(frame)
            if face is None:
                continue
            idx = len(list(folder.glob("*.jpg")))
            cv2.imwrite(str(folder / f"{idx:03d}.jpg"), face)
            cv2.imwrite(str(folder / f"{idx:03d}_flip.jpg"), cv2.flip(face, 1))
            saved += 1
        if saved:
            self.retrain()
        return saved

    def retrain(self):
        with self._lock:
            self.recognizer = _new_recognizer()
            images, labels, counts = [], [], {}
            if db.FACE_DIR.exists():
                for folder in sorted(db.FACE_DIR.iterdir()):
                    if not folder.is_dir():
                        continue
                    try:
                        sid = int(folder.name)
                    except ValueError:
                        continue
                    n = 0
                    for path in sorted(folder.glob("*.jpg")):
                        img = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
                        if img is None:
                            continue
                        images.append(cv2.resize(img, FACE_SIZE))
                        labels.append(sid)
                        n += 1
                    counts[sid] = n
            if images:
                self.recognizer.update(np.array(images), np.array(labels))
                self._trained = True
            else:
                self._trained = False
        # Đồng bộ số mẫu với DB
        for st in db.list_students():
            db.set_sample_count(st["id"], counts.get(st["id"], 0))

    # -------------------------------------------------------------- process
    def process(self, frame, draw: bool = True):
        """Nhận diện toàn bộ khuôn mặt trong khung. Trả về (kết quả, ảnh)."""
        results = []
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.equalizeHist(gray)
        faces = self.detect_faces(gray)
        with self._lock:
            trained = self._trained
            threshold = self.threshold
            for (x, y, w, h) in faces:
                roi = cv2.resize(gray[y : y + h, x : x + w], FACE_SIZE)
                student_id, confidence, student = None, None, None
                if trained:
                    label, conf = self.recognizer.predict(roi)
                    confidence = round(float(conf), 1)
                    if conf <= threshold:
                        student_id = int(label)
                        student = db.get_student(student_id)
                        if student is None:
                            student_id = None
                results.append(
                    {
                        "box": [int(x), int(y), int(w), int(h)],
                        "student_id": student_id,
                        "full_name": student["full_name"] if student else None,
                        "date_of_birth": student["date_of_birth"] if student else None,
                        "class_name": student["class_name"] if student else None,
                        "confidence": confidence,
                        "match": student is not None,
                    }
                )
        if draw:
            for r in results:
                x, y, w, h = r["box"]
                if r["match"]:
                    box_color = (80, 220, 80)          # BGR
                    text_rgb = (90, 230, 90)           # RGB cho Pillow
                    label = f"{r['full_name']} | {r['class_name']} | {r['date_of_birth']}"
                    conf = r["confidence"]
                    if conf is not None:
                        label += f" ({conf})"
                else:
                    box_color = (70, 70, 230)          # BGR
                    text_rgb = (255, 95, 95)           # RGB cho Pillow
                    label = "Chưa nhận diện"
                cv2.rectangle(frame, (x, y), (x + w, y + h), box_color, 2)
                ty = y - 10
                if ty < 16:
                    ty = y + h + 6
                draw_text(frame, label, (x + 2, ty), text_rgb)
        return results, frame
