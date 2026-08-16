# Web port notes

Tracking progress on getting Yoinkboard running in-browser via WebAssembly,
alongside (not instead of) the native macOS build. See the plan in full
below; this file is the running log.

## Why WASM, not a JS/React rewrite

Dear ImGui + GLFW is one of the more web-portable native UI stacks —
Emscripten support is official and well-trodden, since immediate-mode
rendering over OpenGL maps directly onto WebGL. That means most of the
existing C++ (`canvas_objects.h`, `transform_handles.h`, `text_tool.cpp`,
the ImGui rendering path) should port largely unchanged. The real seams are
narrow: file dialogs, the main loop, background threading, and ONNX
inference. See "Seams" below.

## Plan (staged — each stage merges to `main` once verified, no big-bang merge)

1. **[DONE]** Prove the toolchain — trivial ImGui+GLFW hello-world, built
   for web with our exact pinned `external/imgui` commit, running in a real
   browser. De-risks everything else before touching real app code.
2. **[DONE]** Platform abstraction (`platform.h` / `platform_native.cpp` /
   `platform_web.cpp`) for file open/save, swapped at compile time —
   replaces direct `tinyfiledialogs` calls in `main.cpp`.
3. **[DONE]** Get the real app compiling *and running* for web (segmentation
   stubbed out), including the CMake Emscripten toolchain path — folded into
   this stage rather than done separately, since there was no way to
   meaningfully test one without the other anyway.
