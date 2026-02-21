#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <vector>
#include <string>

#include "stb_image.h"
#include "tinyfiledialogs.h"
#include "segmenter.h"

// Forward declarations
void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size, ImVec2& pan_offset, float& zoom_level);
void RenderToolbar(ImVec2 window_size);
void RenderTopBar(ImVec2 window_size);
GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height, std::vector<unsigned char>& out_pixels);
GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int width, int height);
void ApplyMaskToPixels(std::vector<unsigned char>& pixels, const std::vector<unsigned char>& mask, int width, int height);

// Image object on canvas
struct CanvasImage {
    GLuint texture_id;
    ImVec2 position;  // World space position
    ImVec2 size;      // Original size
    float scale;
    bool selected;
    std::string filename;
    std::vector<unsigned char> pixels; // raw RGBA pixel data (width * height * 4)
};

// Application state
struct AppState {
    ImVec2 canvas_pan = ImVec2(0, 0);
    float canvas_zoom = 1.0f;
    bool is_panning = false;
    ImVec2 last_mouse_pos = ImVec2(0, 0);

    enum Tool { TOOL_SELECT, TOOL_HAND, TOOL_SEGMENT };
    Tool current_tool = TOOL_SELECT;

    std::vector<CanvasImage> images;
    int selected_image_index = -1;
    bool is_dragging_image = false;
    ImVec2 drag_start_pos;

    bool is_segmenting = false;
} g_state;

// Global segmenter
Segmenter g_segmenter;

