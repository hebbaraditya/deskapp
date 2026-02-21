#include "segmenter.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <stdio.h>
#include <cfloat>

Segmenter::Segmenter()
    : m_env(ORT_LOGGING_LEVEL_WARNING, "segmenter")
{
    m_session_options.SetIntraOpNumThreads(1);
    m_session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
}

Segmenter::~Segmenter() {}

bool Segmenter::load(const std::string& model_path) {
    try {
        m_session = std::make_unique<Ort::Session>(
            m_env,
            model_path.c_str(),
            m_session_options
        );
        m_loaded = true;
        printf("[Segmenter] Model loaded: %s\n", model_path.c_str());
        return true;
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[Segmenter] Failed to load model: %s\n", e.what());
        m_loaded = false;
        return false;
    }
}

SegmentResult Segmenter::run(
    const std::vector<unsigned char>& pixels,
    int image_width,
    int image_height,
    float click_x,
    float click_y)
{
    SegmentResult result;
    result.width   = image_width;
    result.height  = image_height;
    result.success = false;

    if (!m_loaded) {
        fprintf(stderr, "[Segmenter] Model not loaded.\n");
        return result;
    }

    // --- Preprocess ---
    std::vector<float> input_tensor = preprocess(pixels, image_width, image_height);

    // --- Build input tensor ---
    std::array<int64_t, 4> input_shape = {1, 3, MODEL_SIZE, MODEL_SIZE};
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    Ort::Value input_ort = Ort::Value::CreateTensor<float>(
        mem_info,
        input_tensor.data(),
        input_tensor.size(),
        input_shape.data(),
        input_shape.size()
    );

    // --- Run inference ---
    const char* input_names[]  = {"images"};
    const char* output_names[] = {"output0", "output1"};

    std::vector<Ort::Value> outputs;
    try {
        outputs = m_session->Run(
            Ort::RunOptions{nullptr},
            input_names,
            &input_ort,
            1,
            output_names,
            2
        );
    } catch (const Ort::Exception& e) {
        fprintf(stderr, "[Segmenter] Inference failed: %s\n", e.what());
        return result;
    }

    // output0: [1, 37, 8400]     — detections
    // output1: [1, 32, 160, 160] — prototype masks
    const float* out0_ptr = outputs[0].GetTensorData<float>();
    const float* out1_ptr = outputs[1].GetTensorData<float>();

    auto out0_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    auto out1_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();

    int num_features   = (int)out0_shape[1]; // 37
    int num_detections = (int)out0_shape[2]; // 8400
    int proto_h        = (int)out1_shape[2]; // 160
    int proto_w        = (int)out1_shape[3]; // 160
    int num_protos     = (int)out1_shape[1]; // 32

    std::vector<float> output0(out0_ptr, out0_ptr + num_features * num_detections);
    std::vector<float> output1(out1_ptr, out1_ptr + num_protos * proto_h * proto_w);

    // --- Postprocess ---
    result.mask    = postprocess(output0, output1, image_width, image_height, click_x, click_y);
    result.success = !result.mask.empty();
    return result;
}

// Resize + normalize into NCHW float tensor
std::vector<float> Segmenter::preprocess(
    const std::vector<unsigned char>& pixels,
    int src_width,
    int src_height)
{
    const int dst = MODEL_SIZE;
    std::vector<float> tensor(3 * dst * dst);

    float scale_x = (float)src_width  / dst;
    float scale_y = (float)src_height / dst;

    const float mean[3] = {0.485f, 0.456f, 0.406f};
    const float std_[3] = {0.229f, 0.224f, 0.225f};

    for (int y = 0; y < dst; y++) {
        for (int x = 0; x < dst; x++) {
            int sx = std::min((int)(x * scale_x), src_width  - 1);
            int sy = std::min((int)(y * scale_y), src_height - 1);

            int src_idx = (sy * src_width + sx) * 4;
            float r = pixels[src_idx + 0] / 255.0f;
            float g = pixels[src_idx + 1] / 255.0f;
            float b = pixels[src_idx + 2] / 255.0f;

            tensor[0 * dst * dst + y * dst + x] = (r - mean[0]) / std_[0];
            tensor[1 * dst * dst + y * dst + x] = (g - mean[1]) / std_[1];
            tensor[2 * dst * dst + y * dst + x] = (b - mean[2]) / std_[2];
        }
    }
    return tensor;
}

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static float bilinear_sample(const std::vector<float>& map, int w, int h, float x, float y) {
    x = std::max(0.0f, std::min((float)(w - 1), x));
    y = std::max(0.0f, std::min((float)(h - 1), y));

    int x0 = (int)x, y0 = (int)y;
    int x1 = std::min(x0 + 1, w - 1);
    int y1 = std::min(y0 + 1, h - 1);

    float wx = x - x0, wy = y - y0;

    float v00 = map[y0 * w + x0];
    float v10 = map[y0 * w + x1];
    float v01 = map[y1 * w + x0];
    float v11 = map[y1 * w + x1];

    return v00 * (1 - wx) * (1 - wy)
         + v10 *      wx  * (1 - wy)
         + v01 * (1 - wx) *      wy
         + v11 *      wx  *      wy;
}

