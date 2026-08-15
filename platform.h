#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Platform — the narrow seam between the native (macOS) and web (Emscripten)
// builds. Everything else in this codebase (canvas objects, transform
// handles, text tool, the actual segmentation math) is portable as-is; file
// I/O is the one place native and web are fundamentally different, not just
// differently-implemented:
//
//   - Native file dialogs (tinyfiledialogs) are SYNCHRONOUS — they block
//     until the user picks a path or cancels, then return.
//   - Browser file APIs are ASYNCHRONOUS — triggering a picker returns
//     immediately, and the chosen file's bytes only become available later,
//     via a callback, once the browser gets around to it.
//
// So this interface is callback-based throughout, even though the native
// implementation happens to be able to call back before returning. Calling
// code must never assume synchronous completion — always continue in the
// callback, not after the call site.
//
// Two more differences worth knowing about going in:
//   - There's no such thing as a "file path" for a file the user picked in
//     a browser, so OpenImageFile hands back raw bytes directly rather than
//     a path to re-read — decoding (stbi_load_from_memory) is fully shared
//     between native and web either way.
//   - Browsers can't write to an arbitrary chosen filesystem path (broadly
//     compatible-wise); "saving" there means triggering a download. So
//     SaveFile takes already-encoded bytes and a suggested filename, not a
//     format to encode — PNG encoding (stb_image_write) is fully shared.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <functional>
#include <string>

namespace Platform {

// Prompt the user to pick an image file. Invokes `on_loaded` with the raw,
// still-encoded (e.g. PNG/JPEG) file bytes once one is picked. Never
// invoked if the user cancels the picker.
using BytesLoadedFn = std::function<void(const unsigned char* data, size_t size)>;
void OpenImageFile(BytesLoadedFn on_loaded);

// Offer already-encoded bytes (e.g. from stbi_write_png_to_func) to the
// user as a downloadable/saveable file. `suggested_name` is a hint only —
// native pre-fills the save dialog with it, web uses it as the downloaded
// file's name.
//
// Returns true if native definitively saved the file (dialog confirmed +
// write succeeded). On web this always returns true once the download is
// triggered — browsers give JS no signal for whether/where the user
// actually saved it afterward.
bool SaveFile(const std::string& suggested_name,
             const unsigned char* data, size_t size);

} // namespace Platform
