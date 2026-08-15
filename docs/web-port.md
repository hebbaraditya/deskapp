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
4. ONNX Runtime Web integration for MobileSAM segmentation — hardest,
   most isolated piece, tackled on its own now that everything else works.
5. Cloudflare Pages deployment, including COOP/COEP headers (needed for
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

**Next:** stage 4, ONNX Runtime Web integration for MobileSAM.