int main(int argc, char** argv) {
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

    // Enable alpha blending so transparent pixels render correctly
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

    // Load segmentation model at startup
    if (!g_segmenter.load("models/FastSAM-s.onnx")) {
        fprintf(stderr, "Warning: Could not load segmentation model.\n");
    }

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImVec2 window_size = ImGui::GetIO().DisplaySize;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(window_size);
        ImGui::Begin("MainWindow", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
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
        RenderCanvas(draw_list, canvas_pos, canvas_size, g_state.canvas_pan, g_state.canvas_zoom);

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

void RenderTopBar(ImVec2 window_size) {
    const float bar_height = 56.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(window_size.x, bar_height), IM_COL32(20, 20, 20, 255));
    draw_list->AddLine(ImVec2(0, bar_height), ImVec2(window_size.x, bar_height), IM_COL32(40, 40, 40, 255), 1.0f);

    ImGui::SetCursorPos(ImVec2(20, 16));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("Deskapp");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(120, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));

    if (ImGui::Button("File")) {}
    ImGui::SameLine();
    if (ImGui::Button("Edit")) {}
    ImGui::SameLine();
    if (ImGui::Button("View")) {}
    ImGui::SameLine();
    if (ImGui::Button("Image")) {
        const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp"};
        const char* filepath = tinyfd_openFileDialog("Select Image", "", 4, filters, "Image Files", 0);
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

void RenderToolbar(ImVec2 window_size) {
    const float toolbar_width = 60.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        ImVec2(0, 56),
        ImVec2(toolbar_width, window_size.y),
        IM_COL32(20, 20, 20, 255)
    );

    ImGui::SetCursorPos(ImVec2(10, 66));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));

    // Select tool
    bool select_active = (g_state.current_tool == AppState::TOOL_SELECT);
    if (select_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.60f, 1.00f, 0.3f));
    if (ImGui::Button("V", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_SELECT;
    if (select_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select Tool (V)");

    // Hand tool
    bool hand_active = (g_state.current_tool == AppState::TOOL_HAND);
    if (hand_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.60f, 1.00f, 0.3f));
    if (ImGui::Button("H", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_HAND;
    if (hand_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hand Tool (H)");

    // Segment tool
    bool seg_active = (g_state.current_tool == AppState::TOOL_SEGMENT);
    if (seg_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 1.00f, 0.60f, 0.3f));
    if (ImGui::Button("S", ImVec2(40, 40))) g_state.current_tool = AppState::TOOL_SEGMENT;
    if (seg_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Segment Tool - Click object to extract");

    ImGui::PopStyleVar(2);
    ImGui::EndGroup();

    // Spinner while segmenting
    if (g_state.is_segmenting) {
        ImGui::SetCursorPos(ImVec2(8, 200));
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "...");
    }
}

void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size, ImVec2& pan_offset, float& zoom_level) {
    ImVec2 canvas_min = canvas_pos;
    ImVec2 canvas_max = ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);

    // Background
    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(25, 25, 25, 255));

    // Grid
    const float grid_size  = 50.0f * zoom_level;
    const ImU32 grid_color = IM_COL32(40, 40, 40, 255);
    float start_x = canvas_min.x + fmodf(pan_offset.x * zoom_level, grid_size);
    float start_y = canvas_min.y + fmodf(pan_offset.y * zoom_level, grid_size);
    for (float x = start_x; x < canvas_max.x; x += grid_size)
        draw_list->AddLine(ImVec2(x, canvas_min.y), ImVec2(x, canvas_max.y), grid_color, 1.0f);
    for (float y = start_y; y < canvas_max.y; y += grid_size)
        draw_list->AddLine(ImVec2(canvas_min.x, y), ImVec2(canvas_max.x, y), grid_color, 1.0f);

    // Draw images
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

        draw_list->AddImage(
            (void*)(intptr_t)img.texture_id,
            screen_pos,
            ImVec2(screen_pos.x + screen_size.x, screen_pos.y + screen_size.y)
        );

        if (img.selected) {
            draw_list->AddRect(
                screen_pos,
                ImVec2(screen_pos.x + screen_size.x, screen_pos.y + screen_size.y),
                IM_COL32(100, 150, 255, 255), 0.0f, 0, 2.0f
            );
        }
    }

    // Crosshair at origin
    ImVec2 center = ImVec2(
        canvas_min.x + canvas_size.x * 0.5f + pan_offset.x * zoom_level,
        canvas_min.y + canvas_size.y * 0.5f + pan_offset.y * zoom_level
    );
    draw_list->AddLine(ImVec2(center.x - 10, center.y), ImVec2(center.x + 10, center.y), IM_COL32(100, 150, 255, 255), 2.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - 10), ImVec2(center.x, center.y + 10), IM_COL32(100, 150, 255, 255), 2.0f);

    // Input
    ImGuiIO& io = ImGui::GetIO();
    bool is_mouse_in_canvas = ImGui::IsMouseHoveringRect(canvas_min, canvas_max);

    if (is_mouse_in_canvas) {
        // Zoom with scroll
        if (io.MouseWheel != 0.0f) {
            zoom_level = std::max(0.1f, std::min(5.0f, zoom_level + io.MouseWheel * 0.1f));
        }

        // Mouse position in world space
        ImVec2 mouse_world = ImVec2(
            (io.MousePos.x - canvas_min.x - canvas_size.x * 0.5f) / zoom_level - pan_offset.x,
            (io.MousePos.y - canvas_min.y - canvas_size.y * 0.5f) / zoom_level - pan_offset.y
        );

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
                    ImVec2 delta = ImVec2(
                        mouse_world.x - g_state.drag_start_pos.x,
                        mouse_world.y - g_state.drag_start_pos.y
                    );
                    img.position.x     += delta.x;
                    img.position.y     += delta.y;
                    g_state.drag_start_pos = mouse_world;
                } else {
                    g_state.is_dragging_image = false;
                }
            }

        } else if (g_state.current_tool == AppState::TOOL_HAND) {
            bool should_pan = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(2);
            if (should_pan) {
                if (!g_state.is_panning) {
                    g_state.is_panning     = true;
                    g_state.last_mouse_pos = io.MousePos;
                }
                ImVec2 delta = ImVec2(
                    io.MousePos.x - g_state.last_mouse_pos.x,
                    io.MousePos.y - g_state.last_mouse_pos.y
                );
                pan_offset.x          += delta.x / zoom_level;
                pan_offset.y          += delta.y / zoom_level;
                g_state.last_mouse_pos = io.MousePos;
            } else {
                g_state.is_panning = false;
            }

        } else if (g_state.current_tool == AppState::TOOL_SEGMENT) {
            if (ImGui::IsMouseClicked(0) && !g_state.is_segmenting) {
                for (int i = (int)g_state.images.size() - 1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];

                    float x1 = img.position.x;
                    float y1 = img.position.y;
                    float x2 = img.position.x + img.size.x * img.scale;
                    float y2 = img.position.y + img.size.y * img.scale;

                    if (mouse_world.x >= x1 && mouse_world.x <= x2 &&
                        mouse_world.y >= y1 && mouse_world.y <= y2)
                    {
                        if (img.pixels.empty()) {
                            fprintf(stderr, "[Segment] No pixel data for image %d\n", i);
                            break;
                        }
                        if (!g_segmenter.is_loaded()) {
                            fprintf(stderr, "[Segment] Segmenter not loaded\n");
                            break;
                        }

                        // Convert click to image-local pixel coordinates
                        float local_x = (mouse_world.x - x1) / img.scale;
                        float local_y = (mouse_world.y - y1) / img.scale;

                        printf("[Segment] Image %d, click=(%.1f, %.1f)\n", i, local_x, local_y);
                        g_state.is_segmenting = true;

                        SegmentResult result = g_segmenter.run(
                            img.pixels,
                            (int)img.size.x,
                            (int)img.size.y,
                            local_x,
                            local_y
                        );

                        g_state.is_segmenting = false;

                        if (result.success) {
                            CanvasImage extracted;
                            extracted.pixels   = img.pixels;
                            extracted.size     = img.size;
                            extracted.scale    = img.scale;
                            extracted.selected = false;
                            extracted.filename = img.filename + "_extracted";

                            // Zero out alpha where mask is 0
                            ApplyMaskToPixels(extracted.pixels, result.mask, (int)img.size.x, (int)img.size.y);

                            extracted.texture_id = CreateTextureFromPixels(
                                extracted.pixels, (int)img.size.x, (int)img.size.y
                            );

                            // Place offset so both images are visible
                            extracted.position = ImVec2(x2 + 20, y1);

                            g_state.images.push_back(extracted);
                            printf("[Segment] Done — extracted image added to canvas.\n");
                        } else {
                            printf("[Segment] No segment found at click point.\n");
                        }
                        break;
                    }
                }
            }
        }

        // Space + drag to pan regardless of tool
        if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(0)) {
            if (!g_state.is_panning) {
                g_state.is_panning     = true;
                g_state.last_mouse_pos = io.MousePos;
            }
            ImVec2 delta = ImVec2(
                io.MousePos.x - g_state.last_mouse_pos.x,
                io.MousePos.y - g_state.last_mouse_pos.y
            );
            pan_offset.x          += delta.x / zoom_level;
            pan_offset.y          += delta.y / zoom_level;
            g_state.last_mouse_pos = io.MousePos;
        } else if (!ImGui::IsKeyDown(ImGuiKey_Space)) {
            if (g_state.current_tool != AppState::TOOL_HAND)
                g_state.is_panning = false;
        }

    } else {
        g_state.is_panning = false;
    }
}

GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height, std::vector<unsigned char>& out_pixels) {
    int width, height, channels;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
        fprintf(stderr, "Failed to load image: %s\n", filename);
        return 0;
    }

    // Keep raw pixels for segmentation
    out_pixels.assign(data, data + width * height * 4);

    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);

    *out_width  = width;
    *out_height = height;
    return texture_id;
}

GLuint CreateTextureFromPixels(const std::vector<unsigned char>& pixels, int width, int height) {
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return texture_id;
}

void ApplyMaskToPixels(std::vector<unsigned char>& pixels, const std::vector<unsigned char>& mask, int width, int height) {
    for (int i = 0; i < width * height; i++) {
        if (mask[i] == 0) {
            pixels[i * 4 + 3] = 0; // transparent
        }
    }
}