std::vector<unsigned char> Segmenter::postprocess(
    const std::vector<float>& output0,
    const std::vector<float>& output1,
    int src_width,
    int src_height,
    float click_x,
    float click_y)
{
    const int num_det   = 8400;
    const int proto_h   = 160;
    const int proto_w   = 160;
    const int num_proto = 32;

    // Scale click from image space → model space
    float scale_x       = (float)MODEL_SIZE / src_width;
    float scale_y       = (float)MODEL_SIZE / src_height;
    float click_model_x = click_x * scale_x;
    float click_model_y = click_y * scale_y;

    const float conf_thresh = 0.3f;

    // Pick the SMALLEST box containing the click with sufficient confidence.
    // Smaller box = more specific object = better match for what was clicked.
    int   best_det  = -1;
    float best_area = FLT_MAX;

    for (int d = 0; d < num_det; d++) {
        float score = output0[4 * num_det + d];
        if (score < conf_thresh) continue;

        float cx = output0[0 * num_det + d];
        float cy = output0[1 * num_det + d];
        float w  = output0[2 * num_det + d];
        float h  = output0[3 * num_det + d];

        float x1 = cx - w * 0.5f;
        float y1 = cy - h * 0.5f;
        float x2 = cx + w * 0.5f;
        float y2 = cy + h * 0.5f;

        if (click_model_x >= x1 && click_model_x <= x2 &&
            click_model_y >= y1 && click_model_y <= y2)
        {
            float area = w * h;
            if (area < best_area) {
                best_area = area;
                best_det  = d;
            }
        }
    }

    if (best_det < 0) {
        printf("[Segmenter] No detection at click (%.1f, %.1f) in model space\n",
               click_model_x, click_model_y);
        return {};
    }

    float best_score = output0[4 * num_det + best_det];
    printf("[Segmenter] Best detection %d, score=%.3f, area=%.1f\n",
           best_det, best_score, best_area);

    // Extract 32 mask coefficients
    std::vector<float> coeffs(num_proto);
    for (int p = 0; p < num_proto; p++) {
        coeffs[p] = output0[(5 + p) * num_det + best_det];
    }

    // Compute mask at 160x160: sigmoid(coeffs dot protos)
    std::vector<float> mask_160(proto_h * proto_w, 0.0f);
    for (int p = 0; p < num_proto; p++) {
        const float* proto_map = &output1[p * proto_h * proto_w];
        for (int i = 0; i < proto_h * proto_w; i++) {
            mask_160[i] += coeffs[p] * proto_map[i];
        }
    }
    for (auto& v : mask_160) v = sigmoid(v);

    // Scale up 160x160 → src_width x src_height using bilinear interpolation
    std::vector<unsigned char> final_mask(src_width * src_height, 0);

    float sx = (float)(proto_w - 1) / (src_width  - 1);
    float sy = (float)(proto_h - 1) / (src_height - 1);

    for (int y = 0; y < src_height; y++) {
        for (int x = 0; x < src_width; x++) {
            float mx  = x * sx;
            float my  = y * sy;
            float val = bilinear_sample(mask_160, proto_w, proto_h, mx, my);
            final_mask[y * src_width + x] = (val > 0.5f) ? 255 : 0;
        }
    }

    return final_mask;
}