#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "stb_image.h"
#include "tinyfiledialogs.h"
#include <utility> 

// Add these declarations at the top of your file
std::vector<ImFont*> loadedFonts;
std::vector<std::string> fontNames;


struct Image {
    GLuint texture;
    int width;
    int height;
    float zoom;
    ImVec2 position;
    ImVec2 targetPosition;
    std::string name;
    bool open;
    bool selected;
    bool mirrored;
    int uploadOrder;
    std::vector<unsigned char> data;
    bool eraserMode;
    int eraserSize;
    float rotation; 
    float targetRotation; // New member for rotation angle
    bool isHoveringZoomControl;
    int activeZoomCorner;
    ImVec2 zoomStartPos;
    float zoomStartValue;
    std::vector<unsigned char> pixelData;  // Add this field
    bool isTextImage;
    int originalWidth;
    int originalHeight;
};

std::vector<Image> images;
bool show_metrics = false;
int nextUploadOrder = 0;


struct ImageState {
    std::vector<Image> images;
    int nextUploadOrder;
};

struct Text {
    std::string content;
    ImVec2 position;
    ImVec4 fillColor;  // Renamed from color to fillColor
    ImVec4 strokeColor;  // New: color of the text outline
    float strokeWidth;   // New: width of the text outline
    float size;
    bool selected;
    int fontIndex;  // Add this line to store the font index
};
bool isAddTextPopupOpen = false;
std::vector<Text> texts;

std::vector<ImageState> undoStates;
std::vector<ImageState> redoStates;

ImVec2 gridOffset(0.0f, 0.0f);
float gridScale = 1.0f;

const char* FontGetter(void* vec, int idx)
{
    auto& vector = *static_cast<std::vector<std::string>*>(vec);
    if (idx >= 0 && idx < static_cast<int>(vector.size()))
        return vector[idx].c_str();
    return "Unknown";
}

// Declarations
std::pair<GLuint, std::vector<unsigned char>> RenderTextToTexture(const char* text, ImFont* font, float fontSize, ImVec4 fillColor, ImVec4 strokeColor, float strokeWidth);
void RenderTextToBuffer(std::vector<unsigned char>& buffer, int bufferWidth, int bufferHeight, 
                        const char* text, ImFont* font, float fontSize, float x, float y, ImVec4 color);
void DrawTriangle(std::vector<unsigned char>& buffer, int width, int height, ImVec2 pos[3], ImVec4 col);
bool PointInTriangle(ImVec2 pt, ImVec2 v1, ImVec2 v2, ImVec2 v3);
float Sign(ImVec2 p1, ImVec2 p2, ImVec2 p3);

// Function implementations
std::pair<GLuint, std::vector<unsigned char>> RenderTextToTexture(const char* text, ImFont* font, float fontSize, ImVec4 fillColor, ImVec4 strokeColor, float strokeWidth)
{
    // Calculate the size of the text
    ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    
    // Add some padding
    int texWidth = (int)(textSize.x + strokeWidth * 2 + 10);
    int texHeight = (int)(textSize.y + strokeWidth * 2 + 10);

    // Create an image buffer
    std::vector<unsigned char> imageBuffer(texWidth * texHeight * 4, 0);

    // Render stroke
    if (strokeWidth > 0)
    {
        for (float x = -strokeWidth; x <= strokeWidth; x += 0.5f)
        {
            for (float y = -strokeWidth; y <= strokeWidth; y += 0.5f)
            {
                RenderTextToBuffer(imageBuffer, texWidth, texHeight, text, font, fontSize, 
                                   strokeWidth + 5 + x, strokeWidth + 5 + y, strokeColor);
            }
        }
    }

    // Render fill
    RenderTextToBuffer(imageBuffer, texWidth, texHeight, text, font, fontSize, 
                       strokeWidth + 5, strokeWidth + 5, fillColor);

    // Create OpenGL texture
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, imageBuffer.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return std::make_pair(textureID, imageBuffer);
}

void RenderTextToBuffer(std::vector<unsigned char>& buffer, int bufferWidth, int bufferHeight, 
                        const char* text, ImFont* font, float fontSize, float x, float y, ImVec4 color)
{
    ImFontAtlas* atlas = font->ContainerAtlas;
    for (const char* c = text; *c != '\0'; c++)
    {
        const ImFontGlyph* glyph = font->FindGlyph(*c);
        if (!glyph) continue;

        float char_x = x + glyph->X0 * (fontSize / font->FontSize);
        float char_y = y + glyph->Y0 * (fontSize / font->FontSize);
        float char_w = (glyph->X1 - glyph->X0) * (fontSize / font->FontSize);
        float char_h = (glyph->Y1 - glyph->Y0) * (fontSize / font->FontSize);

        float tex_u0 = glyph->U0 * atlas->TexWidth;
        float tex_v0 = glyph->V0 * atlas->TexHeight;
        float tex_u1 = glyph->U1 * atlas->TexWidth;
        float tex_v1 = glyph->V1 * atlas->TexHeight;

        for (int py = 0; py < char_h; py++)
        {
            for (int px = 0; px < char_w; px++)
            {
                int buffer_x = (int)(char_x + px);
                int buffer_y = (int)(char_y + py);
                if (buffer_x < 0 || buffer_x >= bufferWidth || buffer_y < 0 || buffer_y >= bufferHeight)
                    continue;

                int tex_x = (int)(tex_u0 + (tex_u1 - tex_u0) * (px / char_w));
                int tex_y = (int)(tex_v0 + (tex_v1 - tex_v0) * (py / char_h));
                int tex_index = (tex_y * atlas->TexWidth + tex_x) * 4;

                float alpha = atlas->TexPixelsAlpha8[tex_y * atlas->TexWidth + tex_x] / 255.0f;

                int buffer_index = (buffer_y * bufferWidth + buffer_x) * 4;
                buffer[buffer_index] = (unsigned char)(color.x * 255.0f * alpha);
                buffer[buffer_index + 1] = (unsigned char)(color.y * 255.0f * alpha);
                buffer[buffer_index + 2] = (unsigned char)(color.z * 255.0f * alpha);
                buffer[buffer_index + 3] = (unsigned char)(color.w * 255.0f * alpha);
            }
        }

        x += glyph->AdvanceX * (fontSize / font->FontSize);
    }
}

