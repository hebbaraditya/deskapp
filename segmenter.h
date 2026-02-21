#pragma once

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>

// Result of a segmentation inference
struct SegmentResult {
    std::vector<unsigned char> mask; // binary mask, same width/height as input image
    int width;
    int height;
    bool success;
};

class Segmenter {
public:
    Segmenter();
    ~Segmenter();

    // Load the ONNX model from disk. Call once at startup.
    bool load(const std::string& model_path);

    // Run segmentation on raw RGBA pixel data.
    // click_x, click_y are coordinates in image space (0,0 = top left of image).
    // Returns a mask the same size as the input image.
    SegmentResult run(
        const std::vector<unsigned char>& pixels,
        int image_width,
        int image_height,
        float click_x,
        float click_y
    );

    bool is_loaded() const { return m_loaded; }

private:
    // Resize and normalize image into a 640x640 float tensor (NCHW format)
    std::vector<float> preprocess(
        const std::vector<unsigned char>& pixels,
        int src_width,
        int src_height
    );

    // Decode model outputs into a binary mask for the clicked point
    // output0: [1, 37, 8400] - detections (boxes + scores + mask coefficients)
    // output1: [1, 32, 160, 160] - prototype masks
    std::vector<unsigned char> postprocess(
        const std::vector<float>& output0,
        const std::vector<float>& output1,
        int src_width,
        int src_height,
        float click_x,
        float click_y
    );

    Ort::Env m_env;
    Ort::SessionOptions m_session_options;
    std::unique_ptr<Ort::Session> m_session;
    bool m_loaded = false;

    // Model input size (FastSAM-s expects 640x640)
    static constexpr int MODEL_SIZE = 640;
};