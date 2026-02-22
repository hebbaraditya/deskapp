#include "text_tool.h"
#include "transform_handles.h"
#include "imgui.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <unordered_map>

// ── Global font table ─────────────────────────────────────────────────────────
FontEntry g_fonts[kFontCount];

namespace TextTool {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// World → screen
static ImVec2 w2s(ImVec2 wp, ImVec2 canvas_min, ImVec2 canvas_size,
                   ImVec2 pan, float zoom)
{
    return {
        canvas_min.x + canvas_size.x * 0.5f + (wp.x + pan.x) * zoom,
        canvas_min.y + canvas_size.y * 0.5f + (wp.y + pan.y) * zoom,
    };
}

// Draw one text string with optional stroke, at a given SCREEN position,
// using a given baked ImFont* and display size.
static void drawTextWithStroke(ImDrawList* dl,
                                ImFont* font, float font_size,
                                ImVec2 screen_pos,
                                const char* text,
                                ImVec4 fill,  ImVec4 stroke,
                                float stroke_w)
{
    ImU32 fill_col   = ImGui::ColorConvertFloat4ToU32(fill);
    ImU32 stroke_col = ImGui::ColorConvertFloat4ToU32(stroke);

    if (stroke_w > 0.f) {
        // 8-direction offset stroke — fast, looks great
        static const float dirs[8][2] = {
            {-1,-1},{0,-1},{1,-1},
            {-1, 0},       {1, 0},
            {-1, 1},{0, 1},{1, 1}
        };
        for (auto& d : dirs) {
            dl->AddText(font, font_size,
                { screen_pos.x + d[0] * stroke_w,
                  screen_pos.y + d[1] * stroke_w },
                stroke_col, text);
        }
    }
    dl->AddText(font, font_size, screen_pos, fill_col, text);
}

// Draw a blinking cursor at the end of text
static void drawCursor(ImDrawList* dl, ImFont* font, float font_size,
                        ImVec2 screen_pos, const char* text, float zoom)
{
    // Blink at ~1Hz
    float t = ImGui::GetTime();
    if (fmodf(t, 1.0f) > 0.5f) return;

    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, text);
    ImVec2 cursor_top    = { screen_pos.x + text_size.x + 2.f, screen_pos.y };
    ImVec2 cursor_bottom = { cursor_top.x, screen_pos.y + font_size };
    dl->AddLine(cursor_top, cursor_bottom, IM_COL32(255, 255, 255, 220), 2.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// measureText
// ─────────────────────────────────────────────────────────────────────────────
ImVec2 measureText(const TextObject& obj)
{
    if (obj.content.empty()) return {80.f, obj.style.font_size};

    int fi = std::max(0, std::min(obj.style.font_index, kFontCount - 1));
    ImFont* font = g_fonts[fi].loaded
                   ? g_fonts[fi].bestFor(obj.style.font_size)
                   : ImGui::GetIO().Fonts->Fonts[0];
    if (!font) font = ImGui::GetIO().Fonts->Fonts[0];

    float display_size = obj.style.font_size * obj.transform.scale;
    ImVec2 sz = font->CalcTextSizeA(display_size, FLT_MAX, 0.f,
                                     obj.content.c_str());
    // Divide back to "world size at scale=1" for consistency with Transform
    return { sz.x / obj.transform.scale, sz.y / obj.transform.scale };
}

// ─────────────────────────────────────────────────────────────────────────────
// hitTest — AABB in world space
// ─────────────────────────────────────────────────────────────────────────────
bool hitTest(const TextObject& obj, ImVec2 p)
{
    ImVec2 sz  = obj.cached_size; // updated last frame
    float  pad = 6.f;
    float  x0  = obj.transform.position.x - pad;
    float  y0  = obj.transform.position.y - pad;
    float  x1  = obj.transform.position.x + sz.x * obj.transform.scale + pad;
    float  y1  = obj.transform.position.y + sz.y * obj.transform.scale + pad;
    return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadFonts
// Searches several candidate directories so the app works whether run from
// build/, project root, or anywhere else. Never crashes on missing fonts.
// ─────────────────────────────────────────────────────────────────────────────
static bool fileExists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

int loadFonts()
{
    static const char* kPrefixes[] = { "fonts/", "../fonts/", "../../fonts/" };

    ImGuiIO& io = ImGui::GetIO();

    // Add the default embedded font FIRST so it stays as the UI font.
    // Custom fonts added afterwards do NOT replace it as the default.
    io.Fonts->AddFontDefault();

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 3;

    int loaded = 0;
    for (int fi = 0; fi < kFontCount; ++fi) {
        // Find which prefix contains this font file
        char found_path[512] = {};
        for (const char* prefix : kPrefixes) {
            char candidate[512];
            snprintf(candidate, sizeof(candidate), "%s%s",
                     prefix, kFontManifest[fi].filename);
            if (fileExists(candidate)) {
                strncpy(found_path, candidate, sizeof(found_path)-1);
                break;
            }
        }

        if (found_path[0] == '\0') {
            printf("[TextTool] Font not found: %s\n", kFontManifest[fi].filename);
            g_fonts[fi].name   = kFontManifest[fi].name;
            g_fonts[fi].loaded = false;
            continue;
        }

        bool any = false;
        for (int si = 0; si < 5; ++si) {
            snprintf(cfg.Name, sizeof(cfg.Name), "%s@%.0f",
                     kFontManifest[fi].name, FontEntry::kBakedSizes[si]);
            ImFont* f = io.Fonts->AddFontFromFileTTF(
                            found_path, FontEntry::kBakedSizes[si], &cfg);
            g_fonts[fi].sizes[si] = f;
            if (f) any = true;
        }

        g_fonts[fi].name   = kFontManifest[fi].name;
        g_fonts[fi].loaded = any;
        if (any) { ++loaded; printf("[TextTool] Loaded: %s\n", found_path); }
        else printf("[TextTool] Failed to load: %s\n", found_path);
    }

    printf("[TextTool] %d / %d fonts loaded\n", loaded, kFontCount);
    return loaded;
}

// ─────────────────────────────────────────────────────────────────────────────
// drawAllText — called every frame regardless of active tool
// ─────────────────────────────────────────────────────────────────────────────
void drawAllText(const std::vector<TextObject>& texts,
                 ImDrawList* draw_list,
                 ImVec2 canvas_min,
                 ImVec2 canvas_size,
                 ImVec2 pan,
                 float zoom)
{
    for (const TextObject& obj : texts) {
        if (obj.content.empty()) continue;

        int fi = std::max(0, std::min(obj.style.font_index, kFontCount - 1));
        float display_size = obj.style.font_size * obj.transform.scale * zoom;
        ImFont* font = g_fonts[fi].loaded
                       ? g_fonts[fi].bestFor(display_size)
                       : ImGui::GetIO().Fonts->Fonts[0];
        if (!font) font = ImGui::GetIO().Fonts->Fonts[0];

        ImVec2 sp = w2s(obj.transform.position, canvas_min, canvas_size, pan, zoom);

        // Mirror: flip draw position
        if (obj.transform.mirrored) {
            ImVec2 sz = font->CalcTextSizeA(display_size, FLT_MAX, 0.f,
                                             obj.content.c_str());
            sp.x += sz.x;
            draw_list->PushClipRectFullScreen();
            // Flip via negative scale trick using a push/pop of draw list scale
            // Simple approach: just offset — true mirror needs transform matrix
            // For now draw normally; full mirror in phase 2 with AddImageQuad approach
            draw_list->PopClipRect();
        }

        drawTextWithStroke(draw_list, font, display_size, sp,
                           obj.content.c_str(),
                           obj.style.fill_color,
                           obj.style.stroke_color,
                           obj.style.stroke_width);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawStylePanel — floating ImGui window for text styling
// ─────────────────────────────────────────────────────────────────────────────
bool drawStylePanel(State& state, std::vector<TextObject>& texts)
{
    if (!state.panel_open) return false;

    bool changed = false;
    TextStyle& s = state.pending_style;

    ImGui::SetNextWindowPos({70.f, 64.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({280.f, 320.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGui::Begin("Text Style", &state.panel_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    // Font picker
    ImGui::Text("Font");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##font", kFontManifest[s.font_index].name)) {
        for (int i = 0; i < kFontCount; ++i) {
            bool sel = (s.font_index == i);
            // Preview label with loaded indicator
            char label[64];
            snprintf(label, sizeof(label), "%s%s",
                     kFontManifest[i].name,
                     g_fonts[i].loaded ? "" : " (missing)");
            if (ImGui::Selectable(label, sel)) {
                s.font_index = i;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Size slider
    ImGui::Text("Size");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##size", &s.font_size, 12.f, 256.f, "%.0f px"))
        changed = true;

    ImGui::Spacing();

    // Fill color
    ImGui::Text("Fill Color");
    if (ImGui::ColorEdit4("##fill", (float*)&s.fill_color,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        changed = true;

    ImGui::Spacing();

    // Stroke color + width
    ImGui::Text("Stroke");
    ImGui::SetNextItemWidth(180.f);
    if (ImGui::ColorEdit4("##stroke", (float*)&s.stroke_color,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderFloat("##sw", &s.stroke_width, 0.f, 12.f, "%.1f"))
        changed = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Hint
    ImGui::TextDisabled("Click canvas to place");
    ImGui::TextDisabled("Double-click text to edit");
    ImGui::TextDisabled("Enter / Esc to confirm");
    ImGui::TextDisabled("Delete to remove selected");

    ImGui::End();

    // If editing an existing object, push style changes live
    if (changed && state.editing_index >= 0 &&
        state.editing_index < (int)texts.size()) {
        texts[state.editing_index].style = s;
    }

    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
// update — main per-frame function
// ─────────────────────────────────────────────────────────────────────────────
void update(std::vector<TextObject>& texts,
            State& state,
            ImDrawList* draw_list,
            ImVec2 canvas_min,
            ImVec2 canvas_size,
            ImVec2 pan,
            float zoom,
            ImVec2 mouse_world,
            bool in_canvas)
{
    ImGuiIO& io = ImGui::GetIO();

    // ── Open style panel on first activation ─────────────────────────────────
    if (!state.panel_open) state.panel_open = true;

    // ── IDLE mode: select / double-click to edit / delete ────────────────────
    if (state.mode == Mode::IDLE) {

        // Left click: select
        if (in_canvas && ImGui::IsMouseClicked(0)) {

            bool hit_any = false;
            // Iterate reverse so topmost (last drawn) gets priority
            for (int i = (int)texts.size() - 1; i >= 0; --i) {
                if (hitTest(texts[i], mouse_world)) {
                    // Deselect all others
                    for (auto& t : texts) t.selected = false;
                    texts[i].selected = true;
                    hit_any = true;

                    // Double-click → enter EDITING
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        state.mode            = Mode::EDITING;
                        state.editing_index   = i;
                        state.pending_style   = texts[i].style;
                        state.frames_in_mode  = 0;
                        state.mode_enter_time = ImGui::GetTime();
                        strncpy(state.edit_buf, texts[i].content.c_str(),
                                sizeof(state.edit_buf) - 1);
                        state.edit_buf[sizeof(state.edit_buf)-1] = '\0';
                    }
                    break;
                }
            }

            // Click on empty canvas → start placing new text
            if (!hit_any) {
                for (auto& t : texts) t.selected = false;

                state.mode           = Mode::PLACING;
                state.editing_index  = -1;
                state.place_pos      = mouse_world;
                state.edit_buf[0]    = '\0';
                state.frames_in_mode = 0;
                state.mode_enter_time = ImGui::GetTime();

                ImGui::SetNextFrameWantCaptureKeyboard(true);
            }
        }

        // Delete key: remove selected text
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                texts.erase(
                    std::remove_if(texts.begin(), texts.end(),
                        [](const TextObject& t){ return t.selected; }),
                    texts.end());
            }
        }

        // Draw selection handles for selected text
        for (auto& obj : texts) {
            if (!obj.selected) continue;

            // Update cached size
            obj.cached_size = measureText(obj);

            // DragState lives on the object — we need one per TextObject.
            // For simplicity we use a static map keyed by pointer.
            // (In a real engine you'd store it in TextObject directly.)
            static std::unordered_map<TextObject*, DragState> s_drags;
            DragState& drag = s_drags[&obj];

            bool consumed = TransformHandles::update(
                draw_list, obj.transform, obj.cached_size,
                drag, zoom, pan, canvas_min, canvas_size);

            // Mirror button
            if (TransformHandles::mirrorButton(
                    draw_list, obj.transform, obj.cached_size,
                    zoom, pan, canvas_min, canvas_size)) {
                obj.transform.mirrored = !obj.transform.mirrored;
            }

            // Drag move (only if not on a handle)
            if (!consumed) {
                static bool s_moving = false;
                static ImVec2 s_move_start_mouse;
                static ImVec2 s_move_start_pos;

                if (ImGui::IsMouseClicked(0) && hitTest(obj, mouse_world)) {
                    s_moving = true;
                    s_move_start_mouse = mouse_world;
                    s_move_start_pos   = obj.transform.position;
                }
                if (s_moving && ImGui::IsMouseDown(0)) {
                    obj.transform.position.x = s_move_start_pos.x +
                        (mouse_world.x - s_move_start_mouse.x);
                    obj.transform.position.y = s_move_start_pos.y +
                        (mouse_world.y - s_move_start_mouse.y);
                }
                if (ImGui::IsMouseReleased(0)) s_moving = false;
            }
        }

        return;
    }

    // ── PLACING / EDITING: inline text entry ─────────────────────────────────
    bool is_placing = (state.mode == Mode::PLACING);
    bool is_editing = (state.mode == Mode::EDITING);

    ImVec2 text_world_pos = is_placing
        ? state.place_pos
        : (is_editing ? texts[state.editing_index].transform.position
                      : state.place_pos);

    TextStyle& style = state.pending_style;
    int fi = std::max(0, std::min(style.font_index, kFontCount - 1));
    float display_size = style.font_size * zoom;
    ImFont* font = g_fonts[fi].loaded
                   ? g_fonts[fi].bestFor(display_size)
                   : ImGui::GetIO().Fonts->Fonts[0];
    if (!font) font = ImGui::GetIO().Fonts->Fonts[0];

    ImVec2 screen_pos = w2s(text_world_pos, canvas_min, canvas_size, pan, zoom);

    // Draw live preview of what's being typed
    if (state.edit_buf[0] != '\0') {
        drawTextWithStroke(draw_list, font, display_size, screen_pos,
                           state.edit_buf,
                           style.fill_color, style.stroke_color,
                           style.stroke_width);
    }

    // Draw cursor
    drawCursor(draw_list, font, display_size, screen_pos, state.edit_buf, zoom);

    // Draw a subtle placement indicator box
    {
        ImVec2 hint_sz = font->CalcTextSizeA(display_size, FLT_MAX, 0.f,
            state.edit_buf[0] ? state.edit_buf : "Type here...");
        hint_sz.x = std::max(hint_sz.x, 120.f * zoom);
        draw_list->AddRect(
            { screen_pos.x - 2.f, screen_pos.y - 2.f },
            { screen_pos.x + hint_sz.x + 8.f, screen_pos.y + display_size + 4.f },
            IM_COL32(100, 150, 255, 80), 3.f, 0, 1.f);

        // Placeholder text
        if (state.edit_buf[0] == '\0') {
            draw_list->AddText(font, display_size, screen_pos,
                               IM_COL32(180, 180, 180, 100), "Type here...");
        }
    }

    // Track time spent in this mode (used to ignore the click that started it)
    state.frames_in_mode++;  // still increment for legacy checks

    // ── Capture keyboard input ────────────────────────────────────────────────
    // We manually handle keyboard because we're drawing on the canvas,
    // not inside an ImGui widget. This gives us full control.
    ImGui::SetNextFrameWantCaptureKeyboard(true);

    // Read characters typed this frame
    for (int n = 0; n < io.InputQueueCharacters.Size; ++n) {
        unsigned int c = io.InputQueueCharacters[n];
        if (c == 0 || c == '\t') continue;  // skip tab
        if (c == '\n' || c == '\r') continue; // handled below
        if (c < 32)                continue;  // skip control chars

        // Append to buffer
        int len = (int)strlen(state.edit_buf);
        if (len + 4 < (int)sizeof(state.edit_buf) - 1) {
            // Encode UTF-8 (simple ASCII path — extend if needed)
            if (c < 0x80) {
                state.edit_buf[len]     = (char)c;
                state.edit_buf[len + 1] = '\0';
            }
        }
    }

    // Backspace
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        int len = (int)strlen(state.edit_buf);
        if (len > 0) state.edit_buf[len - 1] = '\0';
    }

    // Confirm: Enter only
    // Click-outside is intentionally NOT a confirm — users often click the canvas
    // a second time thinking they need to focus the text box.
    // Press Enter to place, Escape to cancel.
    bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    // Cancel: Escape
    bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (confirm || cancel) {
        bool has_content = state.edit_buf[0] != '\0';

        if (confirm && has_content) {
            if (is_placing) {
                // Create new TextObject
                TextObject obj;
                obj.content          = state.edit_buf;
                obj.transform.position = state.place_pos;
                obj.transform.scale    = 1.f;
                obj.style              = style;
                obj.selected           = false;
                obj.cached_size        = measureText(obj);
                texts.push_back(obj);
            } else if (is_editing && state.editing_index >= 0) {
                // Update existing
                texts[state.editing_index].content = state.edit_buf;
                texts[state.editing_index].style   = style;
            }
        } else if (cancel && is_editing && state.editing_index >= 0) {
            // On cancel while editing, restore original (content unchanged)
            // Style was already live-updated, restore from backup if needed
            // (keeping it simple: cancel just exits without saving style changes)
        }

        // If confirmed and not cancelled, stay in IDLE but open panel for next
        state.mode           = Mode::IDLE;
        state.editing_index  = -1;
        state.edit_buf[0]    = '\0';
        state.frames_in_mode = 0;
    }
}

} // namespace TextTool