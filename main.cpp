#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
// Turns `while (!glfwWindowShouldClose(window)) { ... }` into something
// emscripten_set_main_loop() can drive on web, via EMSCRIPTEN_MAINLOOP_BEGIN/
// END wrapping the exact same loop body — no-ops on native. Official Dear
// ImGui technique (examples/libs/emscripten/emscripten_mainloop_stub.h),
// copied here rather than reached into the external/imgui submodule so it
// doesn't depend on that pin's examples/ directory sticking around.
#ifdef __EMSCRIPTEN__
#include "emscripten_mainloop_stub.h"
#endif
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "platform.h"
#include "segmenter.h"
#include "canvas_objects.h"
#include "transform_handles.h"
#include "text_tool.h"

// ── Forward declarations ──────────────────────────────────────────────────────
void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size,
                  ImVec2& pan_offset, float& zoom_level);
void RenderToolbar(ImVec2 window_size);
void RenderTopBar(ImVec2 window_size);
// Decodes an already-in-memory encoded image (PNG/JPEG/BMP) and uploads it
// as a GL texture. Takes bytes rather than a path — Platform::OpenImageFile
// hands back bytes on both native and web, since there's no meaningful
// filesystem path for a file the user picked in a browser.
GLuint LoadTextureFromMemory(const unsigned char* data, size_t size,
                             int* out_width, int* out_height,
                             std::vector<unsigned char>& out_pixels);
GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int w, int h);
void   UpdateTextureFromPixels(GLuint tex_id, const std::vector<unsigned char>& pixels, int w, int h);
void   ApplyMaskToPixels(std::vector<unsigned char>& pixels,
                         const std::vector<uint8_t>& mask, int width, int height);
void   ExportImageAsPNG(int image_index);

// ── App state ─────────────────────────────────────────────────────────────────
struct AppState {
    ImVec2 canvas_pan  = {0, 0};
    float  canvas_zoom = 1.0f;
    bool   is_panning  = false;
    ImVec2 last_mouse_pos = {0, 0};

    enum Tool { TOOL_SELECT, TOOL_HAND, TOOL_SEGMENT, TOOL_TEXT };
    Tool current_tool = TOOL_SELECT;

    // Images
    std::vector<CanvasImage> images;
    int    selected_image_index = -1;
    bool   is_dragging_image    = false;
    ImVec2 drag_start_pos       = {0, 0};
    // Per-image drag states for transform handles (keyed by index)
    std::unordered_map<int, DragState> image_drags;

    // Text
    std::vector<TextObject> texts;
    TextTool::State         text_state;
    bool   is_dragging_text = false;
    // Per-text drag states for transform handles (keyed by index, like image_drags)
    std::unordered_map<int, DragState> text_drags;

    // Segment
    int  seg_image_index = -1;
    bool image_encoded   = false;

    std::atomic<bool> is_encoding{false};
    std::atomic<bool> encode_done{false};
    std::atomic<bool> encode_ok{false};
    std::thread       encode_thread;

    std::vector<PromptPoint> prompt_points;
    GLuint overlay_texture = 0;
    int    overlay_w = 0, overlay_h = 0;

    std::mutex                 overlay_mutex;
    bool                       pending_overlay = false;
    std::vector<unsigned char> pending_rgba;
    int                        pending_ow = 0, pending_oh = 0;

    // Status toast
    std::string status_msg;
    float       status_timer = 0.f;

} g_state;

Segmenter g_segmenter;

// ─────────────────────────────────────────────────────────────────────────────
// Overlay helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<unsigned char> BuildOverlayRGBA(const SegmentResult& r)
{
    std::vector<unsigned char> rgba(r.width * r.height * 4, 0);
    for (int i = 0; i < r.width * r.height; i++) {
        if (r.mask[i] > 0) {
            rgba[i*4+0]=80; rgba[i*4+1]=180; rgba[i*4+2]=255; rgba[i*4+3]=120;
        }
    }
    return rgba;
}

static void FlushPendingOverlay()
{
    std::lock_guard<std::mutex> lock(g_state.overlay_mutex);
    if (!g_state.pending_overlay) return;
    int w = g_state.pending_ow, h = g_state.pending_oh;
    if (g_state.overlay_texture && g_state.overlay_w==w && g_state.overlay_h==h)
        UpdateTextureFromPixels(g_state.overlay_texture, g_state.pending_rgba, w, h);
    else {
        if (g_state.overlay_texture) glDeleteTextures(1, &g_state.overlay_texture);
        g_state.overlay_texture = CreateTextureFromPixels(g_state.pending_rgba, w, h);
        g_state.overlay_w = w; g_state.overlay_h = h;
    }
    g_state.pending_overlay = false;
}

