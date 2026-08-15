#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// No onnxruntime_cxx_api.h include here on purpose — the actual ONNX
// Runtime types live behind Impl (pimpl idiom), defined only in
// segmenter.cpp (native) / segmenter_web.cpp (web, ONNX Runtime Web via a
// JS bridge instead). That's what lets this one header + this one class
// declaration serve both platforms with completely different Impls behind
// it, same as Platform::OpenImageFile/SaveFile — main.cpp using Segmenter
// never needs to know or care which backend is compiled in.

// ── Model I/O shape constants ─────────────────────────────────────────────────
// Shared by segmenter_math.cpp (preprocess/upscaleMask — pure math, no ORT
// dependency, compiled into both native and web builds) and each platform's
// Impl. `static` gives each translation unit its own internal-linkage copy,
// same pattern as kFontManifest in canvas_objects.h — no ODR issue, just
// keeps these single-source-of-truth instead of redefined per file.
static constexpr int kSAMSize = 1024; // MobileSAM's fixed square input size
static constexpr int kEmbedC  = 256;
static constexpr int kEmbedH  = 64;
static constexpr int kEmbedW  = 64;

// ── Point prompt ─────────────────────────────────────────────────────────────
struct PromptPoint {
    float x, y;      // pixel coords in the original image
    int   label;     // 1 = foreground, 0 = background
};

// ── Output mask ──────────────────────────────────────────────────────────────
struct SegmentResult {
    std::vector<uint8_t> mask;   // 1 byte per pixel, 255=fg, 0=bg
    int   width  = 0;
    int   height = 0;
    float score  = 0.f;          // IoU prediction from decoder
    bool  valid  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Segmenter
//
// Usage:
//   Segmenter seg;
//   seg.loadModels("models/mobile_sam_encoder.onnx",
//                  "models/mobile_sam_decoder.onnx");
//
//   // Once per image (slow ~200ms):
//   seg.encodeImage(pixels_rgba, width, height);
//
//   // Per stroke / click (fast ~20ms):
//   SegmentResult r = seg.decode(points);
// ─────────────────────────────────────────────────────────────────────────────
class Segmenter {
public:
    Segmenter();
    ~Segmenter(); // defined in the .cpp, where Impl is a complete type

    // Load both ONNX models. Returns false on failure.
    bool loadModels(const std::string& encoder_path,
                    const std::string& decoder_path);

    // Encode the current image. Call this once whenever the image changes.
    // pixels: RGBA, row-major, width*height*4 bytes.
    // Returns false on failure.
    bool encodeImage(const uint8_t* pixels_rgba, int width, int height);

    // Run the mask decoder with the given point prompts.
    // Returns an empty SegmentResult if no image has been encoded yet.
    SegmentResult decode(const std::vector<PromptPoint>& points);

    bool isReady()     const { return encoder_ready_ && decoder_ready_; }
    bool hasImage()    const { return image_encoded_; }
    int  imageWidth()  const { return image_w_; }
    int  imageHeight() const { return image_h_; }

private:
    // ── ONNX Runtime (native) / ONNX Runtime Web (web) ─────────────────────────
    // Opaque — see the platform-specific .cpp for what's actually inside.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool encoder_ready_ = false;
    bool decoder_ready_ = false;

    // ── Cached image embedding ────────────────────────────────────────────────
    std::vector<float> embedding_;   // [1, 256, 64, 64]
    bool               image_encoded_ = false;
    int                image_w_ = 0;
    int                image_h_ = 0;

    // ── Internal helpers ──────────────────────────────────────────────────────
    // Preprocess: RGBA -> normalised float [1,3,1024,1024]
    std::vector<float> preprocess(const uint8_t* rgba, int w, int h);

    // Post-process low-res mask [1,1,256,256] -> full-size binary mask
    std::vector<uint8_t> upscaleMask(const float* low_res_mask,
                                     int mask_w, int mask_h,
                                     int out_w,  int out_h);
};