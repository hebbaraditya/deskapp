#pragma once

#include "imgui.h"
#include "canvas_objects.h"
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// TransformHandles
//
// Draws selection box + 4 corner scale handles + 1 rotate handle around any
// rectangular object. Works identically for CanvasImage and TextObject because
// both use Transform + DragState.
//
// Usage (each frame, after drawing the object):
//
//   static DragState drag;
//   bool consumed = TransformHandles::update(
//       draw_list, obj.transform, obj.cached_size,
//       drag, zoom, canvas_min, canvas_size);
//
// Returns true if the handle consumed the mouse this frame (caller should skip
// its own hit-test / drag logic).
// ─────────────────────────────────────────────────────────────────────────────

namespace TransformHandles {

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr float kHandleRadius   = 6.f;   // corner square half-size (screen px)
static constexpr float kRotateOffset   = 24.f;  // distance above top edge (screen px)
static constexpr float kRotateRadius   = 7.f;   // rotate handle circle radius

static constexpr ImU32 kColorHandle    = IM_COL32(255, 255, 255, 230);
static constexpr ImU32 kColorHandleHov = IM_COL32(100, 180, 255, 255);
static constexpr ImU32 kColorBorder    = IM_COL32(100, 150, 255, 200);
static constexpr ImU32 kColorRotate    = IM_COL32(255, 200,  80, 230);
static constexpr ImU32 kColorRotateHov = IM_COL32(255, 240, 120, 255);

// ── Internal helpers ──────────────────────────────────────────────────────────

// World → screen
static inline ImVec2 worldToScreen(ImVec2 world_pos, ImVec2 canvas_min,
                                    ImVec2 canvas_size, ImVec2 pan, float zoom)
{
    return {
        canvas_min.x + canvas_size.x * 0.5f + (world_pos.x + pan.x) * zoom,
        canvas_min.y + canvas_size.y * 0.5f + (world_pos.y + pan.y) * zoom,
    };
}

// Screen → world
static inline ImVec2 screenToWorld(ImVec2 screen_pos, ImVec2 canvas_min,
                                    ImVec2 canvas_size, ImVec2 pan, float zoom)
{
    return {
        (screen_pos.x - canvas_min.x - canvas_size.x * 0.5f) / zoom - pan.x,
        (screen_pos.y - canvas_min.y - canvas_size.y * 0.5f) / zoom - pan.y,
    };
}

static inline float dist(ImVec2 a, ImVec2 b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx*dx + dy*dy);
}

static inline bool circleHit(ImVec2 mouse, ImVec2 center, float r) {
    return dist(mouse, center) <= r;
}

static inline bool rectHit(ImVec2 mouse, ImVec2 center, float half) {
    return fabsf(mouse.x - center.x) <= half && fabsf(mouse.y - center.y) <= half;
}