static void QueueOverlay(const SegmentResult& result)
{
    if (!result.valid) return;
    std::lock_guard<std::mutex> lock(g_state.overlay_mutex);
    g_state.pending_rgba    = BuildOverlayRGBA(result);
    g_state.pending_ow      = result.width;
    g_state.pending_oh      = result.height;
    g_state.pending_overlay = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// CommitExtraction
// ─────────────────────────────────────────────────────────────────────────────
static void CommitExtraction()
{
    if (g_state.seg_image_index < 0 || g_state.prompt_points.empty()) return;
    SegmentResult result = g_segmenter.decode(g_state.prompt_points);
    if (!result.valid) { printf("[Seg] Invalid result.\n"); return; }

    CanvasImage& src = g_state.images[g_state.seg_image_index];
    CanvasImage ex;
    ex.pixels   = src.pixels;
    ex.size      = src.size;
    ex.transform.scale = src.transform.scale;
    ex.filename  = src.filename + "_extracted";

    ApplyMaskToPixels(ex.pixels, result.mask, (int)src.size.x, (int)src.size.y);
    ex.texture_id = CreateTextureFromPixels(ex.pixels, (int)src.size.x, (int)src.size.y);
    ex.transform.position = {
        src.transform.position.x + src.size.x * src.transform.scale + 20,
        src.transform.position.y
    };

    g_state.images.push_back(ex);
    g_state.status_msg = "Extraction committed!";
    g_state.status_timer = 3.f;

    g_state.prompt_points.clear();
    g_state.seg_image_index = -1;
    g_state.image_encoded   = false;
    if (g_state.overlay_texture) {
        glDeleteTextures(1, &g_state.overlay_texture);
        g_state.overlay_texture = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ExportImageAsPNG
// ─────────────────────────────────────────────────────────────────────────────
void ExportImageAsPNG(int idx)
{
    if (idx < 0 || idx >= (int)g_state.images.size()) return;
    CanvasImage& img = g_state.images[idx];
    if (img.pixels.empty()) return;
    int w=(int)img.size.x, h=(int)img.size.y;

    // Encode to an in-memory buffer first (fully portable — stb_image_write
    // doesn't care whether the bytes end up on disk or in a browser
    // download), then hand off to the platform to actually deliver it.
    std::vector<unsigned char> png_bytes;
    auto write_cb = [](void* ctx, void* data, int size) {
        auto* out   = static_cast<std::vector<unsigned char>*>(ctx);
        auto* bytes = static_cast<unsigned char*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(write_cb, &png_bytes, w, h, 4, img.pixels.data(), w*4)) {
        g_state.status_msg   = "Export failed!";
        g_state.status_timer = 3.f;
        return;
    }

    if (Platform::SaveFile("output.png", png_bytes.data(), png_bytes.size())) {
        g_state.status_msg   = "Exported!";
        g_state.status_timer = 4.f;
    } else {
        g_state.status_msg   = "Export cancelled";
        g_state.status_timer = 2.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (!glfwInit()) { fprintf(stderr, "Failed to init GLFW\n"); return -1; }

    // GL context + matching GLSL version string for ImGui_ImplOpenGL3_Init()
    // below — these have to agree with each other and with the platform.
    // imgui_impl_opengl3.h auto-detects IMGUI_IMPL_OPENGL_ES2 under
    // __EMSCRIPTEN__ (GLES2/WebGL1) internally, but that only changes how
    // the *backend* renders — the glsl_version string we hand it here still
    // has to match, or shader compilation fails outright (desktop "#version
    // 330" uses in/out and other GLSL 3.30 syntax GLES2's "#version 100"
    // doesn't support at all).
    const char* glsl_version;
#ifdef __EMSCRIPTEN__
    glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#else
    glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
#endif

    GLFWwindow* window = glfwCreateWindow(1440, 900, "Yoinkboard", NULL, NULL);
    if (!window) { fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.ChildRounding  = 0.f;
    style.FrameRounding  = style.GrabRounding   = 4.f;
    style.PopupRounding  = style.ScrollbarRounding = 4.f;
    style.WindowBorderSize = style.FrameBorderSize = 0.f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = {0.10f,0.10f,0.10f,1.f};
    c[ImGuiCol_ChildBg]          = {0.12f,0.12f,0.12f,1.f};
    c[ImGuiCol_PopupBg]          = {0.15f,0.15f,0.15f,1.f};
    c[ImGuiCol_Border]           = {0.20f,0.20f,0.20f,1.f};
    c[ImGuiCol_FrameBg]          = {0.18f,0.18f,0.18f,1.f};
    c[ImGuiCol_FrameBgHovered]   = {0.25f,0.25f,0.25f,1.f};
    c[ImGuiCol_FrameBgActive]    = {0.30f,0.30f,0.30f,1.f};
    c[ImGuiCol_TitleBg]          = {0.08f,0.08f,0.08f,1.f};
    c[ImGuiCol_TitleBgActive]    = {0.08f,0.08f,0.08f,1.f};
    c[ImGuiCol_Button]           = {0.20f,0.20f,0.20f,1.f};
    c[ImGuiCol_ButtonHovered]    = {0.28f,0.28f,0.28f,1.f};
    c[ImGuiCol_ButtonActive]     = {0.35f,0.35f,0.35f,1.f};
    c[ImGuiCol_CheckMark]        = {0.40f,0.60f,1.00f,1.f};
    c[ImGuiCol_SliderGrab]       = {0.40f,0.60f,1.00f,1.f};
    c[ImGuiCol_SliderGrabActive] = {0.50f,0.70f,1.00f,1.f};

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load fonts BEFORE first NewFrame so atlas includes them
    TextTool::loadFonts();

    if (!g_segmenter.loadModels("models/mobile_sam_encoder.onnx",
                                "models/mobile_sam_decoder.onnx"))
        fprintf(stderr, "Warning: MobileSAM models not found.\n");

    // ── Main loop ─────────────────────────────────────────────────────────────
#ifdef __EMSCRIPTEN__
    io.IniFilename = nullptr; // no persistent filesystem to fopen() an imgui.ini on
    EMSCRIPTEN_MAINLOOP_BEGIN
#else
    while (!glfwWindowShouldClose(window))
#endif
    {
        glfwPollEvents();

        // Async encode completion
        if (g_state.encode_done.exchange(false)) {
            if (g_state.encode_thread.joinable()) g_state.encode_thread.join();
            g_state.image_encoded = g_state.encode_ok.load();
            g_state.is_encoding   = false;
            if (g_state.image_encoded) {
                if (!g_state.prompt_points.empty())
                    QueueOverlay(g_segmenter.decode(g_state.prompt_points));
                g_state.status_msg   = "Encoded. Click to segment.";
                g_state.status_timer = 3.f;
            } else {
                g_state.status_msg   = "Encode failed!";
                g_state.status_timer = 3.f;
            }
        }

        FlushPendingOverlay();
        if (g_state.status_timer > 0.f) g_state.status_timer -= io.DeltaTime;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImVec2 win_size = io.DisplaySize;

        // Text style panel (floating, above everything)
        if (g_state.current_tool == AppState::TOOL_TEXT)
            TextTool::drawStylePanel(g_state.text_state, g_state.texts);

        // Main fullscreen window
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize(win_size);
        ImGui::Begin("MainWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground);

        RenderTopBar(win_size);
        RenderToolbar(win_size);

        ImVec2 canvas_pos  = {60.f, 56.f};
        ImVec2 canvas_size = {win_size.x - 60.f, win_size.y - 56.f};

        ImGui::SetCursorPos(canvas_pos);
        ImGui::BeginChild("Canvas", canvas_size, false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        RenderCanvas(draw_list, canvas_pos, canvas_size,
                     g_state.canvas_pan, g_state.canvas_zoom);

        ImGui::EndChild();

        // Status toast
        if (g_state.status_timer > 0.f && !g_state.status_msg.empty()) {
            ImGui::SetNextWindowPos({canvas_pos.x + 10.f, win_size.y - 36.f});
            ImGui::SetNextWindowBgAlpha(0.78f);
            ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav);
            ImGui::TextColored({0.9f,0.9f,0.9f,1.f}, "%s", g_state.status_msg.c_str());
            ImGui::End();
        }

        ImGui::End();

        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f,0.10f,0.10f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_END;
#endif

    // On web, emscripten_set_main_loop() above never actually returns —
    // it hands control back to the browser's event loop and keeps calling
    // back into the loop body via requestAnimationFrame, so nothing below
    // this point runs there in practice. Left in place (rather than #ifdef'd
    // out) because it still needs to compile, and matches the official
    // Dear ImGui Emscripten example's own structure.
    if (g_state.encode_thread.joinable()) g_state.encode_thread.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderTopBar
// ─────────────────────────────────────────────────────────────────────────────
void RenderTopBar(ImVec2 win_size)
{
    const float bar_h = 56.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0,0}, {win_size.x, bar_h}, IM_COL32(20,20,20,255));
    dl->AddLine({0, bar_h}, {win_size.x, bar_h}, IM_COL32(40,40,40,255), 1.f);

    ImGui::SetCursorPos({20, 16});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
    ImGui::Text("Yoinkboard");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos({120, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  {4, 0});

    // Load image
    if (ImGui::Button("Image")) {
        // win_size is a stack local — captured by value, not reference,
        // since on web this callback fires later (async file picker), well
        // after RenderTopBar has returned and any reference would dangle.
        Platform::OpenImageFile([win_size](const unsigned char* data, size_t size) {
            CanvasImage img;
            img.selected = false;
            img.filename = "image"; // no real path on web; not used for I/O anywhere
            int w, h;
            img.texture_id = LoadTextureFromMemory(data, size, &w, &h, img.pixels);
            if (img.texture_id) {
                img.size = {(float)w, (float)h};

                // Fit-to-canvas: shrink to fit inside the visible viewport,
                // never enlarge a small image past its native size.
                const float pad       = 40.f;
                ImVec2      avail     = {win_size.x - 60.f, win_size.y - 56.f};
                float       fit_scale = std::min(
                    (avail.x - pad) / (float)w,
                    (avail.y - pad) / (float)h);
                img.transform.scale = std::min(1.f, fit_scale);

                img.transform.position = {
                    -(float)w * img.transform.scale * 0.5f,
                    -(float)h * img.transform.scale * 0.5f
                };
                g_state.images.push_back(img);

                // Reset the view so the newly loaded image is fully visible.
                g_state.canvas_pan  = {0.f, 0.f};
                g_state.canvas_zoom = 1.f;
            }
        });
    }
    ImGui::SameLine();

    // Export PNG
    bool has_sel = (g_state.selected_image_index >= 0 &&
                    g_state.selected_image_index < (int)g_state.images.size());
    if (!has_sel) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.15f,0.15f,0.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.15f,0.15f,0.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {0.4f,0.4f,0.4f,1.f});
    }
    if (ImGui::Button("Export PNG") && has_sel)
        ExportImageAsPNG(g_state.selected_image_index);
    if (!has_sel) {
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select an image first");
    }

    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderToolbar
// ─────────────────────────────────────────────────────────────────────────────
void RenderToolbar(ImVec2 win_size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({0,56}, {60, win_size.y}, IM_COL32(20,20,20,255));

    ImGui::SetCursorPos({10, 66});
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  {0, 4});

    // Switching tools away from Text mid-entry used to just silently drop
    // whatever was being typed (internal mode stayed PLACING/EDITING even
    // though current_tool moved on, so re-entering Text felt like it "went
    // back" to a stuck state). Auto-commit instead, same as pressing Enter.
    auto switchTool = [&](AppState::Tool tool) {
        if (g_state.current_tool == AppState::TOOL_TEXT &&
            g_state.text_state.mode != TextTool::Mode::IDLE) {
            TextTool::finish(g_state.texts, g_state.text_state, /*confirm=*/true);
        }
        g_state.current_tool = tool;
    };

    auto toolBtn = [&](const char* label, AppState::Tool tool,
                        ImVec4 active_col, const char* tooltip) {
        bool active = (g_state.current_tool == tool);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, active_col);
        if (ImGui::Button(label, {40,40})) switchTool(tool);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    };

    toolBtn("V", AppState::TOOL_SELECT,  {0.4f,0.6f,1.0f,0.3f}, "Select (V)");
    toolBtn("H", AppState::TOOL_HAND,    {0.4f,0.6f,1.0f,0.3f}, "Hand (H)");
    toolBtn("S", AppState::TOOL_SEGMENT, {0.4f,1.0f,0.6f,0.3f},
            "Segment (S)\nLeft=FG  Right=BG\nEnter=extract  Esc=cancel");
    toolBtn("T", AppState::TOOL_TEXT,    {1.0f,0.8f,0.3f,0.3f},
            "Text (T)\nClick to place\nDouble-click to edit\nDelete to remove");

    ImGui::PopStyleVar(2);
    ImGui::EndGroup();

    // Segment status
    if (g_state.is_encoding) {
        ImGui::SetCursorPos({4, 250});
        ImGui::TextColored({1.f,0.8f,0.2f,1.f}, "enc..");
    } else if (g_state.image_encoded && !g_state.prompt_points.empty()) {
        ImGui::SetCursorPos({4, 250});
        ImGui::TextColored({0.4f,1.f,0.6f,1.f}, "%dpt",
                           (int)g_state.prompt_points.size());
    }

    // Keyboard shortcuts (only when not typing text). Text entry here is a
    // hand-rolled canvas overlay, not a real ImGui InputText widget, so
    // io.WantTextInput alone doesn't cover it — without the extra mode check
    // below, typing a letter like "v" or "t" into a text box would double as
    // a tool-switch shortcut and silently boot you out of the text tool.
    ImGuiIO& io = ImGui::GetIO();
    bool typing_text = (g_state.current_tool == AppState::TOOL_TEXT &&
                        g_state.text_state.mode != TextTool::Mode::IDLE);
    if (!io.WantTextInput && !typing_text) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)) switchTool(AppState::TOOL_SELECT);
        if (ImGui::IsKeyPressed(ImGuiKey_H)) switchTool(AppState::TOOL_HAND);
        if (ImGui::IsKeyPressed(ImGuiKey_S)) switchTool(AppState::TOOL_SEGMENT);
        if (ImGui::IsKeyPressed(ImGuiKey_T)) {
            switchTool(AppState::TOOL_TEXT);
            g_state.text_state.panel_open = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderCanvas
// ─────────────────────────────────────────────────────────────────────────────
void RenderCanvas(ImDrawList* dl, ImVec2 canvas_pos, ImVec2 canvas_size,
                  ImVec2& pan, float& zoom)
{
    ImVec2 cmin = canvas_pos;
    ImVec2 cmax = {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y};

    dl->AddRectFilled(cmin, cmax, IM_COL32(25,25,25,255));

    // Grid
    float gs  = 50.f * zoom;
    float sx0 = cmin.x + fmodf(pan.x * zoom, gs);
    float sy0 = cmin.y + fmodf(pan.y * zoom, gs);
    ImU32 gc  = IM_COL32(40,40,40,255);
    for (float x = sx0; x < cmax.x; x += gs) dl->AddLine({x,cmin.y},{x,cmax.y},gc);
    for (float y = sy0; y < cmax.y; y += gs) dl->AddLine({cmin.x,y},{cmax.x,y},gc);

    // Helper: world → screen
    auto w2s = [&](ImVec2 wp) -> ImVec2 {
        return {
            cmin.x + canvas_size.x*0.5f + (wp.x + pan.x) * zoom,
            cmin.y + canvas_size.y*0.5f + (wp.y + pan.y) * zoom
        };
    };

    // ── Draw images ───────────────────────────────────────────────────────────
    for (int i = 0; i < (int)g_state.images.size(); i++) {
        CanvasImage& img = g_state.images[i];
        ImVec2 sp    = w2s(img.transform.position);
        ImVec2 sp_br = {
            sp.x + img.size.x * img.transform.scale * zoom,
            sp.y + img.size.y * img.transform.scale * zoom
        };

        // UV flip for mirror
        ImVec2 uv0 = img.transform.mirrored ? ImVec2(1,0) : ImVec2(0,0);
        ImVec2 uv1 = img.transform.mirrored ? ImVec2(0,1) : ImVec2(1,1);
        dl->AddImage((ImTextureID)(intptr_t)img.texture_id, sp, sp_br, uv0, uv1);

        // Segment overlay
        if (i == g_state.seg_image_index && g_state.overlay_texture)
            dl->AddImage((ImTextureID)(intptr_t)g_state.overlay_texture, sp, sp_br);

        // Prompt points
        if (i == g_state.seg_image_index) {
            for (const auto& pt : g_state.prompt_points) {
                ImVec2 pp = {
                    sp.x + pt.x * img.transform.scale * zoom,
                    sp.y + pt.y * img.transform.scale * zoom
                };
                ImU32 col = pt.label ? IM_COL32(50,220,80,255) : IM_COL32(220,60,60,255);
                dl->AddCircleFilled(pp, 6.f*zoom, col);
                dl->AddCircle(pp, 6.f*zoom, IM_COL32(255,255,255,200), 12, 1.5f);
            }
        }

        // Transform handles for selected image
        if (img.selected) {
            DragState& drag = g_state.image_drags[i];
            TransformHandles::update(dl, img.transform, img.size,
                                     drag, zoom, pan, cmin, canvas_size);
            if (TransformHandles::mirrorButton(dl, img.transform, img.size,
                                               zoom, pan, cmin, canvas_size))
                img.transform.mirrored = !img.transform.mirrored;
        }
    }

    // Encoding spinner
    if (g_state.is_encoding) {
        ImVec2 ctr = {cmin.x + canvas_size.x*0.5f, cmin.y + canvas_size.y*0.5f};
        float t = (float)glfwGetTime(), r = 24.f;
        dl->AddCircle(ctr, r, IM_COL32(60,60,60,200), 32, 3.f);
        for (int j = 0; j < 8; j++) {
            float a = t*3.f + j*(3.14159f*2.f/8.f);
            dl->AddCircleFilled({ctr.x+cosf(a)*r, ctr.y+sinf(a)*r},
                3.5f, IM_COL32(80,180,255,(int)(j/8.f*180)+40));
        }
        dl->AddText({ctr.x-32.f, ctr.y+r+8.f}, IM_COL32(180,180,180,255), "Encoding...");
    }

    // Always draw all text (visible in every tool mode)
    TextTool::drawAllText(g_state.texts, dl, cmin, canvas_size, pan, zoom);

    // Selected text's resize/rotate/mirror handles — always live, same as
    // selected image handles above, regardless of which tool is active.
    bool text_handle_consumed = TextTool::updateSelectionHandles(
        g_state.texts, g_state.text_drags, dl, cmin, canvas_size, pan, zoom);

    // Crosshair at world origin
    ImVec2 orig = w2s({0,0});
    dl->AddLine({orig.x-10,orig.y},{orig.x+10,orig.y}, IM_COL32(100,150,255,255), 2.f);
    dl->AddLine({orig.x,orig.y-10},{orig.x,orig.y+10}, IM_COL32(100,150,255,255), 2.f);

    // ── Input ─────────────────────────────────────────────────────────────────
    ImGuiIO& io = ImGui::GetIO();
    // Exclude the floating Text Style panel — it visually sits on top of the
    // canvas, but our hit-testing here is raw screen-rect math with no idea
    // any other window exists, so scrolling/clicking on the panel was also
    // reaching the canvas underneath (e.g. mouse wheel there zoomed the grid
    // instead of just scrolling the panel).
    bool in_canvas = ImGui::IsMouseHoveringRect(cmin, cmax) &&
                     !g_state.text_state.panel_hovered;

    if (in_canvas) {
        // Zoom toward cursor
        if (io.MouseWheel != 0.f) {
            float old_zoom = zoom;
            zoom = std::max(0.1f, std::min(5.f, zoom + io.MouseWheel * 0.1f));
            float mx = io.MousePos.x - (cmin.x + canvas_size.x * 0.5f);
            float my = io.MousePos.y - (cmin.y + canvas_size.y * 0.5f);
            pan.x += mx/zoom - mx/old_zoom;
            pan.y += my/zoom - my/old_zoom;
        }

        // Mouse → world space
        ImVec2 mw = {
            (io.MousePos.x - cmin.x - canvas_size.x*0.5f) / zoom - pan.x,
            (io.MousePos.y - cmin.y - canvas_size.y*0.5f) / zoom - pan.y
        };

        // ── Text tool ─────────────────────────────────────────────────────────
        if (g_state.current_tool == AppState::TOOL_TEXT) {
            TextTool::update(g_state.texts, g_state.text_state,
                             dl, cmin, canvas_size, pan, zoom, mw, in_canvas);

        // ── Select tool ───────────────────────────────────────────────────────
        // Handles both images AND text (text takes priority when they overlap,
        // since text is drawn on top). Selecting one type deselects the other.
        } else if (g_state.current_tool == AppState::TOOL_SELECT) {

            // Find whichever text is currently selected (at most one, by
            // convention) — computed on demand instead of cached, since a
            // text can also be selected from the Text tool's own click
            // handling, and this stays correct either way.
            auto findSelectedText = [&]() -> int {
                for (int i = 0; i < (int)g_state.texts.size(); i++)
                    if (g_state.texts[i].selected) return i;
                return -1;
            };

            if (ImGui::IsMouseClicked(0)) {
                // Was a handle (image or text) just grabbed this click?
                // (TransformHandles::update sets drag.active inside the draw loop above)
                bool handle_just_activated = text_handle_consumed;
                if (!handle_just_activated && g_state.selected_image_index >= 0) {
                    auto it = g_state.image_drags.find(g_state.selected_image_index);
                    if (it != g_state.image_drags.end())
                        handle_just_activated = (it->second.active != HandleId::None);
                }

                if (!handle_just_activated) {
                    // Text hit-test first — text is drawn on top of images
                    int text_hit = -1;
                    for (int i = (int)g_state.texts.size()-1; i >= 0; i--) {
                        if (TextTool::hitTest(g_state.texts[i], mw)) { text_hit = i; break; }
                    }

                    if (text_hit >= 0) {
                        for (auto& img : g_state.images) img.selected = false;
                        g_state.selected_image_index = -1;
                        g_state.is_dragging_image    = false;

                        for (auto& t : g_state.texts) t.selected = false;
                        g_state.texts[text_hit].selected = true;
                        g_state.is_dragging_text = true;
                        g_state.drag_start_pos   = mw;
                    } else {
                        // Find which image was clicked
                        int new_sel = -1;
                        for (int i = (int)g_state.images.size()-1; i >= 0; i--) {
                            CanvasImage& img = g_state.images[i];
                            float x0 = img.transform.position.x;
                            float y0 = img.transform.position.y;
                            float x1 = x0 + img.size.x * img.transform.scale;
                            float y1 = y0 + img.size.y * img.transform.scale;
                            if (mw.x>=x0 && mw.x<=x1 && mw.y>=y0 && mw.y<=y1) {
                                new_sel = i; break;
                            }
                        }

                        // Deselect all
                        for (auto& t : g_state.texts) t.selected = false;
                        g_state.is_dragging_text = false;
                        for (auto& img : g_state.images) img.selected = false;
                        g_state.selected_image_index = new_sel;

                        if (new_sel >= 0) {
                            g_state.images[new_sel].selected = true;
                            // Only start image drag if NOT clicking a handle area
                            // (handle areas are ~10px from corners in screen space,
                            //  but we can't easily check that here — so we defer:
                            //  drag starts next frame if drag.active is still None)
                            g_state.is_dragging_image = true;
                            g_state.drag_start_pos    = mw;
                        } else {
                            g_state.is_dragging_image = false;
                        }
                    }
                }
            }

            // Drag move — text (only when no handle is active)
            if (g_state.is_dragging_text) {
                int sel = findSelectedText();
                if (sel >= 0) {
                    auto drag_it = g_state.text_drags.find(sel);
                    bool handle_active = (drag_it != g_state.text_drags.end() &&
                                          drag_it->second.active != HandleId::None);
                    if (ImGui::IsMouseDown(0)) {
                        if (!handle_active) {
                            TextObject& t = g_state.texts[sel];
                            t.transform.position.x += mw.x - g_state.drag_start_pos.x;
                            t.transform.position.y += mw.y - g_state.drag_start_pos.y;
                            g_state.drag_start_pos = mw;
                        } else {
                            g_state.drag_start_pos = mw;
                        }
                    } else {
                        g_state.is_dragging_text = false;
                    }
                } else {
                    g_state.is_dragging_text = false;
                }
            }

            // Drag move — image (only when no handle is active)
            if (g_state.is_dragging_image && g_state.selected_image_index >= 0) {
                // Use find() not operator[] — operator[] inserts a default DragState
                // with active=None, making handle detection always fail
                auto drag_it = g_state.image_drags.find(g_state.selected_image_index);
                bool handle_active = (drag_it != g_state.image_drags.end() &&
                                      drag_it->second.active != HandleId::None);
                if (ImGui::IsMouseDown(0)) {
                    if (!handle_active) {
                        // Normal image move
                        CanvasImage& img = g_state.images[g_state.selected_image_index];
                        img.transform.position.x += mw.x - g_state.drag_start_pos.x;
                        img.transform.position.y += mw.y - g_state.drag_start_pos.y;
                        g_state.drag_start_pos = mw;
                    } else {
                        // Handle active — keep drag_start_pos in sync so no jump on release
                        g_state.drag_start_pos = mw;
                    }
                } else {
                    g_state.is_dragging_image = false;
                }
            }

            // Delete key — text
            if (!io.WantTextInput &&
                (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                 ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
                int sel = findSelectedText();
                if (sel >= 0) {
                    g_state.texts.erase(g_state.texts.begin() + sel);
                    g_state.text_drags.erase(sel);
                    g_state.is_dragging_text = false;
                }
            }

            // Delete key — image
            if (g_state.selected_image_index >= 0 && !io.WantTextInput &&
                (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                 ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
                glDeleteTextures(1,
                    &g_state.images[g_state.selected_image_index].texture_id);
                g_state.images.erase(g_state.images.begin() + g_state.selected_image_index);
                g_state.image_drags.erase(g_state.selected_image_index);
                g_state.selected_image_index = -1;
            }

        // ── Hand tool ─────────────────────────────────────────────────────────
        } else if (g_state.current_tool == AppState::TOOL_HAND) {
            if (ImGui::IsMouseDown(0) || ImGui::IsMouseDown(2)) {
                if (!g_state.is_panning) {
                    g_state.is_panning     = true;
                    g_state.last_mouse_pos = io.MousePos;
                }
                pan.x += (io.MousePos.x - g_state.last_mouse_pos.x) / zoom;
                pan.y += (io.MousePos.y - g_state.last_mouse_pos.y) / zoom;
                g_state.last_mouse_pos = io.MousePos;
            } else {
                g_state.is_panning = false;
            }

        // ── Segment tool ──────────────────────────────────────────────────────
        } else if (g_state.current_tool == AppState::TOOL_SEGMENT) {
            bool fg = ImGui::IsMouseClicked(0);
            bool bg = ImGui::IsMouseClicked(1);

            if ((fg || bg) && !g_state.is_encoding) {
                for (int i = (int)g_state.images.size()-1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];
                    float x0 = img.transform.position.x, y0 = img.transform.position.y;
                    float x1 = x0+img.size.x*img.transform.scale;
                    float y1 = y0+img.size.y*img.transform.scale;
                    if (mw.x<x0||mw.x>x1||mw.y<y0||mw.y>y1) continue;
                    if (img.pixels.empty()) break;
                    if (!g_segmenter.isReady()) {
                        g_state.status_msg="Models not loaded!"; g_state.status_timer=3.f; break;
                    }

                    if (i != g_state.seg_image_index) {
                        if (g_state.encode_thread.joinable()) g_state.encode_thread.join();
                        g_state.prompt_points.clear();
                        g_state.seg_image_index = i;
                        g_state.image_encoded = false;
                        g_state.encode_ok = false;
                        if (g_state.overlay_texture) {
                            glDeleteTextures(1, &g_state.overlay_texture);
                            g_state.overlay_texture = 0;
                        }
                        g_state.is_encoding = true;
                        g_state.encode_done = false;

                        const uint8_t* px = img.pixels.data();
                        int iw=(int)img.size.x, ih=(int)img.size.y;
                        g_state.encode_thread = std::thread([px,iw,ih](){
                            g_state.encode_ok   = g_segmenter.encodeImage(px,iw,ih);
                            g_state.encode_done = true;
                        });
                        g_state.status_msg="Encoding image..."; g_state.status_timer=60.f;
                    }

                    float lx = (mw.x-x0)/img.transform.scale;
                    float ly = (mw.y-y0)/img.transform.scale;
                    g_state.prompt_points.push_back({lx, ly, fg?1:0});

                    if (g_state.image_encoded)
                        QueueOverlay(g_segmenter.decode(g_state.prompt_points));
                    break;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && g_state.image_encoded)
                CommitExtraction();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_state.prompt_points.clear();
                g_state.seg_image_index = -1;
                g_state.image_encoded   = false;
                if (g_state.overlay_texture) {
                    glDeleteTextures(1, &g_state.overlay_texture);
                    g_state.overlay_texture = 0;
                }
            }
        }

        // Space+drag = pan in any tool
        if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(0)) {
            if (!g_state.is_panning) {
                g_state.is_panning     = true;
                g_state.last_mouse_pos = io.MousePos;
            }
            pan.x += (io.MousePos.x - g_state.last_mouse_pos.x) / zoom;
            pan.y += (io.MousePos.y - g_state.last_mouse_pos.y) / zoom;
            g_state.last_mouse_pos = io.MousePos;
        } else if (!ImGui::IsKeyDown(ImGuiKey_Space) &&
                   g_state.current_tool != AppState::TOOL_HAND) {
            g_state.is_panning = false;
        }

    } else {
        g_state.is_panning = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Texture helpers
// ─────────────────────────────────────────────────────────────────────────────
GLuint LoadTextureFromMemory(const unsigned char* data, size_t size,
                             int* out_w, int* out_h,
                             std::vector<unsigned char>& out_pixels)
{
    int w, h, ch;
    unsigned char* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &ch, 4);
    if (!pixels) { fprintf(stderr, "Failed to decode image: %s\n", stbi_failure_reason()); return 0; }
    out_pixels.assign(pixels, pixels + w*h*4);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    *out_w = w; *out_h = h;
    return tex;
}

GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int w, int h)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    return tex;
}

void UpdateTextureFromPixels(GLuint tex_id, const std::vector<unsigned char>& pixels, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
}

void ApplyMaskToPixels(std::vector<unsigned char>& pixels,
                       const std::vector<uint8_t>& mask, int w, int h)
{
    for (int i = 0; i < w*h; i++)
        if (mask[i] == 0) pixels[i*4+3] = 0;
}