#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <vector>
#include <string>

#include "stb_image.h"
#include "tinyfiledialogs.h"

// Forward declarations
void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size, ImVec2& pan_offset, float& zoom_level);
void RenderToolbar(ImVec2 window_size);
void RenderTopBar(ImVec2 window_size);
GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height);

// Image object on canvas
struct CanvasImage {
    GLuint texture_id;
    ImVec2 position;  // World space position
    ImVec2 size;      // Original size
    float scale;
    bool selected;
    std::string filename;
};

// Application state
struct AppState {
    ImVec2 canvas_pan = ImVec2(0, 0);
    float canvas_zoom = 1.0f;
    bool is_panning = false;
    ImVec2 last_mouse_pos = ImVec2(0, 0);
    
    enum Tool { TOOL_SELECT, TOOL_HAND };
    Tool current_tool = TOOL_SELECT;
    
    std::vector<CanvasImage> images;
    int selected_image_index = -1;
    bool is_dragging_image = false;
    ImVec2 drag_start_pos;
} g_state;

int main(int argc, char** argv) {
    // Initialize GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif

    // Create window
    GLFWwindow* window = glfwCreateWindow(1440, 900, "Deskapp", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup ImGui style - Dark theme with custom colors
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    
    // Custom color scheme
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    
    // Accent color - blue
    colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Get window size
        ImVec2 window_size = ImGui::GetIO().DisplaySize;

        // Create fullscreen window without decorations
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

        // Top bar (56px height)
        RenderTopBar(window_size);

        // Left toolbar (60px width)
        RenderToolbar(window_size);

        // Main canvas area
        ImVec2 canvas_pos = ImVec2(60, 56);
        ImVec2 canvas_size = ImVec2(window_size.x - 60, window_size.y - 56);
        
        ImGui::SetCursorPos(canvas_pos);
        ImGui::BeginChild("Canvas", canvas_size, false, 
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        RenderCanvas(draw_list, canvas_pos, canvas_size, g_state.canvas_pan, g_state.canvas_zoom);
        
        ImGui::EndChild();
        ImGui::End();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.10f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
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
    ImVec2 bar_min = ImVec2(0, 0);
    ImVec2 bar_max = ImVec2(window_size.x, bar_height);
    
    // Background
    draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(20, 20, 20, 255));
    
    // Bottom border
    draw_list->AddLine(
        ImVec2(0, bar_height), 
        ImVec2(window_size.x, bar_height), 
        IM_COL32(40, 40, 40, 255), 
        1.0f
    );
    
    // Logo/Title
    ImGui::SetCursorPos(ImVec2(20, 16));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("Deskapp");
    ImGui::PopStyleColor();
    
    // Menu items
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
        // Open file picker
        const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif"};
        const char* filepath = tinyfd_openFileDialog(
            "Select Image",
            "",
            5,
            filters,
            "Image Files",
            0
        );
        
        if (filepath) {
            int width, height;
            GLuint texture_id = LoadTextureFromFile(filepath, &width, &height);
            if (texture_id) {
                CanvasImage img;
                img.texture_id = texture_id;
                img.position = ImVec2(0, 0); // Center of canvas
                img.size = ImVec2((float)width, (float)height);
                img.scale = 1.0f;
                img.selected = false;
                img.filename = filepath;
                g_state.images.push_back(img);
            }
        }
    }
    
    ImGui::PopStyleVar(2);
    
    // Right side - zoom indicator
    char zoom_text[32];
    snprintf(zoom_text, sizeof(zoom_text), "%.0f%%", g_state.canvas_zoom * 100.0f);
    ImVec2 text_size = ImGui::CalcTextSize(zoom_text);
    ImGui::SetCursorPos(ImVec2(window_size.x - text_size.x - 20, 18));
    ImGui::Text("%s", zoom_text);
}