void DrawTriangle(std::vector<unsigned char>& buffer, int width, int height, ImVec2 pos[3], ImVec4 col)
{
    ImVec2 bb_min = ImVec2(std::min({pos[0].x, pos[1].x, pos[2].x}), std::min({pos[0].y, pos[1].y, pos[2].y}));
    ImVec2 bb_max = ImVec2(std::max({pos[0].x, pos[1].x, pos[2].x}), std::max({pos[0].y, pos[1].y, pos[2].y}));

    for (int y = (int)bb_min.y; y <= (int)bb_max.y; y++)
    {
        for (int x = (int)bb_min.x; x <= (int)bb_max.x; x++)
        {
            if (PointInTriangle(ImVec2((float)x, (float)y), pos[0], pos[1], pos[2]))
            {
                if (x >= 0 && x < width && y >= 0 && y < height)
                {
                    int index = (y * width + x) * 4;
                    buffer[index] = (unsigned char)(col.x * 255.0f);
                    buffer[index + 1] = (unsigned char)(col.y * 255.0f);
                    buffer[index + 2] = (unsigned char)(col.z * 255.0f);
                    buffer[index + 3] = (unsigned char)(col.w * 255.0f);
                }
            }
        }
    }
}

bool PointInTriangle(ImVec2 pt, ImVec2 v1, ImVec2 v2, ImVec2 v3)
{
    float d1, d2, d3;
    bool has_neg, has_pos;

    d1 = Sign(pt, v1, v2);
    d2 = Sign(pt, v2, v3);
    d3 = Sign(pt, v3, v1);

    has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(has_neg && has_pos);
}

float Sign(ImVec2 p1, ImVec2 p2, ImVec2 p3)
{
    return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
}

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    
    // Clear any existing fonts
    io.Fonts->Clear();
    loadedFonts.clear();
    fontNames.clear();

    // Configure font loading
    ImFontConfig config;
    config.OversampleH = 4;
    config.OversampleV = 4;
    config.PixelSnapH = false;

    // Load the default font with increased size
    float baseFontSize = 24.0f; // Increase this for higher resolution
    ImFont* defaultFont = io.Fonts->AddFontDefault(&config);
    if (defaultFont)
    {
        loadedFonts.push_back(defaultFont);
        fontNames.push_back("Default");
        std::cout << "Loaded default font" << std::endl;
    }
    else
    {
        std::cerr << "Failed to load default font" << std::endl;
    }

    // Load your custom fonts
    for (const auto& entry : std::filesystem::directory_iterator("fonts"))
    {
        if (entry.path().extension() == ".ttf")
        {
            std::string fontPath = entry.path().string();
            std::string fontName = entry.path().stem().string();
            
            ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), baseFontSize, &config);
            if (font != nullptr)
            {
                loadedFonts.push_back(font);
                fontNames.push_back(fontName);
                std::cout << "Loaded font: " << fontName << std::endl;
            }
            else
            {
                std::cerr << "Failed to load font: " << fontName << std::endl;
            }
        }
    }

    // Rebuild font atlas
    io.Fonts->Build();

    std::cout << "Total fonts loaded: " << loadedFonts.size() << std::endl;
}

void RenderTextWithStroke(ImDrawList* draw_list, const ImFont* font, float font_size, ImVec2 pos, ImU32 fill_col, ImU32 stroke_col, float stroke_width, const char* text, const char* text_end = NULL)
{
    if (stroke_width > 0)
    {
        for (float x = -stroke_width; x <= stroke_width; x += 1.0f)
        {
            for (float y = -stroke_width; y <= stroke_width; y += 1.0f)
            {
                draw_list->AddText(font, font_size, ImVec2(pos.x + x, pos.y + y), stroke_col, text, text_end);
            }
        }
    }
    draw_list->AddText(font, font_size, pos, fill_col, text, text_end);
}


// Add this function to draw the grid
void DrawGrid(ImDrawList* draw_list, const ImVec2& windowPos, const ImVec2& windowSize)
{
    // Draw background
    draw_list->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), IM_COL32(18, 18, 28, 255));

    const float baseSpacing = 36.0f; // Base spacing between dots
    const float baseSize = 1.0f; // Base size of dots
    const ImU32 dotColor = IM_COL32(179, 179, 204, 255);

    float spacing = baseSpacing * gridScale;
    float size = baseSize * gridScale;

    // Implement a more balanced culling
    const int maxDotsPerDimension = 300; // Increased for more detail
    const float minSpacing = 10.0f; // Minimum spacing between dots

    if (spacing < minSpacing)
    {
        int factor = static_cast<int>(minSpacing / spacing) + 1;
        spacing *= factor;
        size *= std::sqrt(factor); // Increase dot size when spacing increases
    }

    // Ensure a minimum visible size for dots
    const float minVisibleSize = 0.5f;
    size = std::max(size, minVisibleSize);

    ImVec2 offset = ImVec2(
        fmodf(gridOffset.x, spacing),
        fmodf(gridOffset.y, spacing)
    );

    int dotsDrawn = 0;
    for (float x = offset.x - spacing; x < windowSize.x + spacing; x += spacing)
    {
        for (float y = offset.y - spacing; y < windowSize.y + spacing; y += spacing)
        {
            draw_list->AddCircleFilled(
                ImVec2(windowPos.x + x, windowPos.y + y),
                size / 2,
                dotColor
            );

            dotsDrawn++;
            if (dotsDrawn > maxDotsPerDimension * maxDotsPerDimension)
            {
                goto endDrawing; // Exit both loops if we've drawn too many dots
            }
        }
    }

endDrawing:
    // You can add any cleanup or final operations here if needed
}

GLuint CreateTextureFromData(const std::vector<unsigned char>& data, int width, int height)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    return texture;
}

void LoadTextureFromFile(const char* filename, Image& img)
{
    std::cout << "Loading image: " << filename << std::endl;
    int channels;
    unsigned char* image = stbi_load(filename, &img.width, &img.height, &channels, 4);
    if (image == nullptr)
    {
        std::cerr << "Failed to load image" << std::endl;
        return;
    }

    img.data.assign(image, image + img.width * img.height * 4);
    stbi_image_free(image);

    img.texture = CreateTextureFromData(img.data, img.width, img.height);

    std::cout << "Image loaded successfully. Width: " << img.width << ", Height: " << img.height << std::endl;
}

