// NOTE: written against standard, well-documented Emscripten patterns
// (EM_JS + malloc/free bridging), but not yet compiled or run — there's no
// web build of the real app yet (that's web-port stage 3). First real test
// happens then; expect to come back and adjust the exact JS glue/linker
// flags (e.g. -sEXPORTED_RUNTIME_METHODS for _malloc/_free) once it does.

#include "platform.h"

#include <emscripten.h>
#include <cstring>

namespace Platform {

namespace {
    // Stashed between triggering the browser's file picker and the async
    // callback firing once the user actually picks something (or never, if
    // they cancel — browsers don't tell JS about a cancelled
    // <input type=file>, so there's no "on_cancelled" path here).
    BytesLoadedFn g_pending_on_loaded;
} // namespace

// Called from JS (see web_open_image_file below) once the user's chosen
// file has been read into a byte buffer in WASM memory. extern "C" so the
// symbol isn't name-mangled and JS can call it directly as
// Module._yoinkboard_web_image_loaded(...).
extern "C" void EMSCRIPTEN_KEEPALIVE
yoinkboard_web_image_loaded(unsigned char* data, int size)
{
    if (g_pending_on_loaded) {
        g_pending_on_loaded(data, (size_t)size);
    }
    g_pending_on_loaded = nullptr;
}

// Creates (once) a hidden <input type=file>, and on each call clears its
// selection and re-opens the native browser file picker. When the user
// picks a file, reads it as an ArrayBuffer, copies the bytes into WASM
// memory, and calls back into yoinkboard_web_image_loaded() — mirroring
// what OpenImageFile's callback expects, just asynchronously.
EM_JS(void, web_open_image_file, (), {
    if (!Module.__yoinkboardFileInput) {
        var input = document.createElement("input");
        input.type = "file";
        input.accept = "image/png,image/jpeg,image/bmp";
        input.style.display = "none";
        document.body.appendChild(input);
        Module.__yoinkboardFileInput = input;
    }
    var input = Module.__yoinkboardFileInput;
    input.value = ""; // so picking the same file twice still fires "change"
    input.onchange = function() {
        var file = input.files[0];
        if (!file) return;
        var reader = new FileReader();
        reader.onload = function(e) {
            var bytes = new Uint8Array(e.target.result);
            var ptr = Module._malloc(bytes.length);
            Module.HEAPU8.set(bytes, ptr);
            Module._yoinkboard_web_image_loaded(ptr, bytes.length);
            Module._free(ptr);
        };
        reader.readAsArrayBuffer(file);
    };
    input.click();
});

void OpenImageFile(BytesLoadedFn on_loaded)
{
    g_pending_on_loaded = on_loaded;
    web_open_image_file();
}

// Triggers a browser download of `size` bytes at `data` (WASM memory),
// named `name` — the standard Blob + temporary-<a download> trick, since
// browsers don't let a page write to an arbitrary chosen filesystem path.
EM_JS(void, web_save_file,
      (const char* name, const unsigned char* data, int size), {
    var bytes = HEAPU8.subarray(data, data + size);
    var blob = new Blob([bytes], { type: "image/png" });
    var url = URL.createObjectURL(blob);
    var a = document.createElement("a");
    a.href = url;
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
});

bool SaveFile(const std::string& suggested_name,
             const unsigned char* data, size_t size)
{
    web_save_file(suggested_name.c_str(), data, (int)size);
    return true; // fire-and-forget — browsers give no completion signal back
}

} // namespace Platform