void RenderToolbar(ImVec2 window_size) {
    const float toolbar_width = 60.0f;
    const float top_offset = 56.0f;
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 toolbar_min = ImVec2(0, top_offset);
    ImVec2 toolbar_max = ImVec2(toolbar_width, window_size.y);
    
    // Background
    draw_list->AddRectFilled(toolbar_min, toolbar_max, IM_COL32(18, 18, 18, 255));
    
    // Right border
    draw_list->AddLine(
        ImVec2(toolbar_width, top_offset), 
        ImVec2(toolbar_width, window_size.y), 
        IM_COL32(40, 40, 40, 255), 
        1.0f
    );
    
    // Tool buttons
    ImGui::SetCursorPos(ImVec2(10, top_offset + 10));
    ImGui::BeginGroup();
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));
    
    // Select tool
    bool select_selected = (g_state.current_tool == AppState::TOOL_SELECT);
    if (select_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.60f, 1.00f, 0.3f));
    }
    if (ImGui::Button("V", ImVec2(40, 40))) {
        g_state.current_tool = AppState::TOOL_SELECT;
    }
    if (select_selected) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select Tool (V)");
    }
    
    // Hand tool
    bool hand_selected = (g_state.current_tool == AppState::TOOL_HAND);
    if (hand_selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.60f, 1.00f, 0.3f));
    }
    if (ImGui::Button("H", ImVec2(40, 40))) {
        g_state.current_tool = AppState::TOOL_HAND;
    }
    if (hand_selected) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hand Tool (H)");
    }
    
    ImGui::PopStyleVar(2);
    ImGui::EndGroup();
}