bool IsPointInImage(const Image& img, const ImVec2& point)
{
    float displayWidth = img.isTextImage ? img.originalWidth * img.zoom : img.width * img.zoom;
    float displayHeight = img.isTextImage ? img.originalHeight * img.zoom : img.height * img.zoom;

    ImVec2 topLeft = img.position;
    ImVec2 bottomRight = ImVec2(img.position.x + displayWidth, img.position.y + displayHeight);
    return point.x >= topLeft.x && point.x <= bottomRight.x && point.y >= topLeft.y && point.y <= bottomRight.y;
}

void EraseImagePart(Image& img, const ImVec2& point)
{
    // Calculate the center of the image
    ImVec2 center = ImVec2(img.position.x + img.width * img.zoom * 0.5f, 
                           img.position.y + img.height * img.zoom * 0.5f);

    // Translate point to origin
    ImVec2 translated = ImVec2(point.x - center.x, point.y - center.y);

    // Rotate point back
    float cos_r = cosf(-img.rotation * 3.14159f / 180.0f);
    float sin_r = sinf(-img.rotation * 3.14159f / 180.0f);
    ImVec2 rotated = ImVec2(
        translated.x * cos_r - translated.y * sin_r,
        translated.x * sin_r + translated.y * cos_r
    );

    // Scale back to image coordinates
    int centerX = static_cast<int>((rotated.x / img.zoom) + img.width * 0.5f);
    int centerY = static_cast<int>((rotated.y / img.zoom) + img.height * 0.5f);

    for (int y = -img.eraserSize; y <= img.eraserSize; ++y)
    {
        for (int x = -img.eraserSize; x <= img.eraserSize; ++x)
        {
            if (x*x + y*y <= img.eraserSize*img.eraserSize)
            {
                int pixelX = centerX + x;
                int pixelY = centerY + y;
                
                // Apply mirroring if necessary
                if (img.mirrored)
                {
                    pixelX = img.width - 1 - pixelX;
                }

                if (pixelX >= 0 && pixelX < img.width && pixelY >= 0 && pixelY < img.height)
                {
                    int index = (pixelY * img.width + pixelX) * 4;
                    if (img.isTextImage)
                    {
                        // For text images, we set the pixel to fully transparent
                        img.pixelData[index + 3] = 0;
                    }
                    else
                    {
                        // For regular images, use the existing erasing method
                        img.data[index + 3] = 0; // Set alpha to 0 (transparent)
                    }
                }
            }
        }
    }

    // Update texture
    glBindTexture(GL_TEXTURE_2D, img.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width, img.height, GL_RGBA, GL_UNSIGNED_BYTE, 
                    img.isTextImage ? img.pixelData.data() : img.data.data());
}


Image CreateImageCopy(const Image& original)
{
    Image copy = original;
    copy.position.x += 20;  // Offset the copy slightly
    copy.position.y += 20;
    copy.targetPosition = copy.position;
    copy.uploadOrder = nextUploadOrder++;
    copy.selected = false;  // The new copy is not selected initially

    if (original.isTextImage)
    {
        // For text images, create a new texture from pixelData
        copy.texture = CreateTextureFromData(copy.pixelData, copy.width, copy.height);
    }
    else
    {
        // For regular images, create a new texture from data
        copy.texture = CreateTextureFromData(copy.data, copy.width, copy.height);
    }

    return copy;
}

// Helper function to draw a button and handle clicks
bool DrawButton(ImDrawList* draw_list, float x, float y, float width, float height, const char* label, ImU32 color = IM_COL32(70, 70, 70, 255))
{
    ImVec2 buttonMin(x, y);
    ImVec2 buttonMax(x + width, y + height);
    ImVec2 mousePos = ImGui::GetMousePos();
    bool isHovered = mousePos.x >= buttonMin.x && mousePos.x <= buttonMax.x &&
                     mousePos.y >= buttonMin.y && mousePos.y <= buttonMax.y;

    ImU32 buttonColor = isHovered ? IM_COL32(153, 153, 153, 255) : color;  // Lighter gray when hovered
    draw_list->AddRectFilled(buttonMin, buttonMax, buttonColor, 3.0f);
    draw_list->AddText(ImVec2(x + 5, y + 3), IM_COL32(255, 255, 255, 255), label);

    return isHovered && ImGui::IsMouseClicked(0);
}

