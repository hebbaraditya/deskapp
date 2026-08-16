# Yoinkboard

**Try it live: https://yoinkboard.pages.dev** — runs entirely in your
browser via WebAssembly, no server, no account. MobileSAM segmentation
runs client-side too (ONNX Runtime Web).

Tried to clone the dingboard. Got most of the way there, then started
preparing for interviews. Picked it back up later — native macOS build
(this README covers that below) plus a full web port (see
[`docs/web-port.md`](docs/web-port.md) for how that was built, staged
commit by commit).

MobileSAM segmentation runs locally on both platforms — client-side in
the browser via ONNX Runtime Web, no server involved, same as native.
Still on the wishlist: a proper background-removal model, maybe the one
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
./build/yoinkboard
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

Once those two files exist, relaunch `./build/yoinkboard` from the repo
root and the segment tool works: left-click adds foreground points,
right-click background points, Enter extracts, Esc cancels.

## Web build

Live at **https://yoinkboard.pages.dev**, built from the `web-port`
branch. Same source tree as native, no fork — see
[`docs/web-port.md`](docs/web-port.md) for the full staged build-out
(toolchain proof → platform abstraction → real app in-browser → MobileSAM
via ONNX Runtime Web → Cloudflare deployment), including the real bugs
hit and how they were fixed along the way.

Building it yourself needs the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html):

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh

cd ../yoinkboard
mkdir build-web && cd build-web
emcmake cmake ..
cmake --build . -j
python3 -m http.server 8000   # then open http://localhost:8000/yoinkboard.html
```

The web build's MobileSAM encoder (too large for Cloudflare Pages' 25MB
file cap) is fetched from Cloudflare R2 at build time rather than bundled
— native's local `models/` files aren't needed to build for web, only to
run the native binary.

Redeploying (until GitHub auto-deploy is wired up — currently manual):

```bash
mkdir -p dist-web/models
cp build-web/yoinkboard.{html,js,wasm,data} dist-web/
cp dist-web/yoinkboard.html dist-web/index.html
cp models/mobile_sam_decoder.onnx dist-web/models/
wrangler pages deploy dist-web --project-name=yoinkboard
```
