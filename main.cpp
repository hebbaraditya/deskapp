#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>

#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "tinyfiledialogs.h"
#include "segmenter.h"

// ── Forward declarations ──────────────────────────────────────────────────────
void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size,
                  ImVec2& pan_offset, float& zoom_level);
void RenderToolbar(ImVec2 window_size);
void RenderTopBar(ImVec2 window_size);
GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height,
                           std::vector<unsigned char>& out_pixels);
GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int width, int height);
void   UpdateTextureFromPixels(GLuint tex_id, const std::vector<unsigned char>& pixels, int w, int h);
void   ApplyMaskToPixels(std::vector<unsigned char>& pixels,
                         const std::vector<uint8_t>& mask, int width, int height);
void   ExportImageAsPNG(int image_index);

// ── Canvas image ──────────────────────────────────────────────────────────────
struct CanvasImage {
    GLuint      texture_id = 0;
    ImVec2      position   = {0, 0};   // world space
    ImVec2      size       = {0, 0};
    float       scale      = 1.0f;
    bool        selected   = false;
    std::string filename;
    std::vector<unsigned char> pixels; // original RGBA, never modified
};

// ── App state ─────────────────────────────────────────────────────────────────
struct AppState {
    ImVec2 canvas_pan  = {0, 0};
    float  canvas_zoom = 1.0f;
    bool   is_panning  = false;
    ImVec2 last_mouse_pos = {0, 0};

    enum Tool { TOOL_SELECT, TOOL_HAND, TOOL_SEGMENT };
    Tool current_tool = TOOL_SELECT;

    std::vector<CanvasImage> images;
    int  selected_image_index = -1;
    bool is_dragging_image    = false;
    ImVec2 drag_start_pos     = {0, 0};

    // ── Segment tool state ────────────────────────────────────────────────────
    int  seg_image_index   = -1;
    bool image_encoded     = false;

    // Async encoding
    std::atomic<bool> is_encoding{false};
    std::atomic<bool> encode_done{false};
    std::atomic<bool> encode_ok{false};
    std::thread       encode_thread;

    std::vector<PromptPoint> prompt_points;
    GLuint overlay_texture = 0;
    int    overlay_w = 0, overlay_h = 0;

    // Pending overlay update (written by encode thread, read by main thread)
    std::mutex               overlay_mutex;
    bool                     pending_overlay    = false;
    std::vector<unsigned char> pending_rgba;
    int                      pending_ow = 0, pending_oh = 0;

    // Status message
    std::string status_msg;
    float       status_timer = 0.f;

} g_state;

