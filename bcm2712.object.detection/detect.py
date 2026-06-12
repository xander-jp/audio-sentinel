#!/usr/bin/env python3
"""
Raspberry Pi 5 realtime object detection (for PET-bottle sorting)
=================================================================

Captures the IMX219 (Pi Camera v2) feed with picamera2, runs Ultralytics YOLO
for object detection, and shows the result in an OpenCV window with bounding
boxes and a Top-N confidence panel overlaid.

Design notes:
  * The Pi 5 has no GPU/NPU accelerator, so inference runs on the CPU.
    A nano model + NCNN backend keeps all 4 cores busy and is the fastest
    option. Passing --ncnn on first launch auto-exports the .pt to NCNN.
  * COCO class 39 == "bottle". A PET-bottle sorting demo works off this
    "bottle" class out of the box (note: COCO cannot tell PET / glass / can
    apart -- material-level sorting needs a custom-trained model; see README).
  * Capture runs in a dedicated thread, decoupled from the inference loop, so
    grabbing and inference overlap and throughput stays high.

Usage:
    python detect.py                      # yolo12n.pt, picamera2, GUI
    python detect.py --ncnn               # export to NCNN and run fast (recommended)
    python detect.py --model yolo11n.pt   # swap the model
    python detect.py --source 0           # use a USB/V4L2 camera or a video file
    python detect.py --bottle-only        # detect container classes only

Quit: focus the window and press 'q' or ESC.
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
import time
from collections import deque
from pathlib import Path

import cv2
import numpy as np


# COCO "container"-like classes relevant to sorting. Also the allow-list used
# by --bottle-only. bottle=39, cup=41, wine glass=40.
CONTAINER_CLASS_NAMES = {"bottle", "cup", "wine glass"}

# Per-class colors (BGR). Containers get a high-visibility color.
PRIMARY_COLOR = (0, 220, 60)      # containers (green)
SECONDARY_COLOR = (255, 170, 0)   # everything else (cyan)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Pi 5 realtime object detection (YOLO + picamera2 + OpenCV GUI)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--model", default="yolo12n.pt",
                   help="Ultralytics model name or path (e.g. yolo12n.pt / yolo11n.pt)")
    p.add_argument("--ncnn", action="store_true",
                   help="Export to NCNN and run it (fastest on the Pi 5 CPU)")
    p.add_argument("--imgsz", type=int, default=640, help="Inference input size")
    p.add_argument("--conf", type=float, default=0.35, help="Confidence threshold")
    p.add_argument("--iou", type=float, default=0.45, help="NMS IoU threshold")
    p.add_argument("--top", type=int, default=3, help="Entries shown in the confidence panel (Top-N)")
    p.add_argument("--source", default="picamera2",
                   help='"picamera2" / V4L2 index (e.g. "0") / path to a video file')
    p.add_argument("--cam-size", default="1280x720",
                   help="picamera2 capture resolution WxH")
    p.add_argument("--bottle-only", action="store_true",
                   help="Detect container classes only (bottle/cup/wine glass)")
    p.add_argument("--no-gui", action="store_true",
                   help="No window; print detections to the console (headless check)")
    p.add_argument("--threads", type=int, default=os.cpu_count() or 4,
                   help="Thread count for PyTorch / OpenCV")
    return p.parse_args()


# --------------------------------------------------------------------------- #
# Camera sources
# --------------------------------------------------------------------------- #
class PiCam2Source:
    """Run picamera2 in a dedicated thread, always holding the latest BGR frame."""

    def __init__(self, width: int, height: int):
        from picamera2 import Picamera2  # lazy import (depends on the apt build)

        self.picam2 = Picamera2()
        config = self.picam2.create_preview_configuration(
            main={"size": (width, height), "format": "RGB888"}
        )
        self.picam2.configure(config)
        self.picam2.start()
        time.sleep(0.5)  # let AE/AWB settle

        self._frame: np.ndarray | None = None
        self._lock = threading.Lock()
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        while self._running:
            # picamera2's RGB888 is laid out as BGR for OpenCV's purposes
            frame = self.picam2.capture_array()
            with self._lock:
                self._frame = frame

    def read(self) -> tuple[bool, np.ndarray | None]:
        with self._lock:
            if self._frame is None:
                return False, None
            return True, self._frame.copy()

    def release(self) -> None:
        self._running = False
        self._thread.join(timeout=1.0)
        self.picam2.stop()


class CV2Source:
    """Read a V4L2 camera index or a video file via cv2.VideoCapture."""

    def __init__(self, source: str):
        idx = int(source) if source.isdigit() else source
        self.cap = cv2.VideoCapture(idx)
        if not self.cap.isOpened():
            raise RuntimeError(f"failed to open VideoCapture: {source}")

    def read(self) -> tuple[bool, np.ndarray | None]:
        return self.cap.read()

    def release(self) -> None:
        self.cap.release()


def open_source(args: argparse.Namespace):
    if args.source == "picamera2":
        w, h = (int(x) for x in args.cam_size.lower().split("x"))
        print(f"[info] starting picamera2 at {w}x{h} ...")
        return PiCam2Source(w, h)
    print(f"[info] opening cv2.VideoCapture source={args.source!r} ...")
    return CV2Source(args.source)


# --------------------------------------------------------------------------- #
# Model
# --------------------------------------------------------------------------- #
def load_model(args: argparse.Namespace):
    from ultralytics import YOLO

    model_path = args.model
    if args.ncnn:
        # NCNN bakes the input size into the graph at export time -- passing a
        # different --imgsz at predict time does NOT change it (and can even be
        # slower). So each size gets its own export dir, e.g.
        # yolo12n_320_ncnn_model, and --imgsz actually takes effect.
        #
        # This matters a lot on the Pi 5 CPU: native-320 is ~4-6x faster than
        # 640 (e.g. yolo12n 5.6 -> 32 FPS, yolo11n 11 -> 43 FPS), which is the
        # difference between 3 FPS and clearing 30 FPS.
        stem = Path(model_path).with_suffix("").as_posix()
        ncnn_dir = f"{stem}_{args.imgsz}_ncnn_model"
        # Migrate a legacy size-agnostic 640 export if present.
        legacy_dir = f"{stem}_ncnn_model"
        if (args.imgsz == 640 and Path(legacy_dir).exists()
                and not Path(ncnn_dir).exists()):
            ncnn_dir = legacy_dir
        if not Path(ncnn_dir).exists():
            print(f"[info] exporting {model_path} to NCNN at imgsz={args.imgsz} "
                  f"(first run for this size only) ...")
            YOLO(model_path).export(format="ncnn", imgsz=args.imgsz)
            # Ultralytics always writes <stem>_ncnn_model; rename to the
            # size-specific dir so multiple sizes can coexist.
            written = Path(legacy_dir)
            if written.exists() and ncnn_dir != legacy_dir:
                written.rename(ncnn_dir)
        print(f"[info] loading NCNN model: {ncnn_dir}")
        return YOLO(ncnn_dir, task="detect")

    print(f"[info] loading model: {model_path} (auto-downloaded on first run)")
    return YOLO(model_path)


# --------------------------------------------------------------------------- #
# Drawing
# --------------------------------------------------------------------------- #
def draw_overlay(frame: np.ndarray, dets: list[dict], top_n: int,
                 fps: float) -> np.ndarray:
    """Overlay detection boxes and a Top-N confidence panel."""
    h, w = frame.shape[:2]

    # --- bounding boxes ---
    for d in dets:
        x1, y1, x2, y2 = d["xyxy"]
        is_container = d["name"] in CONTAINER_CLASS_NAMES
        color = PRIMARY_COLOR if is_container else SECONDARY_COLOR
        thickness = 3 if is_container else 2
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, thickness)

        label = f"{d['name']} {d['conf'] * 100:.0f}%"
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
        cv2.rectangle(frame, (x1, y1 - th - 8), (x1 + tw + 6, y1), color, -1)
        cv2.putText(frame, label, (x1 + 3, y1 - 5),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 2, cv2.LINE_AA)

    # --- Top-N confidence panel (top-left) ---
    top = sorted(dets, key=lambda d: d["conf"], reverse=True)[:top_n]
    panel_w = 320
    panel_h = 34 + 30 * max(len(top), 1)
    overlay = frame.copy()
    cv2.rectangle(overlay, (10, 10), (10 + panel_w, 10 + panel_h), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.55, frame, 0.45, 0, frame)

    cv2.putText(frame, f"Top-{top_n} detections", (20, 35),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2, cv2.LINE_AA)
    if not top:
        cv2.putText(frame, "(none)", (24, 68),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (180, 180, 180), 1, cv2.LINE_AA)
    for i, d in enumerate(top):
        y = 64 + i * 30
        is_container = d["name"] in CONTAINER_CLASS_NAMES
        color = PRIMARY_COLOR if is_container else (220, 220, 220)
        bar = int(200 * d["conf"])
        cv2.rectangle(frame, (24, y - 14), (24 + bar, y - 2), color, -1)
        cv2.putText(frame, f"{d['name']} {d['conf'] * 100:.0f}%", (28, y - 4),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 1, cv2.LINE_AA)

    # --- FPS (top-right) ---
    cv2.putText(frame, f"{fps:4.1f} FPS", (w - 130, 35),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2, cv2.LINE_AA)
    return frame


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main() -> int:
    args = parse_args()

    # Give CPU inference all 4 cores
    cv2.setNumThreads(args.threads)
    try:
        import torch
        torch.set_num_threads(args.threads)
    except ImportError:
        pass
    os.environ.setdefault("OMP_NUM_THREADS", str(args.threads))

    model = load_model(args)
    names = model.names  # {id: name}

    allowed_ids = None
    if args.bottle_only:
        allowed_ids = [i for i, n in names.items() if n in CONTAINER_CLASS_NAMES]
        print(f"[info] bottle-only: detecting classes = "
              f"{[names[i] for i in allowed_ids]}")

    source = open_source(args)

    window = "Pi5 Object Detection"
    if not args.no_gui:
        cv2.namedWindow(window, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(window, 1280, 720)

    fps_hist: deque[float] = deque(maxlen=30)
    print("[info] running. quit with 'q' / ESC (Ctrl-C in --no-gui mode)")

    try:
        while True:
            t0 = time.perf_counter()
            ok, frame = source.read()
            if not ok or frame is None:
                time.sleep(0.005)
                continue

            results = model.predict(
                frame, imgsz=args.imgsz, conf=args.conf, iou=args.iou,
                classes=allowed_ids, verbose=False,
            )[0]

            dets: list[dict] = []
            if results.boxes is not None:
                for b in results.boxes:
                    cls_id = int(b.cls[0])
                    dets.append({
                        "name": names[cls_id],
                        "conf": float(b.conf[0]),
                        "xyxy": [int(v) for v in b.xyxy[0].tolist()],
                    })

            dt = time.perf_counter() - t0
            fps_hist.append(1.0 / dt if dt > 0 else 0.0)
            fps = sum(fps_hist) / len(fps_hist)

            if args.no_gui:
                top = sorted(dets, key=lambda d: d["conf"], reverse=True)[:args.top]
                summary = ", ".join(f"{d['name']} {d['conf']*100:.0f}%" for d in top)
                print(f"\r{fps:4.1f} FPS | Top-{args.top}: {summary or '(none)':<60}",
                      end="", flush=True)
            else:
                draw_overlay(frame, dets, args.top, fps)
                cv2.imshow(window, frame)
                key = cv2.waitKey(1) & 0xFF
                if key in (ord("q"), 27):  # q or ESC
                    break
    except KeyboardInterrupt:
        pass
    finally:
        source.release()
        if not args.no_gui:
            cv2.destroyAllWindows()
        print("\n[info] stopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
