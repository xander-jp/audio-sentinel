# Pi 5 realtime object detection (for PET-bottle sorting)

A Python script that overlays YOLO object-detection results on the camera feed
from a Raspberry Pi 5 + IMX219 (Pi Camera v2). It draws bounding boxes plus a
Top-N confidence panel in the top-left (e.g. `bottle 96%` / `cup 12%` ...).

## Setup

```bash
./setup.sh                 # apt packages + a venv built with system-site-packages
source venv/bin/activate
```

> The existing `venv` has `include-system-site-packages=false`, so it can't see
> the apt build of picamera2. `setup.sh` moves it aside to `venv.bak.<ts>` and
> rebuilds one that can.

## Run

```bash
python detect.py --ncnn                 # recommended; first run exports to NCNN, then fast
python detect.py --ncnn --bottle-only   # container classes only (bottle/cup/wine glass)
python detect.py                        # plain PyTorch (slower than NCNN)
python detect.py --no-gui               # no window, print to console (sanity check)
```

Quit by focusing the window and pressing `q` or `ESC`.
If no window appears, run `export DISPLAY=:0` first.

Key options (`python detect.py -h` for the full list):

| Option | Default | Description |
|---|---|---|
| `--model` | `yolo12n.pt` | swap to `yolo11n.pt`, etc. |
| `--ncnn` | off | NCNN backend (fastest on the Pi 5 CPU) |
| `--imgsz` | 640 | inference input size; smaller is faster but less accurate |
| `--conf` | 0.35 | confidence threshold |
| `--top` | 3 | entries shown in the confidence panel |
| `--source` | `picamera2` | also accepts a V4L2 index (`0`) or a video file |
| `--bottle-only` | off | detect container classes only |

## About the model

- **YOLO12 / YOLO11**: bundled with Ultralytics. The default is `yolo12n.pt`
  (nano). The Pi 5 has no GPU/NPU and runs CPU inference, so `n` (nano) + NCNN
  is the realistic choice. `s`/`m` and up are more accurate but drop FPS hard.
- **YOLOv13** lives in a separate research repo and is not integrated into
  Ultralytics proper, so YOLO12 is the stable default (swap via `--model`).

## Note on PET-bottle sorting (important)

A COCO-pretrained model returns the generic **`bottle`** class.
**It cannot distinguish materials** like PET / glass / can. A "bottle detection"
demo works as-is, but real sorting (e.g. pick only PET) needs:

1. a custom dataset labeled PET / other, used to fine-tune `yolo12n.pt`, and
2. loading those weights via `--model best.pt`.

The script only color-codes and filters by class name, so swapping in a
retrained model makes it work for sorting directly.

## Files

- `detect.py` — main script (picamera2 capture + YOLO inference + OpenCV GUI)
- `requirements.txt` — pip deps (picamera2 excluded; the apt build is used)
- `setup.sh` — apt packages + venv build