// ── Global segmenter ──────────────────────────────────────────────────────────
Segmenter g_segmenter;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build RGBA overlay from a SegmentResult
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<unsigned char> BuildOverlayRGBA(const SegmentResult& result)
{
    int w = result.width, h = result.height;
    std::vector<unsigned char> rgba(w * h * 4, 0);
    for (int i = 0; i < w * h; i++) {
        if (result.mask[i] > 0) {
            rgba[i * 4 + 0] = 80;
            rgba[i * 4 + 1] = 180;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = 120;
        }
    }
    return rgba;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: apply pending overlay to GPU texture (called from main thread)
// ─────────────────────────────────────────────────────────────────────────────
static void FlushPendingOverlay()
{
    std::lock_guard<std::mutex> lock(g_state.overlay_mutex);
    if (!g_state.pending_overlay) return;

    int w = g_state.pending_ow, h = g_state.pending_oh;

    if (g_state.overlay_texture && g_state.overlay_w == w && g_state.overlay_h == h) {
        UpdateTextureFromPixels(g_state.overlay_texture, g_state.pending_rgba, w, h);
    } else {
        if (g_state.overlay_texture)
            glDeleteTextures(1, &g_state.overlay_texture);
        g_state.overlay_texture = CreateTextureFromPixels(g_state.pending_rgba, w, h);
        g_state.overlay_w = w;
        g_state.overlay_h = h;
    }
    g_state.pending_overlay = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: set a pending overlay from any thread
// ─────────────────────────────────────────────────────────────────────────────
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
// Helper: commit current mask → new canvas image, then reset segment state
// ─────────────────────────────────────────────────────────────────────────────
static void CommitExtraction()
{
    if (g_state.seg_image_index < 0 || g_state.prompt_points.empty()) return;

    SegmentResult result = g_segmenter.decode(g_state.prompt_points);
    if (!result.valid) {
        printf("[Segment] Decode returned invalid result.\n");
        return;
    }

    CanvasImage& src = g_state.images[g_state.seg_image_index];

    CanvasImage extracted;
    extracted.pixels   = src.pixels;
    extracted.size     = src.size;
    extracted.scale    = src.scale;
    extracted.selected = false;
    extracted.filename = src.filename + "_extracted";

    ApplyMaskToPixels(extracted.pixels, result.mask, (int)src.size.x, (int)src.size.y);
    extracted.texture_id = CreateTextureFromPixels(extracted.pixels, (int)src.size.x, (int)src.size.y);
    extracted.position   = ImVec2(src.position.x + src.size.x * src.scale + 20, src.position.y);

    g_state.images.push_back(extracted);
    g_state.status_msg   = "Extraction committed!";
    g_state.status_timer = 3.0f;
    printf("[Segment] Extraction committed — new image added to canvas.\n");

    // Reset segment state
    g_state.prompt_points.clear();
    g_state.seg_image_index = -1;
    g_state.image_encoded   = false;
    if (g_state.overlay_texture) {
        glDeleteTextures(1, &g_state.overlay_texture);
        g_state.overlay_texture = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Export a canvas image as PNG via save dialog
// ─────────────────────────────────────────────────────────────────────────────
void ExportImageAsPNG(int image_index)
{
    if (image_index < 0 || image_index >= (int)g_state.images.size()) return;
    CanvasImage& img = g_state.images[image_index];
    if (img.pixels.empty()) return;

    const char* filters[]  = {"*.png"};
    const char* save_path  = tinyfd_saveFileDialog("Export as PNG", "output.png",
                                                    1, filters, "PNG Image");
    if (!save_path) return;

    int w = (int)img.size.x, h = (int)img.size.y;
    int result = stbi_write_png(save_path, w, h, 4, img.pixels.data(), w * 4);

    if (result) {
        g_state.status_msg   = std::string("Exported: ") + save_path;
        g_state.status_timer = 4.0f;
        printf("[Export] Saved to %s\n", save_path);
    } else {
        g_state.status_msg   = "Export failed!";
        g_state.status_timer = 3.0f;
        printf("[Export] Failed to write %s\n", save_path);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1440, 900, "Deskapp", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
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
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]         = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Border]           = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_CheckMark]        = ImVec4(0.40f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.40f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load MobileSAM models
    if (!g_segmenter.loadModels("models/mobile_sam_encoder.onnx",
                                "models/mobile_sam_decoder.onnx")) {
        fprintf(stderr, "Warning: Could not load MobileSAM models.\n");
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // ── Check async encode completion ─────────────────────────────────────
        if (g_state.encode_done.exchange(false)) {
            if (g_state.encode_thread.joinable())
                g_state.encode_thread.join();

            g_state.image_encoded = g_state.encode_ok.load();
            g_state.is_encoding   = false;

            if (g_state.image_encoded) {
                // Auto-decode with any queued points
                if (!g_state.prompt_points.empty()) {
                    SegmentResult r = g_segmenter.decode(g_state.prompt_points);
                    QueueOverlay(r);
                }
                g_state.status_msg   = "Image encoded. Click to segment.";
                g_state.status_timer = 3.0f;
            } else {
                g_state.status_msg   = "Encode failed!";
                g_state.status_timer = 3.0f;
            }
        }

        // ── Flush pending overlay to GPU ──────────────────────────────────────
        FlushPendingOverlay();

        // ── Status timer ──────────────────────────────────────────────────────
        if (g_state.status_timer > 0.f)
            g_state.status_timer -= io.DeltaTime;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImVec2 window_size = ImGui::GetIO().DisplaySize;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(window_size);
        ImGui::Begin("MainWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground);

        RenderTopBar(window_size);
        RenderToolbar(window_size);

        ImVec2 canvas_pos  = ImVec2(60, 56);
        ImVec2 canvas_size = ImVec2(window_size.x - 60, window_size.y - 56);

        ImGui::SetCursorPos(canvas_pos);
        ImGui::BeginChild("Canvas", canvas_size, false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        RenderCanvas(draw_list, canvas_pos, canvas_size,
                     g_state.canvas_pan, g_state.canvas_zoom);

        ImGui::EndChild();

        // ── Status bar ────────────────────────────────────────────────────────
        if (g_state.status_timer > 0.f && !g_state.status_msg.empty()) {
            ImGui::SetNextWindowPos(ImVec2(canvas_pos.x + 10, window_size.y - 36));
            ImGui::SetNextWindowBgAlpha(0.75f);
            ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav);
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.f),
                               "%s", g_state.status_msg.c_str());
            ImGui::End();
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.10f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Wait for any running encode thread
    if (g_state.encode_thread.joinable())
        g_state.encode_thread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderTopBar(ImVec2 window_size)
{
    const float bar_height = 56.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(window_size.x, bar_height),
                             IM_COL32(20, 20, 20, 255));
    draw_list->AddLine(ImVec2(0, bar_height), ImVec2(window_size.x, bar_height),
                       IM_COL32(40, 40, 40, 255), 1.0f);

    ImGui::SetCursorPos(ImVec2(20, 16));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::Text("Deskapp");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(120, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(4, 0));

    if (ImGui::Button("File")) {}
    ImGui::SameLine();
    if (ImGui::Button("Edit")) {}
    ImGui::SameLine();
    if (ImGui::Button("View")) {}
    ImGui::SameLine();

    // ── Image button: load image ──────────────────────────────────────────────
    if (ImGui::Button("Image")) {
        const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp"};
        const char* filepath = tinyfd_openFileDialog("Select Image", "", 4, filters,
                                                     "Image Files", 0);
        if (filepath) {
            CanvasImage img;
            img.scale    = 1.0f;
            img.selected = false;
            img.filename = filepath;

            int w, h;
            img.texture_id = LoadTextureFromFile(filepath, &w, &h, img.pixels);
            if (img.texture_id) {
                img.size     = ImVec2((float)w, (float)h);
                img.position = ImVec2(-(float)w * 0.5f, -(float)h * 0.5f);
                g_state.images.push_back(img);
            }
        }
    }
    ImGui::SameLine();

    // ── Export button: save selected image ────────────────────────────────────
    bool has_selected = (g_state.selected_image_index >= 0 &&
                         g_state.selected_image_index < (int)g_state.images.size());
    if (!has_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.4f, 0.4f, 0.4f, 1.f));
    }
    if (ImGui::Button("Export PNG")) {
        if (has_selected)
            ExportImageAsPNG(g_state.selected_image_index);
    }
    if (!has_selected) {
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Select an image first");
    }

    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderToolbar(ImVec2 window_size)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(0, 56), ImVec2(60, window_size.y),
                             IM_COL32(20, 20, 20, 255));

    ImGui::SetCursorPos(ImVec2(10, 66));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0, 4));

    // Select
    bool sel = (g_state.current_tool == AppState::TOOL_SELECT);
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 1.0f, 0.3f));
    if (ImGui::Button("V", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_SELECT;
    if (sel) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select Tool (V)");

    // Hand
    bool hand = (g_state.current_tool == AppState::TOOL_HAND);
    if (hand) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 1.0f, 0.3f));
    if (ImGui::Button("H", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_HAND;
    if (hand) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hand Tool (H)");

    // Segment
    bool seg = (g_state.current_tool == AppState::TOOL_SEGMENT);
    if (seg) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 1.0f, 0.6f, 0.3f));
    if (ImGui::Button("S", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_SEGMENT;
    if (seg) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Segment Tool (S)\nLeft = foreground\nRight = background\nEnter = extract\nEsc = cancel");

    ImGui::PopStyleVar(2);
    ImGui::EndGroup();

    // Status indicators
    if (g_state.is_encoding) {
        ImGui::SetCursorPos(ImVec2(5, 210));
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "enc..");
    } else if (g_state.image_encoded && !g_state.prompt_points.empty()) {
        ImGui::SetCursorPos(ImVec2(5, 210));
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%dpt",
                           (int)g_state.prompt_points.size());
    }

    // Keyboard shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)) g_state.current_tool = AppState::TOOL_SELECT;
        if (ImGui::IsKeyPressed(ImGuiKey_H)) g_state.current_tool = AppState::TOOL_HAND;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) g_state.current_tool = AppState::TOOL_SEGMENT;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size,
                  ImVec2& pan_offset, float& zoom_level)
{
    ImVec2 canvas_min = canvas_pos;
    ImVec2 canvas_max = ImVec2(canvas_pos.x + canvas_size.x,
                               canvas_pos.y + canvas_size.y);

    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(25, 25, 25, 255));

    // Grid
    const float grid_size  = 50.0f * zoom_level;
    const ImU32 grid_color = IM_COL32(40, 40, 40, 255);
    float sx0 = canvas_min.x + fmodf(pan_offset.x * zoom_level, grid_size);
    float sy0 = canvas_min.y + fmodf(pan_offset.y * zoom_level, grid_size);
    for (float x = sx0; x < canvas_max.x; x += grid_size)
        draw_list->AddLine(ImVec2(x, canvas_min.y), ImVec2(x, canvas_max.y), grid_color);
    for (float y = sy0; y < canvas_max.y; y += grid_size)
        draw_list->AddLine(ImVec2(canvas_min.x, y), ImVec2(canvas_max.x, y), grid_color);

    // Draw images + overlays
    for (size_t i = 0; i < g_state.images.size(); i++) {
        CanvasImage& img = g_state.images[i];

        ImVec2 screen_pos = ImVec2(
            canvas_min.x + canvas_size.x * 0.5f + (img.position.x + pan_offset.x) * zoom_level,
            canvas_min.y + canvas_size.y * 0.5f + (img.position.y + pan_offset.y) * zoom_level
        );
        ImVec2 screen_size = ImVec2(
            img.size.x * img.scale * zoom_level,
            img.size.y * img.scale * zoom_level
        );
        ImVec2 screen_br = ImVec2(screen_pos.x + screen_size.x,
                                  screen_pos.y + screen_size.y);

        draw_list->AddImage((void*)(intptr_t)img.texture_id, screen_pos, screen_br);

        // Mask overlay for active segment
        if ((int)i == g_state.seg_image_index && g_state.overlay_texture) {
            draw_list->AddImage((void*)(intptr_t)g_state.overlay_texture,
                                screen_pos, screen_br);
        }

        // Prompt points
        if ((int)i == g_state.seg_image_index) {
            for (const auto& pt : g_state.prompt_points) {
                float px = screen_pos.x + pt.x * img.scale * zoom_level;
                float py = screen_pos.y + pt.y * img.scale * zoom_level;
                ImU32 col = (pt.label == 1)
                    ? IM_COL32(50, 220, 80, 255)
                    : IM_COL32(220, 60, 60, 255);
                draw_list->AddCircleFilled(ImVec2(px, py), 6.0f * zoom_level, col);
                draw_list->AddCircle(ImVec2(px, py), 6.0f * zoom_level,
                                     IM_COL32(255, 255, 255, 200), 12, 1.5f);
            }
        }

        if (img.selected) {
            draw_list->AddRect(screen_pos, screen_br,
                               IM_COL32(100, 150, 255, 255), 0, 0, 2.0f);
        }
    }

    // Encoding spinner (rotating arc)
    if (g_state.is_encoding) {
        ImVec2 center = ImVec2(canvas_min.x + canvas_size.x * 0.5f,
                               canvas_min.y + canvas_size.y * 0.5f);
        float t = (float)glfwGetTime();
        float r = 24.0f;
        draw_list->AddCircle(center, r, IM_COL32(60, 60, 60, 200), 32, 3.0f);
        for (int j = 0; j < 8; j++) {
            float angle = t * 3.0f + j * (3.14159f * 2.0f / 8.0f);
            float alpha = (float)j / 8.0f * 180.0f;
            draw_list->AddCircleFilled(
                ImVec2(center.x + cosf(angle) * r, center.y + sinf(angle) * r),
                3.5f, IM_COL32(80, 180, 255, (int)alpha));
        }
        draw_list->AddText(ImVec2(center.x - 28.f, center.y + r + 8.f),
                           IM_COL32(180, 180, 180, 255), "Encoding...");
    }

    // Crosshair at origin
    ImVec2 origin = ImVec2(
        canvas_min.x + canvas_size.x * 0.5f + pan_offset.x * zoom_level,
        canvas_min.y + canvas_size.y * 0.5f + pan_offset.y * zoom_level
    );
    draw_list->AddLine(ImVec2(origin.x - 10, origin.y), ImVec2(origin.x + 10, origin.y),
                       IM_COL32(100, 150, 255, 255), 2.0f);
    draw_list->AddLine(ImVec2(origin.x, origin.y - 10), ImVec2(origin.x, origin.y + 10),
                       IM_COL32(100, 150, 255, 255), 2.0f);

    // ── Input ─────────────────────────────────────────────────────────────────
    ImGuiIO& io = ImGui::GetIO();
    bool in_canvas = ImGui::IsMouseHoveringRect(canvas_min, canvas_max);

    if (in_canvas) {
        // ── Zoom toward cursor ────────────────────────────────────────────────
        if (io.MouseWheel != 0.0f) {
            float old_zoom = zoom_level;
            zoom_level = std::max(0.1f, std::min(5.0f, zoom_level + io.MouseWheel * 0.1f));
            float zoom_ratio = zoom_level / old_zoom;

            // Mouse position relative to canvas centre
            float mx = io.MousePos.x - (canvas_min.x + canvas_size.x * 0.5f);
            float my = io.MousePos.y - (canvas_min.y + canvas_size.y * 0.5f);

            // Adjust pan so the point under the cursor stays fixed
            pan_offset.x = mx / zoom_level - mx / old_zoom + pan_offset.x * zoom_ratio / zoom_ratio;
            pan_offset.y = my / zoom_level - my / old_zoom + pan_offset.y * zoom_ratio / zoom_ratio;

            // Simplified correct form:
            pan_offset.x += (mx / zoom_level - mx / old_zoom);
            pan_offset.y += (my / zoom_level - my / old_zoom);
        }

        // Mouse → world space
        ImVec2 mouse_world = ImVec2(
            (io.MousePos.x - canvas_min.x - canvas_size.x * 0.5f) / zoom_level - pan_offset.x,
            (io.MousePos.y - canvas_min.y - canvas_size.y * 0.5f) / zoom_level - pan_offset.y
        );

        // ── Select tool ───────────────────────────────────────────────────────
        if (g_state.current_tool == AppState::TOOL_SELECT) {
            if (ImGui::IsMouseClicked(0)) {
                g_state.selected_image_index = -1;
                for (int i = (int)g_state.images.size() - 1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];
                    if (mouse_world.x >= img.position.x &&
                        mouse_world.x <= img.position.x + img.size.x * img.scale &&
                        mouse_world.y >= img.position.y &&
                        mouse_world.y <= img.position.y + img.size.y * img.scale)
                    {
                        g_state.selected_image_index = i;
                        g_state.is_dragging_image    = true;
                        g_state.drag_start_pos       = mouse_world;
                        img.selected = true;
                    } else {
                        img.selected = false;
                    }
                }
            }
            if (g_state.is_dragging_image && g_state.selected_image_index >= 0) {
                if (ImGui::IsMouseDown(0)) {
                    CanvasImage& img = g_state.images[g_state.selected_image_index];
                    img.position.x += mouse_world.x - g_state.drag_start_pos.x;
                    img.position.y += mouse_world.y - g_state.drag_start_pos.y;
                    g_state.drag_start_pos = mouse_world;
                } else {
                    g_state.is_dragging_image = false;
                }
            }

            // Delete selected image with Delete/Backspace
            if (g_state.selected_image_index >= 0 &&
                (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                 ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
                glDeleteTextures(1, &g_state.images[g_state.selected_image_index].texture_id);
                g_state.images.erase(g_state.images.begin() + g_state.selected_image_index);
                g_state.selected_image_index = -1;
            }

        // ── Hand tool ─────────────────────────────────────────────────────────
        } else if (g_state.current_tool == AppState::TOOL_HAND) {
            bool should_pan = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(2);
            if (should_pan) {
                if (!g_state.is_panning) {
                    g_state.is_panning     = true;
                    g_state.last_mouse_pos = io.MousePos;
                }
                pan_offset.x += (io.MousePos.x - g_state.last_mouse_pos.x) / zoom_level;
                pan_offset.y += (io.MousePos.y - g_state.last_mouse_pos.y) / zoom_level;
                g_state.last_mouse_pos = io.MousePos;
            } else {
                g_state.is_panning = false;
            }

        // ── Segment tool ──────────────────────────────────────────────────────
        } else if (g_state.current_tool == AppState::TOOL_SEGMENT) {

            bool clicked_fg = ImGui::IsMouseClicked(0);
            bool clicked_bg = ImGui::IsMouseClicked(1);

            if ((clicked_fg || clicked_bg) && !g_state.is_encoding) {
                for (int i = (int)g_state.images.size() - 1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];
                    float x1 = img.position.x;
                    float y1 = img.position.y;
                    float x2 = img.position.x + img.size.x * img.scale;
                    float y2 = img.position.y + img.size.y * img.scale;

                    if (mouse_world.x < x1 || mouse_world.x > x2 ||
                        mouse_world.y < y1 || mouse_world.y > y2) continue;

                    if (img.pixels.empty()) { printf("[Segment] No pixel data.\n"); break; }
                    if (!g_segmenter.isReady()) {
                        g_state.status_msg   = "Models not loaded!";
                        g_state.status_timer = 3.0f;
                        break;
                    }

                    // Switching to a new image → async encode
                    if (i != g_state.seg_image_index) {
                        // Wait for previous encode if running
                        if (g_state.encode_thread.joinable())
                            g_state.encode_thread.join();

                        g_state.prompt_points.clear();
                        g_state.seg_image_index = i;
                        g_state.image_encoded   = false;
                        g_state.encode_ok       = false;
                        if (g_state.overlay_texture) {
                            glDeleteTextures(1, &g_state.overlay_texture);
                            g_state.overlay_texture = 0;
                        }

                        g_state.is_encoding  = true;
                        g_state.encode_done  = false;

                        // Capture data for thread
                        const uint8_t* px_ptr = img.pixels.data();
                        int iw = (int)img.size.x, ih = (int)img.size.y;

                        g_state.encode_thread = std::thread([px_ptr, iw, ih]() {
                            bool ok = g_segmenter.encodeImage(px_ptr, iw, ih);
                            g_state.encode_ok   = ok;
                            g_state.encode_done = true;
                        });

                        // Queue the click point — will be decoded once encode finishes
                        float local_x = (mouse_world.x - x1) / img.scale;
                        float local_y = (mouse_world.y - y1) / img.scale;
                        g_state.prompt_points.push_back({local_x, local_y, clicked_fg ? 1 : 0});
                        g_state.status_msg   = "Encoding image...";
                        g_state.status_timer = 60.0f; // hold until encode done
                        break;
                    }

                    // Same image — add point and decode immediately
                    float local_x = (mouse_world.x - x1) / img.scale;
                    float local_y = (mouse_world.y - y1) / img.scale;
                    int   label   = clicked_fg ? 1 : 0;

                    g_state.prompt_points.push_back({local_x, local_y, label});
                    printf("[Segment] Added %s point (%.1f, %.1f), total=%d\n",
                           label ? "FG" : "BG", local_x, local_y,
                           (int)g_state.prompt_points.size());

                    SegmentResult result = g_segmenter.decode(g_state.prompt_points);
                    QueueOverlay(result);
                    break;
                }
            }

            // Enter = commit extraction
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && g_state.image_encoded) {
                CommitExtraction();
            }

            // Escape = cancel
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_state.prompt_points.clear();
                g_state.seg_image_index = -1;
                g_state.image_encoded   = false;
                if (g_state.overlay_texture) {
                    glDeleteTextures(1, &g_state.overlay_texture);
                    g_state.overlay_texture = 0;
                }
                printf("[Segment] Cancelled.\n");
            }
        }

        // Space + drag = pan in any tool
        if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(0)) {
            if (!g_state.is_panning) {
                g_state.is_panning     = true;
                g_state.last_mouse_pos = io.MousePos;
            }
            pan_offset.x += (io.MousePos.x - g_state.last_mouse_pos.x) / zoom_level;
            pan_offset.y += (io.MousePos.y - g_state.last_mouse_pos.y) / zoom_level;
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
GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height,
                           std::vector<unsigned char>& out_pixels)
{
    int w, h, ch;
    unsigned char* data = stbi_load(filename, &w, &h, &ch, 4);
    if (!data) { fprintf(stderr, "Failed to load: %s\n", filename); return 0; }

    out_pixels.assign(data, data + w * h * 4);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);
    *out_width = w; *out_height = h;
    return tex;
}

GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int w, int h)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return tex;
}

void UpdateTextureFromPixels(GLuint tex_id, const std::vector<unsigned char>& pixels, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
}

void ApplyMaskToPixels(std::vector<unsigned char>& pixels,
                       const std::vector<uint8_t>& mask, int width, int height)
{
    for (int i = 0; i < width * height; i++) {
        if (mask[i] == 0) pixels[i * 4 + 3] = 0;
    }
}