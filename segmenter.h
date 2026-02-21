#pragma once

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>

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
    ~Segmenter() = default;

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
    // ── ONNX Runtime ─────────────────────────────────────────────────────────
    Ort::Env            env_;
    Ort::SessionOptions session_opts_;

    std::unique_ptr<Ort::Session> encoder_session_;
    std::unique_ptr<Ort::Session> decoder_session_;

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