// Stub Segmenter for the web build (stage 3 of the web port) — always
// reports "not ready", exactly like the native build behaves today when
// the MobileSAM model files are missing from disk. main.cpp already
// handles that gracefully (prints a warning, the Segment tool just
// no-ops), so this stub needs zero changes there — same public API as
// segmenter.cpp, different (currently empty) Impl behind it.
//
// Real ONNX Runtime Web integration lands in web-port stage 4, replacing
// just this file — segmenter.h and main.cpp shouldn't need to change
// again for that.

#include "segmenter.h"
#include <cstdio>

struct Segmenter::Impl {
    // Intentionally empty for now — stage 4 adds the ONNX Runtime Web
    // session handles here (bridged via EM_JS, same pattern as
    // platform_web.cpp).
};

Segmenter::Segmenter() : impl_(std::make_unique<Impl>()) {}
Segmenter::~Segmenter() = default;

bool Segmenter::loadModels(const std::string& encoder_path,
                           const std::string& decoder_path)
{
    (void)encoder_path;
    (void)decoder_path;
    printf("[Segmenter] Web build: segmentation not implemented yet "
           "(web-port stage 4).\n");
    return false;
}

bool Segmenter::encodeImage(const uint8_t* pixels_rgba, int width, int height)
{
    (void)pixels_rgba;
    (void)width;
    (void)height;
    return false;
}

SegmentResult Segmenter::decode(const std::vector<PromptPoint>& points)
{
    (void)points;
    return SegmentResult{}; // valid=false, matches "no result" everywhere else
}