void RenderCanvas(ImDrawList* draw_list, ImVec2 canvas_pos, ImVec2 canvas_size, ImVec2& pan_offset, float& zoom_level) {
    ImVec2 canvas_min = canvas_pos;
    ImVec2 canvas_max = ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);
    
    // Canvas background
    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(25, 25, 25, 255));
    
    // Grid pattern
    const float grid_size = 50.0f * zoom_level;
    const ImU32 grid_color = IM_COL32(40, 40, 40, 255);
    
    // Calculate grid offset based on pan
    float start_x = canvas_min.x + fmodf(pan_offset.x * zoom_level, grid_size);
    float start_y = canvas_min.y + fmodf(pan_offset.y * zoom_level, grid_size);
    
    // Draw vertical lines
    for (float x = start_x; x < canvas_max.x; x += grid_size) {
        draw_list->AddLine(ImVec2(x, canvas_min.y), ImVec2(x, canvas_max.y), grid_color, 1.0f);
    }
    
    // Draw horizontal lines
    for (float y = start_y; y < canvas_max.y; y += grid_size) {
        draw_list->AddLine(ImVec2(canvas_min.x, y), ImVec2(canvas_max.x, y), grid_color, 1.0f);
    }
    
    // Draw images
    for (size_t i = 0; i < g_state.images.size(); i++) {
        CanvasImage& img = g_state.images[i];
        
        // Transform world coordinates to screen coordinates
        ImVec2 screen_pos = ImVec2(
            canvas_min.x + canvas_size.x * 0.5f + (img.position.x + pan_offset.x) * zoom_level,
            canvas_min.y + canvas_size.y * 0.5f + (img.position.y + pan_offset.y) * zoom_level
        );
        
        ImVec2 screen_size = ImVec2(
            img.size.x * img.scale * zoom_level,
            img.size.y * img.scale * zoom_level
        );
        
        // Draw image
        draw_list->AddImage(
            (void*)(intptr_t)img.texture_id,
            screen_pos,
            ImVec2(screen_pos.x + screen_size.x, screen_pos.y + screen_size.y)
        );
        
        // Draw selection box if selected
        if (img.selected) {
            draw_list->AddRect(
                screen_pos,
                ImVec2(screen_pos.x + screen_size.x, screen_pos.y + screen_size.y),
                IM_COL32(100, 150, 255, 255),
                0.0f,
                0,
                2.0f
            );
        }
    }
    
    // Center indicator
    ImVec2 center = ImVec2(
        canvas_min.x + canvas_size.x * 0.5f + pan_offset.x * zoom_level,
        canvas_min.y + canvas_size.y * 0.5f + pan_offset.y * zoom_level
    );
    
    // Draw crosshair at origin
    draw_list->AddLine(
        ImVec2(center.x - 10, center.y), 
        ImVec2(center.x + 10, center.y), 
        IM_COL32(100, 150, 255, 255), 
        2.0f
    );
    draw_list->AddLine(
        ImVec2(center.x, center.y - 10), 
        ImVec2(center.x, center.y + 10), 
        IM_COL32(100, 150, 255, 255), 
        2.0f
    );
    
    // Handle input
    ImGuiIO& io = ImGui::GetIO();
    bool is_mouse_in_canvas = ImGui::IsMouseHoveringRect(canvas_min, canvas_max);
    
    if (is_mouse_in_canvas) {
        // Zoom with scroll wheel
        if (io.MouseWheel != 0.0f) {
            float zoom_delta = io.MouseWheel * 0.1f;
            zoom_level = zoom_level + zoom_delta;
            if (zoom_level < 0.1f) zoom_level = 0.1f;
            if (zoom_level > 5.0f) zoom_level = 5.0f;
        }
        
        // Get mouse position in world space
        ImVec2 mouse_world = ImVec2(
            (io.MousePos.x - canvas_min.x - canvas_size.x * 0.5f) / zoom_level - pan_offset.x,
            (io.MousePos.y - canvas_min.y - canvas_size.y * 0.5f) / zoom_level - pan_offset.y
        );
        
        // Handle tool-specific input
        if (g_state.current_tool == AppState::TOOL_SELECT) {
            // Check for image selection/dragging
            if (ImGui::IsMouseClicked(0)) {
                g_state.selected_image_index = -1;
                
                // Check if clicking on an image (reverse order for top-to-bottom)
                for (int i = (int)g_state.images.size() - 1; i >= 0; i--) {
                    CanvasImage& img = g_state.images[i];
                    
                    if (mouse_world.x >= img.position.x && 
                        mouse_world.x <= img.position.x + img.size.x * img.scale &&
                        mouse_world.y >= img.position.y && 
                        mouse_world.y <= img.position.y + img.size.y * img.scale) {
                        
                        g_state.selected_image_index = i;
                        g_state.is_dragging_image = true;
                        g_state.drag_start_pos = mouse_world;
                        img.selected = true;
                    } else {
                        img.selected = false;
                    }
                }
            }
            
            // Handle dragging
            if (g_state.is_dragging_image && g_state.selected_image_index >= 0) {
                if (ImGui::IsMouseDown(0)) {
                    CanvasImage& img = g_state.images[g_state.selected_image_index];
                    ImVec2 delta = ImVec2(
                        mouse_world.x - g_state.drag_start_pos.x,
                        mouse_world.y - g_state.drag_start_pos.y
                    );
                    img.position.x += delta.x;
                    img.position.y += delta.y;
                    g_state.drag_start_pos = mouse_world;
                } else {
                    g_state.is_dragging_image = false;
                }
            }
            
        } else if (g_state.current_tool == AppState::TOOL_HAND) {
            // Pan with left click
            bool should_pan = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(2);
            
            if (should_pan) {
                if (!g_state.is_panning) {
                    g_state.is_panning = true;
                    g_state.last_mouse_pos = io.MousePos;
                }
                ImVec2 delta = ImVec2(io.MousePos.x - g_state.last_mouse_pos.x, 
                                      io.MousePos.y - g_state.last_mouse_pos.y);
                pan_offset.x += delta.x / zoom_level;
                pan_offset.y += delta.y / zoom_level;
                g_state.last_mouse_pos = io.MousePos;
            } else {
                g_state.is_panning = false;
            }
        }
        
        // Space + drag to pan regardless of tool
        if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDown(0)) {
            if (!g_state.is_panning) {
                g_state.is_panning = true;
                g_state.last_mouse_pos = io.MousePos;
            }
            ImVec2 delta = ImVec2(io.MousePos.x - g_state.last_mouse_pos.x, 
                                  io.MousePos.y - g_state.last_mouse_pos.y);
            pan_offset.x += delta.x / zoom_level;
            pan_offset.y += delta.y / zoom_level;
            g_state.last_mouse_pos = io.MousePos;
        } else if (!ImGui::IsMouseDown(0) || g_state.current_tool != AppState::TOOL_HAND) {
            g_state.is_panning = false;
        }
    } else {
        g_state.is_panning = false;
    }
}

GLuint LoadTextureFromFile(const char* filename, int* out_width, int* out_height) {
    int width, height, channels;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
        fprintf(stderr, "Failed to load image: %s\n", filename);
        return 0;
    }
    
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    
    *out_width = width;
    *out_height = height;
    
    return texture_id;
}