4. **[DONE]** ONNX Runtime Web integration for MobileSAM segmentation.
5. **[LIVE]** Cloudflare Pages deployment — **https://yoinkboard.pages.dev**
   is up and confirmed working end-to-end in production (image load +
   real segmentation, visually verified by the user). Remaining polish,
   not blockers: connect GitHub for auto-deploy on push (currently manual
   `wrangler pages deploy`), COOP/COEP headers (needed for real
   `std::thread`/`SharedArrayBuffer` support down the line — not required
   for what's working today), self-hosting onnxruntime-web's dist files
   instead of the CDN this currently depends on.

## Stage 5 log — deployment (in progress)

**Model hosting size problem:** Cloudflare Pages caps individual files at
25 MiB (Cloudflare's own documented limit, confirmed against the current
docs, not assumed). The fp32 encoder doesn't fit either way it's counted —
`mobile_sam_encoder.onnx` (~26.7MB) and its external-data sibling
`mobile_sam_encoder.onnx.data` (~26.6MB) both individually exceed it. The
decoder (~15.8MB) is fine as-is.

**fp16 quantization attempt — abandoned, real bug, not an accuracy
tradeoff.** Tried converting the encoder to fp16 (`quantize_fp16.py`,
using `onnxconverter_common.float16.convert_float_to_float16`) to both
halve the size and — since the result comfortably clears the threshold
where ONNX embeds vs. externalizes weights — collapse it back into a
single ~14.2MB file, avoiding the need for separate object storage
entirely. The conversion completed without error and produced a
plausible-looking file, but it doesn't load: ONNX Runtime Web fails
session creation with

```
Can't create a session. ERROR_CODE: 1, ERROR_MESSAGE:
.../onnxruntime/core/graph/graph_utils.cc:30 int
onnxruntime::graph_utils::GetIndexFromName(...) itr != node_args.end()
was false. Attempting to get index by a name which does not exist:
InsertedPrecisionFreeCast_/encoder/neck/neck.3/Constant_output_0 for
node: /encoder/layers.1/blocks.0/attn/norm/Mul/SimplifiedLayerNormFusion/
```

i.e. the conversion tool's cast-node cleanup pass corrupted the graph —
it inserted a reference to a node it never actually wired up, specifically
around a `SimplifiedLayerNormFusion` node (an ONNX-Runtime-specific fused
op, from the TinyViT/MobileSAM encoder's LayerNorm pattern). Retried with
`disable_shape_infer=True` (a documented workaround for similar
`onnxconverter_common` graph-corruption issues) — identical error,
reproducible, not a fluke. This is a real incompatibility between that
library and this model's graph structure, not a quality/accuracy
question — the model never gets far enough to load, let alone segment
anything. Not worth more time chasing (e.g. trying ONNX Runtime's own
`onnxruntime.transformers` float16 converter instead, which is aware of
ORT-specific fused ops) given R2 is a known-good, already-planned
alternative. `quantize_fp16.py` is left in the repo in case it's worth
revisiting with a different toolchain later; `main.cpp` reverted to fp32
on both platforms.

**R2 setup — done, verified working.** Host the two large encoder files
on Cloudflare R2 (Cloudflare's own recommendation for exactly this case —
"file too big for Pages" — and R2's zero-egress-fee pricing fits an
asset-heavy WASM app well regardless). Decoder + everything else still
ships from the single Pages deploy.

Steps (all via `wrangler`, installed with `npm install -g wrangler`):
1. `wrangler login` — OAuth via browser. First attempt's auto-opened
   browser tab didn't actually load the URL for some reason; opening the
   printed URL directly with `open "<url>"` worked. The login process
   also appears to time out/exit if left too long before the OAuth
   callback completes — if `wrangler whoami` still says unauthenticated,
   just re-run `wrangler login`.
2. `wrangler r2 bucket create yoinkboard-models` — failed the first time
   with `[code: 10042] Please enable R2 through the Cloudflare Dashboard`.
   R2 needs a one-time account-level activation (accepting its terms) via
   the dashboard — not something the CLI can do. Once enabled, the same
   command succeeded.
3. Uploaded both encoder files. **Gotcha:** `wrangler r2 object put`
   defaults to a *local* simulated bucket in this wrangler version unless
   you pass `--remote` — the first upload attempt silently succeeded
   against local-only storage. Redid both with `--remote`:
   ```bash
   wrangler r2 object put yoinkboard-models/mobile_sam_encoder.onnx \
     --file models/mobile_sam_encoder.onnx \
     --content-type application/octet-stream --remote
   wrangler r2 object put yoinkboard-models/mobile_sam_encoder.onnx.data \
     --file models/mobile_sam_encoder.onnx.data \
     --content-type application/octet-stream --remote
   ```
   Verified byte-for-byte via `wrangler r2 object get ... --remote --pipe
   | wc -c` matching the original file sizes exactly (`bucket info`'s
   aggregate stats lagged behind and briefly showed 0 objects — a stats
   propagation delay, not a real problem; the direct GET is the real
   check).
4. `wrangler r2 bucket dev-url enable yoinkboard-models` — public
   `r2.dev` URL, free, works entirely via CLI (no dashboard click needed
   for this part, unlike enabling R2 itself). Got:
   `https://pub-1412e4d9fcf2440abe938992a8d8dda4.r2.dev`
5. **CORS.** Public objects are fetchable via `curl` by default, but
   browsers separately enforce CORS on cross-origin `fetch()` — the
   response had no `Access-Control-Allow-Origin` header until configured.
   `wrangler r2 bucket cors set` needs a JSON file shaped like
   Cloudflare's own R2 API (`{"rules": [{"allowed": {"methods": [...],
   "origins": [...], "headers": [...]}, "maxAgeSeconds": ...}]}`), *not*
   the plain S3-style `[{"AllowedOrigins": [...], ...}]` shape that seemed
   like the obvious first guess — wrangler rejects that with "must contain
   a 'rules' array". Set `origins: ["*"]`, `methods: ["GET", "HEAD"]`
   (public model weights, no reason to restrict). Verified with
   `curl -I -H "Origin: http://localhost:8000" <url>` showing
   `Access-Control-Allow-Origin: *` in the response.
6. `main.cpp`'s web encoder path now points at the R2 URL directly
   (`#ifdef __EMSCRIPTEN__`), native unchanged (still a local relative
   path). Rebuilt web, removed the local encoder files from the test
   server's directory entirely (so there's no way to accidentally pass by
   falling back to a local copy), and reconfirmed segmentation actually
   works fetching the encoder cross-origin from `r2.dev` while the app
   itself was served from `localhost:8000` — as close to the real
   production topology (Pages domain + R2 domain, two different origins)
   as local testing gets. Confirmed working, visually, by the user.

**Pages deploy — done, live.** Deployed the app itself (decoder +
WASM/JS/fonts) to Cloudflare Pages:

1. `wrangler pages project create yoinkboard --production-branch=web-port`
   — creates the project; picked `web-port` as the production branch
   since that's what we're deploying from for now (see the earlier
   decision to not merge to `main` yet).
2. Staged a clean deploy directory (`dist-web/`, gitignored) containing
   only the actual served assets — `yoinkboard.html`/`.js`/`.wasm`/`.data`
   plus `models/mobile_sam_decoder.onnx` — deliberately excluding
   CMake's own build files (`CMakeCache.txt`, `CMakeFiles/`, `Makefile`)
   that live alongside them in `build-web/`. Also copied
   `yoinkboard.html` → `index.html`, since Pages serves `index.html` by
   default at the root and our emcc-generated shell isn't named that.
