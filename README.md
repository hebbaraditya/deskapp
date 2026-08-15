# deskapp

Tried to clone the dingboard. Got most of the way there, then started
preparing for interviews. If anyone's interested, feel free to continue
the work.

AI-related tasks are pending — the goal is to integrate small models and
run them locally (WASM eventually?) so we don't depend on a server the
way dingboard does for half of its pipeline (the SAM model). MobileSAM
segmentation is wired up in code and works locally now (see below);
still on the wishlist: a proper background-removal model, maybe the one
ByteDance/TikTok put out, if I get the time and patience back (🤞).

## Requirements (macOS)

Built and tested on macOS (Apple Silicon). Install via Homebrew:

```bash
brew install cmake glfw onnxruntime
```

`CMakeLists.txt` hardcodes the onnxruntime path:

```
set(ONNXRUNTIME_ROOT "/opt/homebrew/Cellar/onnxruntime/<version>")
```

Check what Homebrew actually installed (`ls /opt/homebrew/Cellar/onnxruntime/`)
and update that line to match if it differs.

## Getting the source

`external/imgui` is a submodule reference, but this repo doesn't have a
committed `.gitmodules` yet, so `git submodule update --init` won't work
as-is. Until that's fixed, pull it manually at the pinned commit:

```bash
git clone https://github.com/ocornut/imgui.git external/imgui
cd external/imgui && git checkout ea83628438f5d29f2b576b694efd3e864fc00b1c
```

## Build

```bash
mkdir build && cd build
cmake ..
cmake --build . -j
```

## Run

Run the binary **from the repo root**, not from `build/` — it loads
things like the MobileSAM models using paths relative to the current
working directory:

```bash
./build/deskapp
```

Without the MobileSAM model files (see below), the app still runs fine;
the segment ("S") tool just no-ops with a "models not found" warning
printed to stderr.

## AI segmentation (MobileSAM) — optional

The segment tool needs two ONNX files in `models/`, which are gitignored
(too large/binary to commit) and must be generated locally:

- `models/mobile_sam_encoder.onnx` (+ `.onnx.data`)
- `models/mobile_sam_decoder.onnx`

`export_mobilesam.py` generates them from the official checkpoint. The
export needs specific, older dependency versions — the newest `torch`
breaks the ONNX export (dynamo exporter chokes on the decoder's dynamic
shapes), so pin exactly as below. Using [`uv`](https://github.com/astral-sh/uv)
for an isolated env:

```bash
# 1. Isolated venv for the export only (keeps torch off your normal setup)
uv venv .venv-export --python 3.12
source .venv-export/bin/activate

# 2. Matched dependency set — do not just `pip install torch`, latest breaks export
uv pip install "torch==2.2.2" "torchvision==0.17.2" "numpy<2" onnx onnxscript timm
uv pip install git+https://github.com/ChaoningZhang/MobileSAM.git

# 3. Official checkpoint (~40MB)
mkdir -p models
curl -L -o models/mobile_sam.pt \
  https://github.com/ChaoningZhang/MobileSAM/raw/master/weights/mobile_sam.pt

# 4. Export → models/mobile_sam_encoder.onnx, models/mobile_sam_decoder.onnx
python3 export_mobilesam.py
```

Once those two files exist, relaunch `./build/deskapp` from the repo
root and the segment tool works: left-click adds foreground points,
right-click background points, Enter extracts, Esc cancels.
