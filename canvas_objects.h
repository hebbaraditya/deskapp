#pragma once

#include "imgui.h"
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Font manifest — matches exactly what's in the fonts/ folder
// Order here = order in the UI font dropdown
// Add new fonts here + drop the .ttf into fonts/ — nothing else to change
// ─────────────────────────────────────────────────────────────────────────────
struct FontManifest {
    const char* name;      // display name in UI
    const char* filename;  // path relative to fonts/
};

static constexpr FontManifest kFontManifest[] = {
    { "Bebas Neue",        "BebasNeue-Regular.ttf"              },
    { "Anton",             "Anton-Regular.ttf"                  },
    { "Bungee",            "Bungee-Regular.ttf"                 },
    { "Patua One",         "PatuaOne-Regular.ttf"               },
    { "Rubik Mono",        "RubikMonoOne-Regular.ttf"           },
    { "Young Serif",       "YoungSerif-Regular.ttf"             },
    { "Roboto Black",      "Roboto-Black.ttf"                   },
    { "Roboto Bold",       "Roboto-Bold.ttf"                    },
    { "Roboto Regular",    "Roboto-Regular.ttf"                 },
    { "Indie Flower",      "IndieFlower-Regular.ttf"            },
    { "Kavoon",            "Kavoon-Regular.ttf"                 },
    { "Londrina Outline",  "LondrinaOutline-Regular.ttf"        },
    { "Rubik Moonrocks",   "RubikMoonrocks-Regular.ttf"         },
    { "Pixelify Sans",     "PixelifySans-VariableFont_wght.ttf" },
};
static constexpr int kFontCount = (int)(sizeof(kFontManifest) / sizeof(kFontManifest[0]));

// ─────────────────────────────────────────────────────────────────────────────
// FontEntry — one loaded font baked at multiple sizes
//
// ImGui bakes fonts into a texture atlas at startup. You can't change the baked
// size at runtime without rebuilding the atlas (expensive). The solution:
// pre-bake 5 sizes and pick the best one via bestFor().
// draw_list->AddText(font, custom_size, ...) can then scale between baked sizes
// with acceptable quality (scaling down = fine, scaling up = slightly blurry).
//
// This is also the correct WASM approach — no filesystem access at runtime,
// fonts are loaded once and embedded. When we port to Emscripten we'll switch
// AddFontFromFileTTF -> AddFontFromMemoryTTF with xxd-embedded byte arrays,
// and nothing else changes.
// ─────────────────────────────────────────────────────────────────────────────
struct FontEntry {
    std::string name;
    ImFont*     sizes[5] = {};   // baked at kBakedSizes[0..4]
    bool        loaded   = false;

    static constexpr float kBakedSizes[5] = { 32.f, 48.f, 64.f, 96.f, 128.f };

    // Pick the smallest baked size that is >= 75% of requested.
    // This means we scale down slightly rather than up = better quality.
    ImFont* bestFor(float requested) const {
        ImFont* best = nullptr;
        for (int i = 4; i >= 0; --i)
            if (sizes[i] && kBakedSizes[i] >= requested * 0.75f)
                best = sizes[i];
        if (!best)
            for (int i = 4; i >= 0; --i)
                if (sizes[i]) { best = sizes[i]; break; }
        return best;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Transform — shared by CanvasImage and TextObject
// Having one struct means TransformHandles works on both identically
// ─────────────────────────────────────────────────────────────────────────────
struct Transform {
    ImVec2 position = {0.f, 0.f};  // world-space top-left
    float  scale    = 1.f;          // uniform scale multiplier
    float  rotation = 0.f;          // degrees clockwise (reserved, phase 2)
    bool   mirrored = false;         // horizontal flip
};

// ─────────────────────────────────────────────────────────────────────────────
// CanvasImage
// ─────────────────────────────────────────────────────────────────────────────
struct CanvasImage {
    unsigned int texture_id = 0;
    ImVec2       size       = {0.f, 0.f};  // original pixel dimensions
    Transform    transform;
    bool         selected  = false;
    std::string  filename;
    std::vector<unsigned char> pixels;     // RGBA, kept for export + mask ops
};

// ─────────────────────────────────────────────────────────────────────────────
// TextStyle — all visual properties for a TextObject
// ─────────────────────────────────────────────────────────────────────────────
struct TextStyle {
    int    font_index   = 0;              // index into g_fonts[]
    float  font_size    = 64.f;           // world-space display size in pixels
    ImVec4 fill_color   = {1, 1, 1, 1};  // white fill (classic meme default)
    ImVec4 stroke_color = {0, 0, 0, 1};  // black stroke
    float  stroke_width = 3.f;            // stroke thickness at zoom=1
};

// ─────────────────────────────────────────────────────────────────────────────
// TextObject — live text drawn via ImDrawList every frame
// Never baked to texture → always re-editable on double-click
// ─────────────────────────────────────────────────────────────────────────────
struct TextObject {
    std::string content;                   // UTF-8 text
    Transform   transform;
    TextStyle   style;
    bool        selected    = false;
    ImVec2      cached_size = {0.f, 0.f};  // pixel size of text block, updated each frame
};

// ─────────────────────────────────────────────────────────────────────────────
// HandleId — which corner/handle is being dragged
// ─────────────────────────────────────────────────────────────────────────────
enum class HandleId : int {
    None        = -1,
    TopLeft     = 0,
    TopRight    = 1,
    BottomRight = 2,
    BottomLeft  = 3,
    Rotate      = 4,   // circle above top edge
};

// ─────────────────────────────────────────────────────────────────────────────
// DragState — persists across frames for any active handle drag
// One instance lives inside each selectable object
// ─────────────────────────────────────────────────────────────────────────────
struct DragState {
    HandleId active      = HandleId::None;
    ImVec2   start_mouse = {0.f, 0.f};
    float    start_scale = 1.f;
    float    start_rot   = 0.f;
    ImVec2   start_pos   = {0.f, 0.f};
    ImVec2   start_size  = {0.f, 0.f};
};