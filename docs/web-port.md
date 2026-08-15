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

1. **[DONE] Prove the toolchain** — trivial ImGui+GLFW hello-world, built
   for web with our exact pinned `external/imgui` commit, running in a real
   browser. De-risks everything else before touching real app code.
2. Introduce a small platform abstraction (`platform.h` /
   `platform_native.cpp` / `platform_web.cpp`) for file open/save, swapped
   at compile time — replaces direct `tinyfiledialogs` calls in `main.cpp`.
3. Get the real app (`main.cpp` + `canvas_objects.h` + `transform_handles.h`
   + `text_tool.cpp/h`) compiling for web, with segmentation stubbed out.
4. ONNX Runtime Web integration for MobileSAM segmentation — hardest,
   most isolated piece, tackled on its own once everything else works.
5. Add an Emscripten CMake toolchain path so `cmake ..` (native) and
   `emcmake cmake ..` (web) both build from the same source tree.
6. Cloudflare Pages deployment, including COOP/COEP headers (needed for
   `SharedArrayBuffer`/threading) via a `_headers` file.

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

**Next:** stage 2, the platform abstraction for file dialogs.
