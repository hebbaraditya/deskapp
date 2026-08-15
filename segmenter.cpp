#include "segmenter.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>

// kSAMSize/kEmbedC/kEmbedH/kEmbedW come from segmenter.h — shared with
// segmenter_math.cpp (preprocess()/upscaleMask(), defined there now).

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