3. `wrangler pages deploy dist-web --project-name=yoinkboard` — first
   deploy. Warned about uncommitted changes in the working directory
   (harmless here, was mid-commit-cycle; `--commit-dirty=true` silences
   it if it comes up again).
4. **Live at https://yoinkboard.pages.dev** — confirmed working in
   production by the user: real image load, real MobileSAM segmentation,
   Pages + R2 as two separate real domains (not simulated locally
   anymore).

**Remaining, not blockers:**
- Connect GitHub repo in the Cloudflare dashboard for auto-deploy on
  push — needs a manual OAuth-ish GitHub App install click in the
  dashboard, not CLI-automatable. Redeploys are `wrangler pages deploy
  dist-web --project-name=yoinkboard` (rebuild web, restage `dist-web/`,
  redeploy) until that's set up.
- COOP/COEP headers, self-hosting onnxruntime-web instead of CDN (see
  the stage 5 plan bullet above).

## Seams (native → web)

| Seam | Native | Web |
|---|---|---|
| Main loop | GLFW `while` loop | `emscripten_set_main_loop()` |
| File open/save | `tinyfiledialogs` | Browser File API / drag-drop |
| AI inference | ONNX Runtime C++ API | ONNX Runtime Web (same `.onnx` files, different binding) |
| Background thread (segment encode) | `std::thread` | Web Worker / pthreads (needs `SharedArrayBuffer` → COOP/COEP headers) |
| Windowing/GL context | GLFW native | GLFW's Emscripten port (`--use-port=contrib.glfw3`) |

