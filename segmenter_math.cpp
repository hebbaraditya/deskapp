// Segmenter's pure-math helpers — image preprocessing and mask upscaling.
// Zero ONNX Runtime dependency (native or web), so this one file compiles
// into both builds unchanged, instead of being duplicated between
// segmenter.cpp (native) and segmenter_web.cpp (web).

#include "segmenter.h"
#include <algorithm>
#include <cmath>

// ── ImageNet normalisation constants ─────────────────────────────────────────
static constexpr float kMean[3] = {123.675f, 116.28f,  103.53f};
static constexpr float kStd[3]  = {58.395f,  57.12f,   57.375f};

// ─────────────────────────────────────────────────────────────────────────────
// Preprocess: RGBA uint8 -> float32 [1, 3, 1024, 1024]
// Resizes longest side to 1024, pads remainder with zeros.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> Segmenter::preprocess(const uint8_t* rgba, int w, int h)
{
    float scale = static_cast<float>(kSAMSize) / std::max(w, h);
    int   rw    = static_cast<int>(std::round(w * scale));
    int   rh    = static_cast<int>(std::round(h * scale));

    // Bilinear resize into temporary RGB buffer
    std::vector<uint8_t> rgb_resized(rw * rh * 3);
    for (int y = 0; y < rh; ++y) {
        float src_y = (y + 0.5f) / scale - 0.5f;
        int   y0    = std::max(0, (int)src_y);
        int   y1    = std::min(h - 1, y0 + 1);
        float wy    = src_y - y0;

        for (int x = 0; x < rw; ++x) {
            float src_x = (x + 0.5f) / scale - 0.5f;
            int   x0    = std::max(0, (int)src_x);
            int   x1    = std::min(w - 1, x0 + 1);
            float wx    = src_x - x0;

            for (int c = 0; c < 3; ++c) {
                float v00 = rgba[(y0 * w + x0) * 4 + c];
                float v01 = rgba[(y0 * w + x1) * 4 + c];
                float v10 = rgba[(y1 * w + x0) * 4 + c];
                float v11 = rgba[(y1 * w + x1) * 4 + c];
                float val = (1 - wy) * ((1 - wx) * v00 + wx * v01)
                           +     wy  * ((1 - wx) * v10 + wx * v11);
                rgb_resized[(y * rw + x) * 3 + c] = static_cast<uint8_t>(val);
            }
        }
    }

    // Pad to 1024x1024 and normalise -> CHW float
    std::vector<float> tensor(3 * kSAMSize * kSAMSize, 0.f);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            for (int c = 0; c < 3; ++c) {
                float pixel = rgb_resized[(y * rw + x) * 3 + c];
                float norm  = (pixel - kMean[c]) / kStd[c];
                tensor[c * kSAMSize * kSAMSize + y * kSAMSize + x] = norm;
            }
        }
    }
    return tensor;
}

// ─────────────────────────────────────────────────────────────────────────────
static float bilinear_sample(const float* data, int w, int h, float fx, float fy)
{
    int x0 = std::max(0, (int)fx);
    int y0 = std::max(0, (int)fy);
    int x1 = std::min(w - 1, x0 + 1);
    int y1 = std::min(h - 1, y0 + 1);
    float wx = fx - x0, wy = fy - y0;
    return (1-wy)*((1-wx)*data[y0*w+x0] + wx*data[y0*w+x1])
          +   wy *((1-wx)*data[y1*w+x0] + wx*data[y1*w+x1]);
}

std::vector<uint8_t> Segmenter::upscaleMask(const float* low_res,
                                             int mask_w, int mask_h,
                                             int out_w,  int out_h)
{
    std::vector<uint8_t> result(out_w * out_h, 0);
    float sx = static_cast<float>(mask_w - 1) / (out_w - 1);
    float sy = static_cast<float>(mask_h - 1) / (out_h - 1);
    for (int y = 0; y < out_h; ++y)
        for (int x = 0; x < out_w; ++x)
            result[y * out_w + x] =
                (bilinear_sample(low_res, mask_w, mask_h, x*sx, y*sy) > 0.f)
                ? 255 : 0;
    return result;
}
