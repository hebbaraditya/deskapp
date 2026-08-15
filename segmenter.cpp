#include "segmenter.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>

// ── ImageNet normalisation constants ─────────────────────────────────────────
static constexpr float kMean[3] = {123.675f, 116.28f,  103.53f};
static constexpr float kStd[3]  = {58.395f,  57.12f,   57.375f};
static constexpr int   kSAMSize = 1024;
static constexpr int   kEmbedH  = 64;
static constexpr int   kEmbedW  = 64;
static constexpr int   kEmbedC  = 256;

// ── Impl — the actual ONNX Runtime state, hidden from segmenter.h ────────────
struct Segmenter::Impl {
    Ort::Env            env{ORT_LOGGING_LEVEL_WARNING, "Segmenter"};
    Ort::SessionOptions session_opts;

    std::unique_ptr<Ort::Session> encoder_session;
    std::unique_ptr<Ort::Session> decoder_session;

    Impl() {
        session_opts.SetIntraOpNumThreads(4);
        session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
Segmenter::Segmenter() : impl_(std::make_unique<Impl>()) {}
Segmenter::~Segmenter() = default;

// ─────────────────────────────────────────────────────────────────────────────
bool Segmenter::loadModels(const std::string& encoder_path,
                           const std::string& decoder_path)
{
    try {
        impl_->encoder_session = std::make_unique<Ort::Session>(
            impl_->env, encoder_path.c_str(), impl_->session_opts);
        encoder_ready_ = true;
        printf("[Segmenter] Encoder loaded: %s\n", encoder_path.c_str());
    } catch (const Ort::Exception& e) {
        printf("[Segmenter] Failed to load encoder: %s\n", e.what());
        return false;
    }

    try {
        impl_->decoder_session = std::make_unique<Ort::Session>(
            impl_->env, decoder_path.c_str(), impl_->session_opts);
        decoder_ready_ = true;
        printf("[Segmenter] Decoder loaded: %s\n", decoder_path.c_str());
    } catch (const Ort::Exception& e) {
        printf("[Segmenter] Failed to load decoder: %s\n", e.what());
        return false;
    }

    return true;
}

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
bool Segmenter::encodeImage(const uint8_t* pixels_rgba, int width, int height)
{
    if (!encoder_ready_) {
        printf("[Segmenter] Encoder not loaded.\n");
        return false;
    }

    image_encoded_ = false;
    image_w_       = width;
    image_h_       = height;

    std::vector<float> input = preprocess(pixels_rgba, width, height);

    std::vector<int64_t> input_shape = {1, 3, kSAMSize, kSAMSize};
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        mem_info,
        input.data(), input.size(),
        input_shape.data(), input_shape.size());

    const char* input_names[]  = {"image"};
    const char* output_names[] = {"image_embeddings"};

    try {
        auto outputs = impl_->encoder_session->Run(
            Ort::RunOptions{nullptr},
            input_names,  &input_tensor, 1,
            output_names, 1);

        size_t emb_size = kEmbedC * kEmbedH * kEmbedW;
        embedding_.resize(emb_size);
        std::memcpy(embedding_.data(),
                    outputs[0].GetTensorData<float>(),
                    emb_size * sizeof(float));

        image_encoded_ = true;
        printf("[Segmenter] Image encoded (%dx%d)\n", width, height);
        return true;
    } catch (const Ort::Exception& e) {
        printf("[Segmenter] Encoder run failed: %s\n", e.what());
        return false;
    }
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

// ─────────────────────────────────────────────────────────────────────────────
SegmentResult Segmenter::decode(const std::vector<PromptPoint>& points)
{
    SegmentResult result;
    if (!decoder_ready_ || !image_encoded_ || points.empty())
        return result;

    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    // 1. image_embeddings [1, 256, 64, 64]
    std::vector<int64_t> emb_shape = {1, kEmbedC, kEmbedH, kEmbedW};
    Ort::Value emb_tensor = Ort::Value::CreateTensor<float>(
        mem_info, embedding_.data(), embedding_.size(),
        emb_shape.data(), emb_shape.size());

    // 2. point_coords [1, N, 2] and point_labels [1, N]
    int N = static_cast<int>(points.size());
    float scale = static_cast<float>(kSAMSize) / std::max(image_w_, image_h_);

    std::vector<float> coords(N * 2);
    std::vector<float> labels(N);
    for (int i = 0; i < N; ++i) {
        coords[i * 2 + 0] = points[i].x * scale;
        coords[i * 2 + 1] = points[i].y * scale;
        labels[i]          = static_cast<float>(points[i].label);
    }

    std::vector<int64_t> coords_shape = {1, N, 2};
    std::vector<int64_t> labels_shape = {1, N};

    Ort::Value coords_tensor = Ort::Value::CreateTensor<float>(
        mem_info, coords.data(), coords.size(),
        coords_shape.data(), coords_shape.size());

    Ort::Value labels_tensor = Ort::Value::CreateTensor<float>(
        mem_info, labels.data(), labels.size(),
        labels_shape.data(), labels_shape.size());

    // 3. mask_input [1, 1, 256, 256] — zeros (no prior mask)
    std::vector<float>   mask_input(256 * 256, 0.f);
    std::vector<int64_t> mask_shape = {1, 1, 256, 256};
    Ort::Value mask_tensor = Ort::Value::CreateTensor<float>(
        mem_info, mask_input.data(), mask_input.size(),
        mask_shape.data(), mask_shape.size());

    // 4. has_mask_input [1] = 0
    std::vector<float>   has_mask        = {0.f};
    std::vector<int64_t> has_mask_shape  = {1};
    Ort::Value has_mask_tensor = Ort::Value::CreateTensor<float>(
        mem_info, has_mask.data(), has_mask.size(),
        has_mask_shape.data(), has_mask_shape.size());

    // 5. orig_im_size [2] = [height, width]
    std::vector<float>   orig_size       = {(float)image_h_, (float)image_w_};
    std::vector<int64_t> orig_size_shape = {2};
    Ort::Value orig_size_tensor = Ort::Value::CreateTensor<float>(
        mem_info, orig_size.data(), orig_size.size(),
        orig_size_shape.data(), orig_size_shape.size());

    const char* input_names[] = {
        "image_embeddings", "point_coords", "point_labels",
        "mask_input", "has_mask_input", "orig_im_size"
    };
    const char* output_names[] = {"masks", "iou_predictions", "low_res_masks"};

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(emb_tensor));
    inputs.push_back(std::move(coords_tensor));
    inputs.push_back(std::move(labels_tensor));
    inputs.push_back(std::move(mask_tensor));
    inputs.push_back(std::move(has_mask_tensor));
    inputs.push_back(std::move(orig_size_tensor));

    try {
        auto outputs = impl_->decoder_session->Run(
            Ort::RunOptions{nullptr},
            input_names, inputs.data(), inputs.size(),
            output_names, 3);

        // masks: [1, 1, H, W] — SamOnnxModel upscales to orig size
        auto& masks_out  = outputs[0];
        auto  out_shape  = masks_out.GetTensorTypeAndShapeInfo().GetShape();
        int   out_h      = (int)out_shape[2];
        int   out_w      = (int)out_shape[3];
        const float* mask_data = masks_out.GetTensorData<float>();
        const float* iou_data  = outputs[1].GetTensorData<float>();

        result.score  = iou_data[0];
        result.width  = image_w_;
        result.height = image_h_;
        result.valid  = true;

        if (out_w == image_w_ && out_h == image_h_) {
            result.mask.resize(image_w_ * image_h_);
            for (int i = 0; i < image_w_ * image_h_; ++i)
                result.mask[i] = (mask_data[i] > 0.f) ? 255 : 0;
        } else {
            result.mask = upscaleMask(mask_data, out_w, out_h, image_w_, image_h_);
        }

        printf("[Segmenter] Decoded %dx%d IoU=%.3f\n", image_w_, image_h_, result.score);
        return result;
    } catch (const Ort::Exception& e) {
        printf("[Segmenter] Decoder run failed: %s\n", e.what());
        return result;
    }
}