Everything else (canvas object model, transform handles, text tool,
segmenter's actual logic minus the ORT calls) needs no changes.

## Stage 1 log — toolchain proof (done)

- Installed Emscripten SDK via `emsdk` (not committed to this repo — lives
  at `~/workspace/emsdk`, sibling to this project, on this machine). To set
  up on a fresh machine:
  ```bash
  git clone https://github.com/emscripten-core/emsdk.git
  cd emsdk
  ./emsdk install latest
  ./emsdk activate latest
  source ./emsdk_env.sh   # add to shell profile for persistence
  ```
  Note: `emsdk install` requires Python 3.10+; macOS system Python is often
  older (3.9), so point `PATH` at a newer `python3` (e.g. via
  `brew install python@3.12`, using the `libexec/bin` symlinks) for the
  install step only — the SDK bundles its own Python afterward.
- Built ImGui's official `examples/example_glfw_opengl3/Makefile.emscripten`
  against our exact pinned `external/imgui` commit
  (`ea83628438f5d29f2b576b694efd3e864fc00b1c`) — compiled clean (only
  harmless deprecation warnings from `imgui_demo.cpp`, which we don't use),
  produced `index.html` + `index.js` + `index.wasm` (~1MB wasm), served via
  `python3 -m http.server`, confirmed rendering at 60 FPS in Chrome.
- This proves: our pinned ImGui version, the GLFW backend, and the OpenGL3
  backend are all Emscripten-compatible as-is. No changes needed there.

## Stage 2 log — platform abstraction (done)

- Added `Platform::OpenImageFile(callback)` / `Platform::SaveFile(name,
  bytes, size)` — callback-based on *both* platforms, since native dialogs
  (synchronous) and browser file APIs (asynchronous) need to fit the same
  call shape. Native wraps `tinyfiledialogs`; web uses `EM_JS` + a hidden
  `<input type=file>` for open, a Blob+download-link for save.
- `LoadTextureFromFile(path)` → `LoadTextureFromMemory(bytes)` — there's no
  meaningful filesystem path for a browser-picked file, so the interface
  hands back raw bytes on both platforms; decoding (`stbi_load_from_memory`)
  is fully shared.
- `ExportImageAsPNG` now encodes to an in-memory buffer
  (`stbi_write_png_to_func`) before handing bytes to `Platform::SaveFile` —
  PNG encoding is shared code now, not duplicated per platform.
- Verified on native: compiles clean, Image load + Export PNG both work
  end-to-end through the new abstraction.
- `platform_web.cpp` written but not yet tested at this point — no web
  build of the real app existed yet.

## Stage 3 log — real app running in-browser (done)

This was the big one. In order:

1. **Decoupled `Segmenter` from ONNX Runtime headers.** `segmenter.h` used
   to `#include <onnxruntime_cxx_api.h>` directly and hold `Ort::Env` /
   `Ort::SessionOptions` / `Ort::Session` as plain members — meaning the
   header itself wouldn't even parse without ONNX Runtime installed, which
   isn't true for the web build. Pimpl'd it: `segmenter.h` now declares
   `struct Impl;` + `std::unique_ptr<Impl> impl_;` with zero ORT
   dependency, and `segmenter.cpp` defines `Segmenter::Impl` with the real
   ORT types inside. Public API unchanged, so `main.cpp` needed zero edits
   for this. Verified on native: both models still load correctly
   afterward.
2. **`segmenter_web.cpp`** — a second, separate implementation of the same
   `Segmenter` class (same pattern as `platform_native.cpp`/
   `platform_web.cpp`), currently a stub: `loadModels()` always returns
   `false`. `main.cpp` already handles "models not found" gracefully (it's
   the same behavior as native without the model files present), so this
   needed no application-logic changes at all — just an alternate compiled
   backend.
3. **Main loop.** Wrapped the existing `while (!glfwWindowShouldClose(...))`
   with `EMSCRIPTEN_MAINLOOP_BEGIN`/`END` (official Dear ImGui technique —
   copied `emscripten_mainloop_stub.h` into this repo rather than reaching
   into the `external/imgui` submodule for it). No-op on native; on web it
   turns the loop into something `emscripten_set_main_loop()` can drive.
4. **CMakeLists.txt** — branched on the `EMSCRIPTEN` variable (set
   automatically by `emcmake`): native path unchanged (glfw3/OpenGL/ONNX
   Runtime via `find_package`/`link_directories`), web path compiles
   `platform_web.cpp` + `segmenter_web.cpp` instead of the native
   equivalents, uses `--use-port=contrib.glfw3` instead of `find_package
   (glfw3)`, skips ONNX Runtime linking entirely, and `--preload-file
   fonts@fonts` so the existing `fopen("fonts/...")` calls in
   `TextTool::loadFonts()` resolve unchanged against Emscripten's virtual
   filesystem. `main.cpp` and `text_tool.cpp` needed no `#ifdef`s for any
   of this.
5. **First real build attempt: linked successfully**, but loaded to a
   blank black canvas. Console showed the actual bug: GLSL shader compile
   errors — `main()` unconditionally hardcoded desktop `#version 330` +
   OpenGL 3.3 Core context hints for *all* platforms. `imgui_impl_opengl3.h`
   auto-detects `IMGUI_IMPL_OPENGL_ES2` under `__EMSCRIPTEN__` internally
   (GLES2/WebGL1), but that only changes how the backend renders — the
   `glsl_version` string passed into `ImGui_ImplOpenGL3_Init()` still has to
   match, or shader compilation fails outright. Fixed by branching the GL
   context hints + `glsl_version` on `__EMSCRIPTEN__` (ES2 · `#version 100`
   · `GLFW_OPENGL_ES_API` on web, desktop GL 3.3 Core unchanged natively),
   matching Dear ImGui's own reference example. Also added
   `ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas")` for
   proper resize/focus handling against the canvas element.
6. **Rebuilt — the real app rendered correctly in Chrome**: toolbar, tool
   sidebar (V/H/S/T), canvas grid, all matching the native build. Verified
   visually, not just "no console errors."

Native build re-verified working after every one of the above changes —
nothing here touched native behavior.

## Stage 4 log — MobileSAM segmentation running in-browser (done)

The core problem going in: `ort.InferenceSession.run()` in ONNX Runtime Web
is a JS Promise (async) — there's no synchronous WASM-native ORT API to
link against like native's `onnxruntime_cxx_api.h`. To keep `Segmenter`'s
public API identical on both platforms (the promise stage 3's commit
message made), used **Asyncify** (`-sASYNCIFY=1`): lets a JS `async
function`, declared via `EM_ASYNC_JS`, be called from C++ as an ordinary
blocking call — the WASM stack transparently unwinds while the Promise is
pending and resumes when it resolves.

1. **Extracted `Segmenter`'s pure-math methods** (`preprocess()` — RGBA to
   normalised tensor, `upscaleMask()` — bilinear mask resize) into a new
   shared `segmenter_math.cpp`, compiled into both builds. Neither method
   touches ORT types, so this was pure code motion, no logic changes.
   Model I/O shape constants (`kSAMSize`/`kEmbedC/H/W`) moved from
   `segmenter.cpp` into `segmenter.h` so both `segmenter.cpp` and
   `segmenter_web.cpp` share one definition instead of redeclaring them.
2. **Real `segmenter_web.cpp`**, replacing the stage-3 stub: three
   `EM_ASYNC_JS` bridge functions —
   - `web_load_models(encoder_url, decoder_url)` — `ort.InferenceSession
     .create()` for both, stashed on `Module`. Returns a bitmask so
     `loadModels()` can report per-model failures like native's two
     separate `try`/`catch` blocks do.
   - `web_run_encoder(input_ptr, input_len)` — wraps the WASM-memory
     region at `input_ptr` directly as a `Float32Array` view (no copy),
     runs the encoder, copies the output embedding into a `_malloc`'d
     WASM buffer, returns that pointer for C++ to read and free.
   - `web_run_decoder(...)` — same pattern for point-coords/labels in,
     mask+IoU out (scalar outputs via WASM-memory out-pointers, i.e. JS
     writes into `HEAP32`/`HEAPF32` at addresses C++ passed in).

   `embedding_`, `image_encoded_`, `image_w_`/`image_h_` are plain
   `Segmenter` members (no ORT dependency) already, so they just work
   as the interchange buffer between `encodeImage()` and `decode()`
   on web exactly like they already did on native.
3. **onnxruntime-web loaded via CDN** (`shell_minimal.html`), pinned to
   `1.27.0` (verified against the actual published npm version, not
   guessed) rather than `@latest`, so a future upstream release can't
   silently change behavior underneath us. Self-hosting this is a stage-5
   TODO — CDN dependency is fine for local testing, not for a real deploy.
4. **First real build linked clean**, but two runtime bugs surfaced only
   once actually exercised in-browser (confirmed via the user checking
   DevTools console each time — no Accessibility permission on this
   machine to drive DevTools automatically):
   - `Module._malloc is not a function`, thrown from `platform_web.cpp`'s
     file-open flow. Root cause, confirmed by inspecting the actual
     generated `yoinkboard.js`: `_malloc`/`_free`/`HEAPU8`/`HEAPF32`/
     `HEAP32` exist as bare closure-scope variables inside the generated
     JS, **not** also mirrored onto the `Module` object — unlike
     `EMSCRIPTEN_KEEPALIVE`-exported C functions (like our own
     `yoinkboard_web_image_loaded`), which *are* attached to `Module`.
     `EM_JS`/`EM_ASYNC_JS` bodies are inlined into that same closure
     scope, so the fix was simply dropping the `Module.` prefix
     everywhere in `platform_web.cpp` and `segmenter_web.cpp`.
   - `thread constructor failed` / `Aborted(native code called abort())`,
     crashing the moment segmentation was triggered. `main.cpp` spawns a
     real `std::thread` to run the encode step off the render thread —
     Emscripten's `std::thread` needs pthread support plus COOP/COEP
     cross-origin isolation headers, neither of which are set up (that's
     what stage 5's `_headers` file is for). Fixed by *not* spawning a
     thread on web at all: `Segmenter::encodeImage()` is already
     non-blocking-to-the-browser via Asyncify (it's backed by a JS
     Promise under the hood), so `main.cpp` now branches on
     `__EMSCRIPTEN__` to call it directly instead of wrapping it in
     `std::thread(...)`. Native path (real thread) untouched.
5. **Rebuilt — real segmentation confirmed working end-to-end**, visually,
   in Chrome: loaded a 5120×2880 photo, pressed S, clicked the subject —
   console showed `[Segmenter] Image encoded (5120x2880)` then
   `[Segmenter] Decoded 5120x2880 IoU=1.020`, and the blue mask overlay
   correctly followed the boundary between the subject and background.
   Genuine client-side MobileSAM inference, no server involved.

Native build re-verified working after every change in this stage too —
the `segmenter_math.cpp` extraction and the `std::thread` branch both
touch shared code, so this mattered more than usual here.

**Known rough edges, left for stage 5 / later polish (not blockers):**
- `g_segmenter.loadModels(...)` runs at startup and (via Asyncify) blocks
  the canvas from rendering *anything* until it resolves — fine for
  testing, bad UX for a real deploy (blank canvas during model download).
  Should become lazy (load on first Segment-tool use) with a visible
  loading state.
- A harmless console warning — `emscripten_set_main_loop_timing: Cannot
  set timing mode... call emscripten_set_main_loop first` — fires once
  during the `ImGui_ImplGlfw_InstallEmscriptenCallbacks` canvas-resize
  hookup, before the main loop technically exists yet (still inside the
  Asyncify-suspended `loadModels()` call at that point). Doesn't affect
  rendering or functionality; not yet root-caused further.
- CDN dependency for onnxruntime-web (see point 3 above).

**Next:** stage 5, Cloudflare Pages deployment.