// ── Corner positions (screen space) ──────────────────────────────────────────
// Returns the 4 corners [TL, TR, BR, BL] + rotate handle in screen space
// given the object's world-space rect.
static void getHandlePositions(const Transform& t, ImVec2 world_size,
                                ImVec2 canvas_min, ImVec2 canvas_size,
                                ImVec2 pan, float zoom,
                                ImVec2 out_corners[4], ImVec2& out_rotate)
{
    // Effective world-space display size
    ImVec2 ws = { world_size.x * t.scale, world_size.y * t.scale };

    // 4 corners in world space
    ImVec2 wc[4] = {
        { t.position.x,          t.position.y          }, // TL
        { t.position.x + ws.x,   t.position.y          }, // TR
        { t.position.x + ws.x,   t.position.y + ws.y   }, // BR
        { t.position.x,          t.position.y + ws.y   }, // BL
    };

    for (int i = 0; i < 4; ++i)
        out_corners[i] = worldToScreen(wc[i], canvas_min, canvas_size, pan, zoom);

    // Rotate handle: midpoint of top edge, offset upward in screen space
    ImVec2 top_mid = {
        (out_corners[0].x + out_corners[1].x) * 0.5f,
        (out_corners[0].y + out_corners[1].y) * 0.5f - kRotateOffset
    };
    out_rotate = top_mid;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main update function — call once per selected object per frame
//
// Parameters:
//   draw_list   — ImDrawList to draw handles into
//   t           — Transform to read/modify
//   world_size  — unscaled size of the object (pixels for image, text block size)
//   drag        — persistent drag state (one per object)
//   zoom        — canvas zoom level
//   pan         — canvas pan offset (world units)
//   canvas_min  — top-left of canvas in screen space
//   canvas_size — size of canvas in screen space
//
// Returns true if mouse interaction was consumed by a handle this frame.
// ─────────────────────────────────────────────────────────────────────────────
static bool update(ImDrawList* draw_list,
                   Transform& t,
                   ImVec2 world_size,
                   DragState& drag,
                   float zoom,
                   ImVec2 pan,
                   ImVec2 canvas_min,
                   ImVec2 canvas_size)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool consumed = false;

    ImVec2 corners[4];
    ImVec2 rot_handle;
    getHandlePositions(t, world_size, canvas_min, canvas_size, pan, zoom,
                       corners, rot_handle);

    // ── Draw selection border ─────────────────────────────────────────────────
    draw_list->AddRect(corners[0], corners[2], kColorBorder, 0.f, 0, 1.5f);

    // ── Draw line from top edge to rotate handle ──────────────────────────────
    ImVec2 top_mid = {
        (corners[0].x + corners[1].x) * 0.5f,
        (corners[0].y + corners[1].y) * 0.5f
    };
    draw_list->AddLine(top_mid, rot_handle, IM_COL32(180, 180, 180, 160), 1.f);

    // ── Hit test all handles ──────────────────────────────────────────────────
    HandleId hovered = HandleId::None;

    // Rotate handle first (highest priority — sits outside the rect)
    if (circleHit(mouse, rot_handle, kRotateRadius + 3.f))
        hovered = HandleId::Rotate;

    // Corner handles
    if (hovered == HandleId::None) {
        for (int i = 0; i < 4; ++i) {
            if (rectHit(mouse, corners[i], kHandleRadius + 3.f)) {
                hovered = static_cast<HandleId>(i);
                break;
            }
        }
    }

    // ── Start drag ────────────────────────────────────────────────────────────
    if (ImGui::IsMouseClicked(0) && hovered != HandleId::None) {
        drag.active      = hovered;
        drag.start_mouse = mouse;
        drag.start_scale = t.scale;
        drag.start_rot   = t.rotation;
        drag.start_pos   = t.position;
        drag.start_size  = { world_size.x * t.scale, world_size.y * t.scale };
        consumed = true;
    }

    // ── Process active drag ───────────────────────────────────────────────────
    if (drag.active != HandleId::None && ImGui::IsMouseDown(0)) {
        consumed = true;

        if (drag.active == HandleId::Rotate) {
            // ── Rotate: angle from object centre to mouse ─────────────────────
            ImVec2 centre_screen = {
                (corners[0].x + corners[2].x) * 0.5f,
                (corners[0].y + corners[2].y) * 0.5f
            };
            float angle_now   = atan2f(mouse.y   - centre_screen.y,
                                       mouse.x   - centre_screen.x);
            float angle_start = atan2f(drag.start_mouse.y - centre_screen.y,
                                       drag.start_mouse.x - centre_screen.x);
            float delta_deg = (angle_now - angle_start) * (180.f / 3.14159265f);
            t.rotation = drag.start_rot + delta_deg;

        } else {
            // ── Scale from opposite corner ────────────────────────────────────
            //
            // Strategy: the corner OPPOSITE to the dragged handle stays fixed.
            // We compute how far the dragged corner has moved and derive a new
            // uniform scale from the ratio of new size / original size.

            // Opposite corner index
            int opp = ((int)drag.active + 2) % 4;

            // Fixed anchor in world space (the opposite corner)
            ImVec2 anchor_screen = corners[opp];
            ImVec2 anchor_world  = screenToWorld(anchor_screen,
                                                  canvas_min, canvas_size, pan, zoom);

            // Current mouse in world space
            ImVec2 mouse_world = screenToWorld(mouse, canvas_min, canvas_size, pan, zoom);

            // Vector from anchor to current mouse
            ImVec2 vec = { mouse_world.x - anchor_world.x,
                           mouse_world.y - anchor_world.y };

            // Expected size vector based on which corner is being dragged
            // TL(0): vec should be (-w, -h), TR(1): (+w, -h), BR(2): (+w, +h), BL(3): (-w, +h)
            float raw_w = fabsf(vec.x);
            float raw_h = fabsf(vec.y);
            float original_w = world_size.x;
            float original_h = world_size.y;

            if (original_w >= 1.f && original_h >= 1.f) {
                float sx = raw_w / original_w;
                float sy = raw_h / original_h;
                float new_scale = (raw_w + raw_h > 0.f) ? (sx + sy) * 0.5f : drag.start_scale;
                new_scale = std::max(0.05f, std::min(20.f, new_scale));
                t.scale = new_scale;

                float nw = original_w * new_scale;
                float nh = original_h * new_scale;

                //  Dragged | Fixed(anchor) | New position
                //  TL(0)   | BR(2)         | {anchor.x - nw, anchor.y - nh}
                //  TR(1)   | BL(3)         | {anchor.x,      anchor.y - nh}
                //  BR(2)   | TL(0)         | {anchor.x,      anchor.y     }
                //  BL(3)   | TR(1)         | {anchor.x - nw, anchor.y     }
                switch (drag.active) {
                    case HandleId::TopLeft:
                        t.position = { anchor_world.x - nw, anchor_world.y - nh }; break;
                    case HandleId::TopRight:
                        t.position = { anchor_world.x,      anchor_world.y - nh }; break;
                    case HandleId::BottomRight:
                        t.position = { anchor_world.x,      anchor_world.y      }; break;
                    case HandleId::BottomLeft:
                        t.position = { anchor_world.x - nw, anchor_world.y      }; break;
                    default: break;
                }
            }
        }
    }

    // ── Release drag ──────────────────────────────────────────────────────────
    if (ImGui::IsMouseReleased(0) && drag.active != HandleId::None) {
        drag.active = HandleId::None;
        consumed = true;
    }

    // ── Draw handles ──────────────────────────────────────────────────────────
    // Corner handles
    for (int i = 0; i < 4; ++i) {
        HandleId hid = static_cast<HandleId>(i);
        bool is_hov  = (hovered == hid) || (drag.active == hid);
        ImU32 col    = is_hov ? kColorHandleHov : kColorHandle;
        float r      = kHandleRadius;

        draw_list->AddRectFilled(
            { corners[i].x - r, corners[i].y - r },
            { corners[i].x + r, corners[i].y + r },
            col, 2.f);
        draw_list->AddRect(
            { corners[i].x - r, corners[i].y - r },
            { corners[i].x + r, corners[i].y + r },
            IM_COL32(60, 60, 60, 200), 2.f, 0, 1.f);
    }

    // Rotate handle
    {
        bool is_hov = (hovered == HandleId::Rotate) || (drag.active == HandleId::Rotate);
        ImU32 col   = is_hov ? kColorRotateHov : kColorRotate;
        draw_list->AddCircleFilled(rot_handle, kRotateRadius, col);
        draw_list->AddCircle(rot_handle, kRotateRadius, IM_COL32(60, 60, 60, 200), 16, 1.f);
    }

    // Mirror indicator (small arrow icon drawn inside left edge)
    if (t.mirrored) {
        ImVec2 mid_left = {
            corners[0].x - 14.f,
            (corners[0].y + corners[3].y) * 0.5f
        };
        draw_list->AddText(mid_left, IM_COL32(100, 200, 255, 200), "<>");
    }

    return consumed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience: draw a mirror toggle button above the selection box.
// Call after update() if you want the Mirror button.
// Returns true if mirror was toggled this frame.
// ─────────────────────────────────────────────────────────────────────────────
static bool mirrorButton(ImDrawList* draw_list,
                          const Transform& t,
                          ImVec2 world_size,
                          float zoom, ImVec2 pan,
                          ImVec2 canvas_min, ImVec2 canvas_size)
{
    ImVec2 corners[4];
    ImVec2 rot_handle;
    getHandlePositions(t, world_size, canvas_min, canvas_size, pan, zoom,
                       corners, rot_handle);

    // Place button to the right of the rotate handle
    ImVec2 btn_pos = { corners[1].x + 8.f, corners[0].y - 20.f };
    ImVec2 btn_min = btn_pos;
    ImVec2 btn_max = { btn_pos.x + 52.f, btn_pos.y + 18.f };

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool hov = mouse.x >= btn_min.x && mouse.x <= btn_max.x &&
               mouse.y >= btn_min.y && mouse.y <= btn_max.y;

    draw_list->AddRectFilled(btn_min, btn_max,
        hov ? IM_COL32(80, 120, 200, 220) : IM_COL32(50, 50, 50, 200), 3.f);
    draw_list->AddText({ btn_min.x + 6.f, btn_min.y + 2.f },
        IM_COL32(255, 255, 255, 230), "Mirror");

    if (hov && ImGui::IsMouseClicked(0))
        return true;
    return false;
}

} // namespace TransformHandles