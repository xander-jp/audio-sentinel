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
# Recommended for 30+ FPS on the Pi 5 CPU: NCNN + a conv-based nano model + imgsz 320
python detect.py --ncnn --model yolo11n.pt --imgsz 320 --bottle-only

python detect.py --ncnn --model yolo11n.pt --imgsz 320   # all classes
python detect.py --ncnn                                  # yolo12n @640 (most accurate, ~6 FPS)
python detect.py                                         # plain PyTorch (slowest)
python detect.py --ncnn --model yolo11n.pt --imgsz 320 --no-gui   # console sanity check
```

Quit by focusing the window and pressing `q` or `ESC`.
If no window appears, run `export DISPLAY=:0` first.

### Performance: pick imgsz and model deliberately

The Pi 5 has no GPU/NPU, so FPS is dominated by the model architecture and the
input size. Two non-obvious facts decide everything:

1. **NCNN bakes the input size into the graph at export time.** Passing a
   different `--imgsz` at predict time does *not* resize the network -- it can
   even be slower. `detect.py` now exports a *separate* NCNN dir per size
   (e.g. `yolo12n_320_ncnn_model`), so `--imgsz 320` genuinely runs at 320.
2. **yolo12 uses attention modules that NCNN parallelizes poorly on ARM CPUs.**
   yolo11/yolov8 are conv-only and scale across cores far better.

Measured on this Pi 5 (NCNN, native-size export):

| model + size | pure inference | full loop (cam+draw) | accuracy (COCO mAP50-95) |
|---|---|---|---|
| yolov8n @320 | 43 FPS | ~35 FPS | ~37.3 |
| **yolo11n @320** | **43 FPS** | **~35 FPS** | **~39.5** |
| yolo12n @320 | 32 FPS | ~28 FPS | ~40.6 |
| yolo11n @640 | 11 FPS | -- | ~39.5 |
| yolo12n @640 | 5.6 FPS | -- | ~40.6 |

**yolo11n @320 is the sweet spot**: clears 30 FPS with headroom, ~1 mAP below
yolo12n (imperceptible for bottle detection). Drop to `--imgsz 256` for more
speed, or stay at 640 only when small/distant objects matter and FPS does not.
For full accuracy *and* high FPS together you need a GPU/NPU (Jetson Orin, or a
Raspberry Pi AI HAT+ / Hailo-8 on this same Pi 5).

Key options (`python detect.py -h` for the full list):

| Option | Default | Description |
|---|---|---|
| `--model` | `yolo12n.pt` | swap to `yolo11n.pt`, etc. |
| `--ncnn` | off | NCNN backend (fastest on the Pi 5 CPU) |
| `--imgsz` | 640 | inference input size; with `--ncnn` it also sets the export size (320 ≈ 4-6× faster than 640) |
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