void DisplayImage(Image& img, bool& imageClicked)
{
    // Smooth movement
    img.position.x = img.position.x * 0.9f + img.targetPosition.x * 0.1f;
    img.position.y = img.position.y * 0.9f + img.targetPosition.y * 0.1f;

    ImVec2 uv_min = img.mirrored ? ImVec2(1.0f, 0.0f) : ImVec2(0.0f, 0.0f);
    ImVec2 uv_max = img.mirrored ? ImVec2(0.0f, 1.0f) : ImVec2(1.0f, 1.0f);

    // Calculate the actual size to display
    float displayWidth = img.isTextImage ? img.originalWidth * img.zoom : img.width * img.zoom;
    float displayHeight = img.isTextImage ? img.originalHeight * img.zoom : img.height * img.zoom;
    ImVec2 scaled_size = ImVec2(displayWidth, displayHeight);

    // Calculate the center of the image
    ImVec2 center = ImVec2(img.position.x + scaled_size.x * 0.5f, img.position.y + scaled_size.y * 0.5f);

    // Calculate rotated corners
    ImVec2 corners[4] = {
        ImVec2(-scaled_size.x * 0.5f, -scaled_size.y * 0.5f),
        ImVec2(scaled_size.x * 0.5f, -scaled_size.y * 0.5f),
        ImVec2(scaled_size.x * 0.5f, scaled_size.y * 0.5f),
        ImVec2(-scaled_size.x * 0.5f, scaled_size.y * 0.5f)
    };

    float cos_r = cosf(img.rotation * 3.14159f / 180.0f);
    float sin_r = sinf(img.rotation * 3.14159f / 180.0f);
    for (int i = 0; i < 4; ++i)
    {
        ImVec2 rotated = ImVec2(
            corners[i].x * cos_r - corners[i].y * sin_r + center.x,
            corners[i].x * sin_r + corners[i].y * cos_r + center.y
        );
        corners[i] = rotated;
    }

    // Find the top-left and bottom-right corners of the bounding box
    ImVec2 topLeft = corners[0], bottomRight = corners[0];
    for (int i = 1; i < 4; ++i)
    {
        topLeft.x = std::min(topLeft.x, corners[i].x);
        topLeft.y = std::min(topLeft.y, corners[i].y);
        bottomRight.x = std::max(bottomRight.x, corners[i].x);
        bottomRight.y = std::max(bottomRight.y, corners[i].y);
    }

    // Draw the image
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddImageQuad(
        (void*)(intptr_t)img.texture,
        corners[0], corners[1], corners[2], corners[3],
        uv_min, ImVec2(uv_max.x, uv_min.y), uv_max, ImVec2(uv_min.x, uv_max.y)
    );

    // Custom hit-testing and interaction logic
    ImVec2 mousePos = ImGui::GetMousePos();
    bool isHovered = mousePos.x >= topLeft.x && mousePos.x <= bottomRight.x &&
                     mousePos.y >= topLeft.y && mousePos.y <= bottomRight.y;

    bool isInteractingWithZoomControl = false;

    static bool isRotating = false;
    static ImVec2 rotationCenter;
    static float initialAngle;

    // Draw zoom control boxes and handle zooming only if the image is selected
    if (img.selected)
    {
        float boxSize = 10.0f;
        float boxOffset = 5.0f;
        ImU32 boxColor = IM_COL32(200, 200, 200, 255);
        ImU32 boxHoverColor = IM_COL32(255, 255, 255, 255);

        ImVec2 zoomCorners[4] = {
            ImVec2(topLeft.x - boxSize - boxOffset, topLeft.y - boxSize - boxOffset),
            ImVec2(bottomRight.x + boxOffset, topLeft.y - boxSize - boxOffset),
            ImVec2(bottomRight.x + boxOffset, bottomRight.y + boxOffset),
            ImVec2(topLeft.x - boxSize - boxOffset, bottomRight.y + boxOffset)
        };

        for (int i = 0; i < 4; ++i)
        {
            ImVec2 boxMin = zoomCorners[i];
            ImVec2 boxMax = ImVec2(boxMin.x + boxSize, boxMin.y + boxSize);

            bool isHovering = ImGui::IsMouseHoveringRect(boxMin, boxMax);
            ImU32 color = isHovering ? boxHoverColor : boxColor;

            draw_list->AddRectFilled(boxMin, boxMax, color);

            if (isHovering && !img.eraserMode)
            {
                isInteractingWithZoomControl = true;
                if (ImGui::IsMouseClicked(0))
                {
                    img.activeZoomCorner = i;
                    img.zoomStartPos = ImGui::GetMousePos();
                    img.zoomStartValue = img.zoom;
                    imageClicked = true;
                }
            }
        }

        // Handle zooming
        if (img.activeZoomCorner != -1 && ImGui::IsMouseDown(0) && !img.eraserMode)
        {
            ImVec2 dragDelta = ImVec2(ImGui::GetMousePos().x - img.zoomStartPos.x, 
                                      ImGui::GetMousePos().y - img.zoomStartPos.y);
            
            float dragDistance = sqrtf(dragDelta.x * dragDelta.x + dragDelta.y * dragDelta.y);
            float zoomFactor = 1.0f + dragDistance * 0.01f;

            bool shouldZoomOut = false;
            switch (img.activeZoomCorner) {
                case 0: shouldZoomOut = dragDelta.x > 0 || dragDelta.y > 0; break;
                case 1: shouldZoomOut = dragDelta.x < 0 || dragDelta.y > 0; break;
                case 2: shouldZoomOut = dragDelta.x < 0 || dragDelta.y < 0; break;
                case 3: shouldZoomOut = dragDelta.x > 0 || dragDelta.y < 0; break;
            }

            if (shouldZoomOut) zoomFactor = 1.0f / zoomFactor;

            float newZoom = img.zoomStartValue * zoomFactor;
            newZoom = std::max(0.1f, std::min(newZoom, 5.0f));

            ImVec2 zoomCenter = zoomCorners[img.activeZoomCorner];
            ImVec2 centerOffset = ImVec2(zoomCenter.x - img.position.x, zoomCenter.y - img.position.y);

            img.targetPosition.x = zoomCenter.x - centerOffset.x * (newZoom / img.zoom);
            img.targetPosition.y = zoomCenter.y - centerOffset.y * (newZoom / img.zoom);

            img.zoom = newZoom;
        }

        float buttonWidth = 60.0f;
        float buttonHeight = 20.0f;
        float buttonSpacing = 5.0f;
        float buttonsStartX = topLeft.x;
        float buttonsY = topLeft.y - buttonHeight - 5.0f;

        // Helper function to draw a button and handle clicks
        auto DrawButtonConditional = [&](const char* label, ImU32 color, bool enabled) -> bool {
            ImU32 buttonColor = enabled ? color : IM_COL32(100, 100, 100, 255); // Grayed out if disabled
            bool clicked = DrawButton(draw_list, buttonsStartX, buttonsY, buttonWidth, buttonHeight, label, buttonColor);
            buttonsStartX += buttonWidth + buttonSpacing;
            return clicked && enabled;
        };

        // Mirror button - always displays as "Mirror" with a constant color
        if (DrawButtonConditional("Mirror", IM_COL32(70, 70, 70, 255), true)) // Mirror button is always enabled
        {
            img.mirrored = !img.mirrored;
            imageClicked = true;
            std::cout << "Mirror button clicked. Mirrored: " << img.mirrored << std::endl;
        }

        // Eraser button
        if (DrawButtonConditional("Eraser", 
                                  img.eraserMode ? IM_COL32(180, 190, 254, 255) : IM_COL32(70, 70, 70, 255), 
                                  true)) // Eraser button is always enabled
        {
            img.eraserMode = !img.eraserMode;
            imageClicked = true;
            std::cout << "Eraser button clicked. Eraser mode: " << img.eraserMode << std::endl;
        }

        // Eraser size slider (only visible when eraser mode is active)
        if (img.eraserMode)
        {
            float sliderWidth = 100.0f;
            ImGui::SetCursorScreenPos(ImVec2(buttonsStartX, buttonsY));
            ImGui::PushItemWidth(sliderWidth);
            if (ImGui::SliderInt("##EraserSize", &img.eraserSize, 1, 50, "Size: %d"))
            {
                imageClicked = true;
            }
            ImGui::PopItemWidth();
            buttonsStartX += sliderWidth + buttonSpacing;
        }

        // Copy button
        if (DrawButtonConditional("Copy", IM_COL32(70, 70, 70, 255), !img.eraserMode))
        {
            Image copy = CreateImageCopy(img);
            images.push_back(copy);
            imageClicked = true;
        }

        // Delete button
        if (DrawButtonConditional("Delete", IM_COL32(70, 70, 70, 255), !img.eraserMode))
        {
            img.open = false;
            imageClicked = true;
        }

        // Move to Back button
        if (DrawButtonConditional("To Back", IM_COL32(70, 70, 70, 255), !img.eraserMode))
        {
            int lowestOrder = std::numeric_limits<int>::max();
            for (const auto& otherImg : images)
            {
                if (otherImg.uploadOrder < lowestOrder)
                {
                    lowestOrder = otherImg.uploadOrder;
                }
            }
            img.uploadOrder = lowestOrder - 1;
            imageClicked = true;
        }

        // Rotate button
        if (DrawButtonConditional("Rotate", 
                                  isRotating ? IM_COL32(180, 190, 254, 255) : IM_COL32(70, 70, 70, 255), 
                                  !img.eraserMode))
        {
            if (!isRotating)
            {
                isRotating = true;
                rotationCenter = center;
                initialAngle = atan2f(ImGui::GetMousePos().y - rotationCenter.y, ImGui::GetMousePos().x - rotationCenter.x);
            }
            else
            {
                isRotating = false;
            }
            imageClicked = true;
        }

        // Handle rotation
        if (isRotating && ImGui::IsMouseDown(0) && !img.eraserMode)
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            float currentAngle = atan2f(mousePos.y - center.y, mousePos.x - center.x);
            float angleDiff = currentAngle - initialAngle;
            img.rotation += angleDiff * (180.0f / 3.14159f);
            initialAngle = currentAngle;

            // Normalize rotation to 0-360 degrees
            while (img.rotation < 0.0f) img.rotation += 360.0f;
            while (img.rotation >= 360.0f) img.rotation -= 360.0f;
        }

        // Draw selection box around the selected image
        draw_list->AddRect(
            topLeft,
            bottomRight,
            IM_COL32(180, 190, 254, 255), 0.0f, 15, 2.0f
        );
    }

    // Handle eraser mode
    if (img.selected && img.eraserMode && isHovered && ImGui::IsMouseDown(0))
    {
        EraseImagePart(img, mousePos);
        imageClicked = true;
    }

    // Draw eraser cursor
    if (img.selected && img.eraserMode && isHovered)
    {
        float radius = img.eraserSize * img.zoom / 2.0f;
        draw_list->AddCircle(mousePos, radius, IM_COL32(255, 255, 255, 200), 0, 2.0f);
    }

    // Reset active zoom corner if mouse is released
    if (ImGui::IsMouseReleased(0))
    {
        img.activeZoomCorner = -1;
        isRotating = false;
    }

    // Reset hover state
    img.isHoveringZoomControl = isInteractingWithZoomControl;
}

