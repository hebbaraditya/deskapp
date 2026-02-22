#pragma once

#include "imgui.h"
#include "canvas_objects.h"
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// TextTool
//
// State machine with 3 modes:
//   IDLE     — T key or toolbar button activates text tool
//   PLACING  — user clicked canvas, typing new text inline
//   EDITING  — user double-clicked existing text, editing it inline
//
// Font loading:
//   Call TextTool::loadFonts() once after ImGui is initialised.
//   Each font in kFontManifest is baked at 5 sizes (32/48/64/96/128px).
//
// WASM note:
//   For WASM port: replace AddFontFromFileTTF → AddFontFromMemoryTTF.
//   Nothing else changes.
// ─────────────────────────────────────────────────────────────────────────────

// Global font table — filled by TextTool::loadFonts(), read everywhere
extern FontEntry g_fonts[kFontCount];

namespace TextTool {

enum class Mode { IDLE, PLACING, EDITING };

struct State {
    Mode   mode           = Mode::IDLE;
    int    editing_index  = -1;       // index into texts[], -1 when placing new
    char   edit_buf[512]  = {};       // inline editor buffer
    ImVec2 place_pos      = {0.f, 0.f}; // world-space position of new text
    TextStyle pending_style;           // style persists between placements
    bool   panel_open     = false;
    int    frames_in_mode = 0;         // frames spent in current mode
    double mode_enter_time = 0.0;      // ImGui::GetTime() when mode last changed
};

// ── Public API ────────────────────────────────────────────────────────────────

// Call once after ImGui context + backends are initialised, before main loop.
int loadFonts();

// Call every frame from RenderCanvas when text tool is active.
void update(std::vector<TextObject>& texts,
            State& state,
            ImDrawList* draw_list,
            ImVec2 canvas_min,
            ImVec2 canvas_size,
            ImVec2 pan,
            float zoom,
            ImVec2 mouse_world,
            bool in_canvas);

// Draw ALL text objects — call every frame regardless of active tool.
void drawAllText(const std::vector<TextObject>& texts,
                 ImDrawList* draw_list,
                 ImVec2 canvas_min,
                 ImVec2 canvas_size,
                 ImVec2 pan,
                 float zoom);

// Floating style panel (font, size, colors, stroke).
bool drawStylePanel(State& state, std::vector<TextObject>& texts);

// Helpers
ImVec2 measureText(const TextObject& obj);
bool   hitTest(const TextObject& obj, ImVec2 world_point);

} // namespace TextTool