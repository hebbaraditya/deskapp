#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

#include "stb_image.h"
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
    int  seg_image_index   = -1;    // which image is being segmented
    bool image_encoded     = false; // has encoder run on seg_image_index?
    bool is_encoding       = false;

    std::vector<PromptPoint> prompt_points; // accumulated fg/bg points
    GLuint overlay_texture = 0;             // live mask overlay
    int    overlay_w = 0, overlay_h = 0;

} g_state;

// ── Global segmenter ──────────────────────────────────────────────────────────
Segmenter g_segmenter;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: rebuild overlay texture from current mask
// ─────────────────────────────────────────────────────────────────────────────
static void RebuildOverlay(const SegmentResult& result)
{
    if (!result.valid) return;

    int w = result.width, h = result.height;
    std::vector<unsigned char> rgba(w * h * 4, 0);

    for (int i = 0; i < w * h; i++) {
        if (result.mask[i] > 0) {
            rgba[i * 4 + 0] = 80;
            rgba[i * 4 + 1] = 180;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = 120; // semi-transparent blue tint
        }
    }

    if (g_state.overlay_texture &&
        g_state.overlay_w == w && g_state.overlay_h == h) {
        UpdateTextureFromPixels(g_state.overlay_texture, rgba, w, h);
    } else {
        if (g_state.overlay_texture)
            glDeleteTextures(1, &g_state.overlay_texture);
        g_state.overlay_texture = CreateTextureFromPixels(rgba, w, h);
        g_state.overlay_w = w;
        g_state.overlay_h = h;
    }
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
        ImGui::SetTooltip("Segment Tool\nLeft click = foreground\nRight click = background\nEnter = extract");

    ImGui::PopStyleVar(2);
    ImGui::EndGroup();

    // Status indicators
    if (g_state.is_encoding) {
        ImGui::SetCursorPos(ImVec2(5, 210));
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "enc");
    }
    if (g_state.image_encoded && !g_state.prompt_points.empty()) {
        ImGui::SetCursorPos(ImVec2(5, 228));
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%d pt",
                           (int)g_state.prompt_points.size());
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

        // Draw mask overlay for the image being segmented
        if ((int)i == g_state.seg_image_index && g_state.overlay_texture) {
            draw_list->AddImage((void*)(intptr_t)g_state.overlay_texture,
                                screen_pos, screen_br);
        }

        // Draw prompt points
        if ((int)i == g_state.seg_image_index) {
            for (const auto& pt : g_state.prompt_points) {
                float px = screen_pos.x + pt.x * img.scale * zoom_level;
                float py = screen_pos.y + pt.y * img.scale * zoom_level;
                ImU32 col = (pt.label == 1)
                    ? IM_COL32(50, 220, 80, 255)   // green = foreground
                    : IM_COL32(220, 60, 60, 255);  // red   = background
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

    // Crosshair at origin
    ImVec2 center = ImVec2(
        canvas_min.x + canvas_size.x * 0.5f + pan_offset.x * zoom_level,
        canvas_min.y + canvas_size.y * 0.5f + pan_offset.y * zoom_level
    );
    draw_list->AddLine(ImVec2(center.x - 10, center.y), ImVec2(center.x + 10, center.y),
                       IM_COL32(100, 150, 255, 255), 2.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - 10), ImVec2(center.x, center.y + 10),
                       IM_COL32(100, 150, 255, 255), 2.0f);

    // ── Input ─────────────────────────────────────────────────────────────────
    ImGuiIO& io = ImGui::GetIO();
    bool in_canvas = ImGui::IsMouseHoveringRect(canvas_min, canvas_max);

    if (in_canvas) {
        // Zoom
        if (io.MouseWheel != 0.0f)
            zoom_level = std::max(0.1f, std::min(5.0f, zoom_level + io.MouseWheel * 0.1f));

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

            bool clicked_fg = ImGui::IsMouseClicked(0); // left  = foreground
            bool clicked_bg = ImGui::IsMouseClicked(1); // right = background

            if ((clicked_fg || clicked_bg) && !g_state.is_encoding) {
                // Find which image was clicked
                for (int i = (int)g_state.images.size() - 1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];
                    float x1 = img.position.x;
                    float y1 = img.position.y;
                    float x2 = img.position.x + img.size.x * img.scale;
                    float y2 = img.position.y + img.size.y * img.scale;

                    if (mouse_world.x < x1 || mouse_world.x > x2 ||
                        mouse_world.y < y1 || mouse_world.y > y2) continue;

                    if (img.pixels.empty()) { printf("[Segment] No pixel data.\n"); break; }
                    if (!g_segmenter.isReady()) { printf("[Segment] Models not loaded.\n"); break; }

                    // If switching to a new image, re-encode
                    if (i != g_state.seg_image_index) {
                        g_state.prompt_points.clear();
                        g_state.seg_image_index = i;
                        g_state.image_encoded   = false;
                        if (g_state.overlay_texture) {
                            glDeleteTextures(1, &g_state.overlay_texture);
                            g_state.overlay_texture = 0;
                        }

                        printf("[Segment] Encoding image %d (%dx%d)...\n",
                               i, (int)img.size.x, (int)img.size.y);
                        g_state.is_encoding = true;

                        bool ok = g_segmenter.encodeImage(
                            img.pixels.data(), (int)img.size.x, (int)img.size.y);

                        g_state.is_encoding   = false;
                        g_state.image_encoded = ok;

                        if (!ok) { printf("[Segment] Encode failed.\n"); break; }
                    }

                    // Add the point
                    float local_x = (mouse_world.x - x1) / img.scale;
                    float local_y = (mouse_world.y - y1) / img.scale;
                    int   label   = clicked_fg ? 1 : 0;

                    g_state.prompt_points.push_back({local_x, local_y, label});
                    printf("[Segment] Added %s point (%.1f, %.1f), total=%d\n",
                           label ? "FG" : "BG", local_x, local_y,
                           (int)g_state.prompt_points.size());

                    // Decode with updated points
                    SegmentResult result = g_segmenter.decode(g_state.prompt_points);
                    if (result.valid) RebuildOverlay(result);

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