void HandleTextInterface(ImVec2 windowSize, bool& textClicked)
{
    static Text* draggedText = nullptr;
    static Text* selectedText = nullptr;
    static ImVec2 dragStartPos;
    static int activeZoomCorner = -1;
    static float zoomStartValue = 1.0f;
    static ImVec2 zoomStartPos;
    static bool colorPickerOpen = false;
    static ImGuiID colorPickerID = 0;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    bool clickedOnAnyText = false;

    if (!colorPickerOpen && !isAddTextPopupOpen)
    {
        for (auto& text : texts)
        {
            ImVec2 screenPos = ImVec2(
                text.position.x * gridScale + gridOffset.x,
                text.position.y * gridScale + gridOffset.y
            );

            ImFont* font = loadedFonts[text.fontIndex];
            float scaledSize = text.size * gridScale;

            ImVec2 textSize = font->CalcTextSizeA(scaledSize, FLT_MAX, 0.0f, text.content.c_str());
            
            float padding = scaledSize * 0.25f;

            ImVec2 boxMin = ImVec2(screenPos.x - padding, screenPos.y - padding);
            ImVec2 boxMax = ImVec2(screenPos.x + textSize.x + padding, screenPos.y + textSize.y + padding);

            ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(text.fillColor);
            ImU32 strokeColor = ImGui::ColorConvertFloat4ToU32(text.strokeColor);
            
            if (text.strokeWidth > 0)
            {
                for (float x = -text.strokeWidth; x <= text.strokeWidth; x += 0.5f)
                {
                    for (float y = -text.strokeWidth; y <= text.strokeWidth; y += 0.5f)
                    {
                        draw_list->AddText(font, scaledSize, ImVec2(screenPos.x + x, screenPos.y + y), strokeColor, text.content.c_str());
                    }
                }
            }
            draw_list->AddText(font, scaledSize, screenPos, fillColor, text.content.c_str());

            bool isHovered = ImGui::IsMouseHoveringRect(boxMin, boxMax);
            if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                selectedText = &text;
                draggedText = &text;
                dragStartPos = ImGui::GetMousePos();
                clickedOnAnyText = true;
                textClicked = true;
            }

            if (&text == selectedText)
            {
                draw_list->AddRect(boxMin, boxMax, IM_COL32(180, 190, 254, 255), 0.0f, 0, 2.0f);

                float zoomBoxSize = 10.0f;
                float zoomBoxOffset = 5.0f;
                ImU32 zoomBoxColor = IM_COL32(200, 200, 200, 255);
                ImU32 zoomBoxHoverColor = IM_COL32(255, 255, 255, 255);

                ImVec2 zoomCorners[4] = {
                    ImVec2(boxMin.x - zoomBoxSize - zoomBoxOffset, boxMin.y - zoomBoxSize - zoomBoxOffset),
                    ImVec2(boxMax.x + zoomBoxOffset, boxMin.y - zoomBoxSize - zoomBoxOffset),
                    ImVec2(boxMax.x + zoomBoxOffset, boxMax.y + zoomBoxOffset),
                    ImVec2(boxMin.x - zoomBoxSize - zoomBoxOffset, boxMax.y + zoomBoxOffset)
                };

                for (int i = 0; i < 4; ++i)
                {
                    ImVec2 cornerMin = zoomCorners[i];
                    ImVec2 cornerMax = ImVec2(cornerMin.x + zoomBoxSize, cornerMin.y + zoomBoxSize);

                    bool isZoomHovering = ImGui::IsMouseHoveringRect(cornerMin, cornerMax);
                    ImU32 color = isZoomHovering ? zoomBoxHoverColor : zoomBoxColor;

                    draw_list->AddRectFilled(cornerMin, cornerMax, color);

                    if (isZoomHovering && ImGui::IsMouseClicked(0))
                    {
                        activeZoomCorner = i;
                        zoomStartPos = ImGui::GetMousePos();
                        zoomStartValue = text.size;
                        textClicked = true;
                        clickedOnAnyText = true;
                    }
                }

                if (activeZoomCorner != -1 && ImGui::IsMouseDown(0))
                {
                    ImVec2 dragDelta = ImVec2(ImGui::GetMousePos().x - zoomStartPos.x, 
                                              ImGui::GetMousePos().y - zoomStartPos.y);
                    
                    float dragDistance = sqrtf(dragDelta.x * dragDelta.x + dragDelta.y * dragDelta.y);
                    float zoomFactor = 1.0f + dragDistance * 0.005f;

                    bool shouldZoomOut = false;
                    switch (activeZoomCorner) {
                        case 0: shouldZoomOut = dragDelta.x > 0 || dragDelta.y > 0; break;
                        case 1: shouldZoomOut = dragDelta.x < 0 || dragDelta.y > 0; break;
                        case 2: shouldZoomOut = dragDelta.x < 0 || dragDelta.y < 0; break;
                        case 3: shouldZoomOut = dragDelta.x > 0 || dragDelta.y < 0; break;
                    }

                    if (shouldZoomOut) zoomFactor = 1.0f / zoomFactor;

                    float targetSize = zoomStartValue * zoomFactor;
                    text.size = text.size * 0.9f + targetSize * 0.1f;
                    text.size = std::max(5.0f, std::min(text.size, 1000.0f));
                    textClicked = true;
                    clickedOnAnyText = true;
                }
            }

            if (&text == draggedText && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                text.position.x += dragDelta.x / gridScale;
                text.position.y += dragDelta.y / gridScale;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                textClicked = true;
                clickedOnAnyText = true;
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clickedOnAnyText)
        {
            selectedText = nullptr;
            draggedText = nullptr;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            draggedText = nullptr;
            activeZoomCorner = -1;
        }
    }

    ImVec2 buttonPos(10, windowSize.y - 40);
    ImGui::SetCursorPos(buttonPos);
    if (ImGui::Button("Add Text"))
    {
        ImGui::OpenPopup("Add Text");
        isAddTextPopupOpen = true;
    }

    ImGui::SetNextWindowPos(ImVec2(buttonPos.x, buttonPos.y - 420), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_Always);

    if (ImGui::BeginPopup("Add Text"))
    {
        isAddTextPopupOpen = true;

        static char textBuffer[256] = "";
        static ImVec4 fillColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        static ImVec4 strokeColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        static float strokeWidth = 0.0f;
        static int selectedFontSize = 1;
        static const char* fontSizes[] = { "Small Font", "Medium Font", "Large Font" };
        static const float fontSizeValues[] = { 24.0f, 48.0f, 72.0f };
        static int selectedFont = 0;

        ImGui::PushItemWidth(150);

        if (ImGui::BeginCombo("Font", fontNames[selectedFont].c_str()))
        {
            for (int i = 0; i < fontNames.size(); i++)
            {
                bool isSelected = (selectedFont == i);
                if (ImGui::Selectable(fontNames[i].c_str(), isSelected))
                {
                    selectedFont = i;
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Combo("Size", &selectedFontSize, fontSizes, IM_ARRAYSIZE(fontSizes));
        ImGui::ColorEdit3("Fill Color", (float*)&fillColor);
        ImGui::ColorEdit3("Stroke Color", (float*)&strokeColor);
        ImGui::SliderFloat("Stroke Width", &strokeWidth, 0.0f, 5.0f);

        ImGui::PopItemWidth();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
        ImGui::InputTextMultiline("##Text", textBuffer, IM_ARRAYSIZE(textBuffer), ImVec2(330, 60), ImGuiInputTextFlags_AllowTabInput);
        ImGui::PopStyleColor();

        ImFont* previewFont = loadedFonts[selectedFont];
        float previewFontSize = fontSizeValues[selectedFontSize];

        ImVec2 textSize = previewFont->CalcTextSizeA(previewFontSize, FLT_MAX, 0.0f, textBuffer);
        ImVec2 previewSize = ImVec2(std::max(textSize.x + 10.0f, 330.0f), std::max(textSize.y + 10.0f, 100.0f));

        ImVec2 pos = ImGui::GetCursorScreenPos();
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 fillColorU32 = ImGui::ColorConvertFloat4ToU32(fillColor);
        ImU32 strokeColorU32 = ImGui::ColorConvertFloat4ToU32(strokeColor);

        ImVec2 previewBoxMin = ImVec2(pos.x, pos.y);
        ImVec2 previewBoxMax = ImVec2(pos.x + previewSize.x, pos.y + previewSize.y);
        drawList->AddRectFilled(previewBoxMin, previewBoxMax, IM_COL32(50, 50, 50, 255));

        ImVec2 textPos = ImVec2(pos.x + 5, pos.y + 5);
        if (strokeWidth > 0)
        {
            for (float x = -strokeWidth; x <= strokeWidth; x += 0.5f)
            {
                for (float y = -strokeWidth; y <= strokeWidth; y += 0.5f)
                {
                    drawList->AddText(previewFont, previewFontSize, ImVec2(textPos.x + x, textPos.y + y), strokeColorU32, textBuffer);
                }
            }
        }
        drawList->AddText(previewFont, previewFontSize, textPos, fillColorU32, textBuffer);

        ImGui::Dummy(previewSize);

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);

        if (ImGui::Button("Add"))
        {
            if (strlen(textBuffer) > 0)
            {
                // Calculate the center of the current view in screen coordinates
                ImVec2 screenCenter = ImVec2(windowSize.x * 0.5f, windowSize.y * 0.5f);

                // Convert screen coordinates to world coordinates
                ImVec2 worldPos = ImVec2(
                    (screenCenter.x - gridOffset.x) / gridScale,
                    (screenCenter.y - gridOffset.y) / gridScale
                );

                // Render text to texture
                auto [textureID, pixelData] = RenderTextToTexture(textBuffer, previewFont, previewFontSize, fillColor, strokeColor, strokeWidth);

                // Calculate the size of the text
                ImVec2 textSize = previewFont->CalcTextSizeA(previewFontSize, FLT_MAX, 0.0f, textBuffer);

                // Add the texture as an image to your images collection
                Image newImage;
                newImage.texture = textureID;
                newImage.width = (int)(textSize.x + strokeWidth * 2 + 10);
                newImage.height = (int)(textSize.y + strokeWidth * 2 + 10);
                newImage.position = worldPos;
                newImage.targetPosition = worldPos;
                newImage.zoom = 1.0f;
                newImage.rotation = 0.0f;
                newImage.name = "Text Image";
                newImage.open = true;
                newImage.selected = false;
                newImage.mirrored = false;
                newImage.uploadOrder = nextUploadOrder++;
                newImage.pixelData = pixelData;
                newImage.isTextImage = true;
                newImage.eraserMode = false;
                newImage.eraserSize = 5;
                newImage.isHoveringZoomControl = false;
                newImage.activeZoomCorner = -1;
                
                // Store the original size of the text image
                newImage.originalWidth = newImage.width;
                newImage.originalHeight = newImage.height;
                
                // Center the text by adjusting its position
                newImage.position.x -= newImage.width * 0.5f / gridScale;
                newImage.position.y -= newImage.height * 0.5f / gridScale;
                newImage.targetPosition = newImage.position;

                // Save current state for undo
                undoStates.push_back({images, nextUploadOrder});
                redoStates.clear();

                images.push_back(newImage);

                ImGui::CloseCurrentPopup();
                isAddTextPopupOpen = false;
                textClicked = true;

                std::cout << "Added new text image. Position: (" << newImage.position.x << ", " << newImage.position.y 
                        << "), Size: " << newImage.width << "x" << newImage.height 
                        << ", Original Size: " << newImage.originalWidth << "x" << newImage.originalHeight << std::endl;
            }
        }
        ImGui::EndPopup();
    }
    else
    {
        isAddTextPopupOpen = false;
    }

    if (selectedText && ImGui::IsKeyPressed(ImGuiKey_Delete) && !colorPickerOpen && !isAddTextPopupOpen)
    {
        texts.erase(std::remove_if(texts.begin(), texts.end(),
            [&](const Text& t) { return &t == selectedText; }),
            texts.end());
        selectedText = nullptr;
        textClicked = true;
    }

    if (colorPickerOpen || isAddTextPopupOpen)
    {
        ImGui::GetIO().WantCaptureMouse = true;
    }
}

void ShowImageViewer(bool* p_open)
{
    static Image* selectedImage = nullptr;
    static Image* draggedImage = nullptr;
    static ImVec2 dragStartPos;
    static bool isGrabbingGrid = false;
    static ImVec2 gridGrabStartPos;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Image Viewer", p_open, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetIO().DisplaySize;

    // Draw the grid for the entire window
    DrawGrid(draw_list, windowPos, windowSize);

    ImGui::Text("Welcome to the Advanced Image Viewer!");

    if (ImGui::Button("Load Image"))
    {
        std::cout << "Load Image button clicked" << std::endl;
        const char* filters[] = { "*.png", "*.jpg", "*.jpeg", "*.bmp" };
        const char* file = tinyfd_openFileDialog(
            "Open Image",
            "",
            4,
            filters,
            "Image Files",
            0
        );
        if (file)
        {
            std::cout << "File selected: " << file << std::endl;
            Image img;
            LoadTextureFromFile(file, img);
            if (img.texture)
            {
                // Save state for undo
                undoStates.push_back({images, nextUploadOrder});
                redoStates.clear();

                img.zoom = 1.0f;
                img.position = img.targetPosition = ImVec2(50, 50);
                img.name = file;
                img.open = true;
                img.selected = false;
                img.mirrored = false;
                img.uploadOrder = nextUploadOrder++;
                img.eraserMode = false;
                img.eraserSize = 5;
                img.rotation = 0.0f;
                img.targetRotation = 0.0f;
                img.isHoveringZoomControl = false;
                img.activeZoomCorner = -1;
                images.push_back(img);
                std::cout << "Image added to the viewer" << std::endl;
            }
            else
            {
                std::cerr << "Failed to load image texture" << std::endl;
            }
        }
        else
        {
            std::cerr << "No file selected or dialog cancelled" << std::endl;
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        // Save current state for undo
        undoStates.push_back({images, nextUploadOrder});
        redoStates.clear();

        std::cout << "Clear All button clicked" << std::endl;
        for (const auto& img : images)
        {
            glDeleteTextures(1, &img.texture);
        }
        images.clear();
        texts.clear();  // Clear texts as well
        nextUploadOrder = 0;
        selectedImage = nullptr;
        draggedImage = nullptr;
    }

    ImGui::SameLine();
    if (ImGui::Button("Undo") && !undoStates.empty())
    {
        // Save current state for redo
        redoStates.push_back({images, nextUploadOrder});

        // Restore previous state
        ImageState prevState = undoStates.back();
        undoStates.pop_back();

        // Clear current images
        for (const auto& img : images)
        {
            glDeleteTextures(1, &img.texture);
        }

        // Restore images and nextUploadOrder
        images = prevState.images;
        nextUploadOrder = prevState.nextUploadOrder;

        // Recreate textures for restored images
        for (auto& img : images)
        {
            img.texture = CreateTextureFromData(img.data, img.width, img.height);
        }

        selectedImage = nullptr;
        draggedImage = nullptr;
    }

    ImGui::SameLine();
    if (ImGui::Button("Redo") && !redoStates.empty())
    {
        // Save current state for undo
        undoStates.push_back({images, nextUploadOrder});

        // Restore next state
        ImageState nextState = redoStates.back();
        redoStates.pop_back();

        // Clear current images
        for (const auto& img : images)
        {
            glDeleteTextures(1, &img.texture);
        }

        // Restore images and nextUploadOrder
        images = nextState.images;
        nextUploadOrder = nextState.nextUploadOrder;

        // Recreate textures for restored images
        for (auto& img : images)
        {
            img.texture = CreateTextureFromData(img.data, img.width, img.height);
        }

        selectedImage = nullptr;
        draggedImage = nullptr;
    }

    ImGui::SameLine();
    ImGui::Checkbox("Show Metrics", &show_metrics);

    ImGui::BeginChild("ImageDisplayArea", ImVec2(0, -30), false, ImGuiWindowFlags_HorizontalScrollbar);
    
    // Sort images based on upload order (ascending)
    std::sort(images.begin(), images.end(), [](const Image& a, const Image& b) {
        return a.uploadOrder < b.uploadOrder;
    });

    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 relativeMousePos = ImVec2(mousePos.x - windowPos.x, mousePos.y - windowPos.y);

    Image* hoveredImage = nullptr;
    bool imageClicked = false;

    // Display all images and find the topmost hovered image
    for (auto& img : images)
    {
        if (img.open)
        {
            DisplayImage(img, imageClicked);

            if (IsPointInImage(img, relativeMousePos))
            {
                hoveredImage = &img;
            }
        }
    }

    bool textClicked = false;
    HandleTextInterface(ImGui::GetWindowSize(), textClicked);

    // Handle selection and start of dragging
    if (!isAddTextPopupOpen)
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hoveredImage && !textClicked)
            {
                // Deselect the previously selected image
                if (selectedImage && selectedImage != hoveredImage)
                {
                    selectedImage->selected = false;
                    selectedImage->eraserMode = false;  // Turn off eraser mode when deselecting
                }

                // Select the new image
                selectedImage = hoveredImage;
                selectedImage->selected = true;

                // Only start dragging if not in eraser mode
                if (!selectedImage->eraserMode)
                {
                    draggedImage = selectedImage;
                    dragStartPos = ImGui::GetMousePos();
                }
                else
                {
                    draggedImage = nullptr;
                }

                // Ensure all other images are deselected
                for (auto& img : images)
                {
                    if (&img != selectedImage)
                    {
                        img.selected = false;
                        img.eraserMode = false;
                    }
                }

                std::cout << "Selected image. Mirrored: " << selectedImage->mirrored 
                          << ", Eraser mode: " << selectedImage->eraserMode << std::endl;
            }
            else if (!imageClicked && !textClicked)
            {
                // Clicking on empty space
                if (selectedImage)
                {
                    selectedImage->selected = false;
                    selectedImage->eraserMode = false;
                }
                selectedImage = nullptr;
                draggedImage = nullptr;

                // Ensure all images are deselected
                for (auto& img : images)
                {
                    img.selected = false;
                    img.eraserMode = false;
                }

                // Start grabbing the grid
                isGrabbingGrid = true;
                gridGrabStartPos = ImGui::GetMousePos();
            }
        }

        // Handle dragging
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (draggedImage && !draggedImage->eraserMode)
            {
                // Move only the dragged image if not in eraser mode
                draggedImage->position.x += dragDelta.x;
                draggedImage->position.y += dragDelta.y;
                draggedImage->targetPosition = draggedImage->position;
            }
            else if (isGrabbingGrid)
            {
                // Move the grid and all images
                ImVec2 oldGridOffset = gridOffset;
                gridOffset.x += dragDelta.x;
                gridOffset.y += dragDelta.y;
                
                ImVec2 gridMovement = ImVec2(gridOffset.x - oldGridOffset.x, gridOffset.y - oldGridOffset.y);
                for (auto& img : images)
                {
                    if (img.open)
                    {
                        img.position.x += gridMovement.x;
                        img.position.y += gridMovement.y;
                        img.targetPosition = img.position;
                    }
                }
                // Move texts with the grid
                for (auto& text : texts)
                {
                    text.position.x += gridMovement.x / gridScale;
                    text.position.y += gridMovement.y / gridScale;
                }
            }
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }

        // Reset dragged image and grid grabbing when mouse is released
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            draggedImage = nullptr;
            isGrabbingGrid = false;
        }

        // Update grid scale based on zoom operations
        float mouseWheel = ImGui::GetIO().MouseWheel;
        if (mouseWheel != 0.0f)
        {
            float zoomFactor = 1.0f + mouseWheel * 0.1f;
            float oldGridScale = gridScale;
            gridScale *= zoomFactor;
            gridScale = std::max(0.1f, gridScale); // Prevent scale from going negative or zero

            // Adjust gridOffset to zoom towards mouse position
            ImVec2 mouseGridPos = ImVec2(
                (mousePos.x - windowPos.x - gridOffset.x) / oldGridScale,
                (mousePos.y - windowPos.y - gridOffset.y) / oldGridScale
            );
            gridOffset.x = mousePos.x - windowPos.x - mouseGridPos.x * gridScale;
            gridOffset.y = mousePos.y - windowPos.y - mouseGridPos.y * gridScale;

            // Adjust image positions and sizes based on zoom
            for (auto& img : images)
            {
                ImVec2 imgGridPos = ImVec2(
                    (img.position.x - gridOffset.x) / oldGridScale,
                    (img.position.y - gridOffset.y) / oldGridScale
                );
                img.position.x = gridOffset.x + imgGridPos.x * gridScale;
                img.position.y = gridOffset.y + imgGridPos.y * gridScale;
                img.targetPosition = img.position;
                img.zoom *= zoomFactor;
            }

            // Adjust text positions and sizes based on zoom
            for (auto& text : texts)
            {
                ImVec2 textGridPos = ImVec2(
                    (text.position.x - gridOffset.x) / oldGridScale,
                    (text.position.y - gridOffset.y) / oldGridScale
                );
                text.position.x = gridOffset.x + textGridPos.x * gridScale;
                text.position.y = gridOffset.y + textGridPos.y * gridScale;
                text.size *= zoomFactor; // Adjust text size based on zoom
            }
        }
    }

    images.erase(std::remove_if(images.begin(), images.end(),
        [&](const Image& img) { 
            if (!img.open) {
                if (&img == selectedImage) {
                    selectedImage = nullptr;
                }
                if (&img == draggedImage) {
                    draggedImage = nullptr;
                }
                glDeleteTextures(1, &img.texture);
                return true;
            }
            return false;
        }), images.end());

    ImGui::EndChild();

    ImGui::End();

    if (show_metrics)
    {
        ImGui::ShowMetricsWindow(&show_metrics);
    }
}

int main(int, char**)
{
    // Initialize GLFW
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    // Configure GLFW
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

    // Enable MSAA
    glfwWindowHint(GLFW_SAMPLES, 4);

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Advanced Image Viewer", NULL, NULL);
    if (window == NULL)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Enable MSAA in OpenGL
    glEnable(GL_MULTISAMPLE);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Enable keyboard navigation and other quality improvements
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontAllowUserScaling = true;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 120");

    // Load fonts
    LoadFonts();

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Show the main application window
        bool show_viewer = true;
        ShowImageViewer(&show_viewer);

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
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



