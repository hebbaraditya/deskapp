// Real ONNX Runtime Web-backed Segmenter (web-port stage 4).
//
// ONNX Runtime Web's InferenceSession.run() is a JS Promise (async) —
// there's no synchronous WASM-native ORT API to link against the way
// native links onnxruntime_cxx_api.h. To keep Segmenter's public API
// identical on both platforms (same segmenter.h, zero main.cpp changes,
// same promise stage 3's commit made), this uses Emscripten's Asyncify:
// EM_ASYNC_JS lets a JS `async function` be called from C++ as if it were
// an ordinary blocking call — the WASM stack transparently unwinds while
// the Promise is pending and resumes when it resolves. Needs -sASYNCIFY=1
// on the link line (see CMakeLists.txt, web target only).
//
// Model tensors cross the JS/WASM boundary as raw pointers into WASM
// linear memory (Module.HEAPF32 views) rather than copies — same
// low-copy-marshalling approach as platform_web.cpp's malloc/HEAPU8
// bridge for image bytes.

#include "segmenter.h"

#include <emscripten.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── Impl ──────────────────────────────────────────────────────────────────
// Nothing to hold here — the actual ORT sessions live as JS objects on
// Module (see web_load_models below), not in any C++ state. Exists only to
// satisfy segmenter.h's pimpl contract (unique_ptr<Impl> needs a complete
// type at the point ~Segmenter() is defined).
struct Segmenter::Impl {};

Segmenter::Segmenter() : impl_(std::make_unique<Impl>()) {}
Segmenter::~Segmenter() = default;

// ─────────────────────────────────────────────────────────────────────────────
// JS bridge: load both models. Returns a bitmask (bit0=encoder, bit1=decoder)
// rather than a bool so loadModels() below can report per-model failures
// the same way native's two separate try/catch blocks do.
// ─────────────────────────────────────────────────────────────────────────────
EM_ASYNC_JS(int, web_load_models,
            (const char* encoder_url, const char* decoder_url), {
    var encUrl = UTF8ToString(encoder_url);
    var decUrl = UTF8ToString(decoder_url);
    var result = 0;

    try {
        Module.__yoinkboardEncoder = await ort.InferenceSession.create(encUrl);
        result |= 1;
        console.log("[Segmenter] Encoder loaded: " + encUrl);
    } catch (e) {
        console.error("[Segmenter] Failed to load encoder: " + e);
    }

    try {
        Module.__yoinkboardDecoder = await ort.InferenceSession.create(decUrl);
        result |= 2;
        console.log("[Segmenter] Decoder loaded: " + decUrl);
    } catch (e) {
        console.error("[Segmenter] Failed to load decoder: " + e);
    }

    return result;
});

bool Segmenter::loadModels(const std::string& encoder_path,
                           const std::string& decoder_path)
{
    int result = web_load_models(encoder_path.c_str(), decoder_path.c_str());
    encoder_ready_ = (result & 1) != 0;
    decoder_ready_ = (result & 2) != 0;

    if (!encoder_ready_)
        printf("[Segmenter] Failed to load encoder: %s\n", encoder_path.c_str());
    if (!decoder_ready_)
        printf("[Segmenter] Failed to load decoder: %s\n", decoder_path.c_str());

    return encoder_ready_ && decoder_ready_;
}

// ─────────────────────────────────────────────────────────────────────────────
// JS bridge: run the encoder on a preprocessed [1,3,1024,1024] input tensor
// already sitting in WASM memory at input_ptr. Returns a malloc'd pointer
// (freed by the C++ caller below, same allocator on both sides — Module.
// _malloc IS the WASM heap's malloc) to the [1,256,64,64] embedding output,
// or 0 on failure.
// ─────────────────────────────────────────────────────────────────────────────
EM_ASYNC_JS(int, web_run_encoder, (const float* input_ptr, int input_len), {
    var session = Module.__yoinkboardEncoder;
    if (!session) return 0;

    try {
        var inputArr = new Float32Array(HEAPF32.buffer, input_ptr, input_len);
        var tensor = new ort.Tensor("float32", inputArr, [1, 3, 1024, 1024]);
        var outputs = await session.run({ image: tensor });
        var embData = outputs.image_embeddings.data;

        var outPtr = _malloc(embData.length * 4);
        HEAPF32.set(embData, outPtr >> 2);
        return outPtr;
    } catch (e) {
        console.error("[Segmenter] Encoder run failed: " + e);
        return 0;
    }
});

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

    int emb_ptr = web_run_encoder(input.data(), (int)input.size());
    if (!emb_ptr) return false;

    size_t emb_size = kEmbedC * kEmbedH * kEmbedW;
    embedding_.resize(emb_size);
    std::memcpy(embedding_.data(), reinterpret_cast<const float*>(emb_ptr),
                emb_size * sizeof(float));
    free(reinterpret_cast<void*>(emb_ptr));

    image_encoded_ = true;
    printf("[Segmenter] Image encoded (%dx%d)\n", width, height);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// JS bridge: run the decoder. Mask width/height/IoU come back via WASM-memory
// out-pointers (JS writes into HEAP32/HEAPF32 at those addresses — same
// "pointer as an address into our own linear memory" idiom as everywhere
// else here); the mask data itself comes back as the malloc'd return
// pointer, same convention as web_run_encoder above.
// ─────────────────────────────────────────────────────────────────────────────
EM_ASYNC_JS(int, web_run_decoder, (
    const float* emb_ptr, int emb_len,
    const float* coords_ptr, int n_points,
    const float* labels_ptr,
    int orig_h, int orig_w,
    int* out_w, int* out_h, float* out_iou
), {
    var session = Module.__yoinkboardDecoder;
    if (!session) return 0;

    try {
        var embArr    = new Float32Array(HEAPF32.buffer, emb_ptr, emb_len);
        var coordsArr = new Float32Array(HEAPF32.buffer, coords_ptr, n_points * 2);
        var labelsArr = new Float32Array(HEAPF32.buffer, labels_ptr, n_points);
        var maskInput = new Float32Array(256 * 256); // zeros — no prior mask
        var hasMask   = new Float32Array([0]);
        var origSize  = new Float32Array([orig_h, orig_w]);

        var feeds = {
            image_embeddings: new ort.Tensor("float32", embArr, [1, 256, 64, 64]),
            point_coords:     new ort.Tensor("float32", coordsArr, [1, n_points, 2]),
            point_labels:     new ort.Tensor("float32", labelsArr, [1, n_points]),
            mask_input:       new ort.Tensor("float32", maskInput, [1, 1, 256, 256]),
            has_mask_input:   new ort.Tensor("float32", hasMask, [1]),
            orig_im_size:     new ort.Tensor("float32", origSize, [2]),
        };

        var outputs = await session.run(feeds);
        var masks   = outputs.masks;          // [1, 1, H, W]
        var dims    = masks.dims;
        var mh = dims[2], mw = dims[3];
        var iou = outputs.iou_predictions.data[0];

        HEAP32[out_w >> 2]    = mw;
        HEAP32[out_h >> 2]    = mh;
        HEAPF32[out_iou >> 2] = iou;

        var maskData = masks.data;
        var outPtr = _malloc(maskData.length * 4);
        HEAPF32.set(maskData, outPtr >> 2);
        return outPtr;
    } catch (e) {
        console.error("[Segmenter] Decoder run failed: " + e);
        return 0;
    }
});

SegmentResult Segmenter::decode(const std::vector<PromptPoint>& points)
{
    SegmentResult result;
    if (!decoder_ready_ || !image_encoded_ || points.empty())
        return result;

    int   N     = static_cast<int>(points.size());
    float scale = static_cast<float>(kSAMSize) / std::max(image_w_, image_h_);

    std::vector<float> coords(N * 2);
    std::vector<float> labels(N);
    for (int i = 0; i < N; ++i) {
        coords[i * 2 + 0] = points[i].x * scale;
        coords[i * 2 + 1] = points[i].y * scale;
        labels[i]          = static_cast<float>(points[i].label);
    }

    int   out_w = 0, out_h = 0;
    float iou   = 0.f;
    int mask_ptr = web_run_decoder(
        embedding_.data(), (int)embedding_.size(),
        coords.data(), N,
        labels.data(),
        image_h_, image_w_,
        &out_w, &out_h, &iou);

    if (!mask_ptr) {
        printf("[Segmenter] Decode failed.\n");
        return result;
    }

    const float* mask_data = reinterpret_cast<const float*>(mask_ptr);

    result.score  = iou;
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

    free(reinterpret_cast<void*>(mask_ptr));

    printf("[Segmenter] Decoded %dx%d IoU=%.3f\n", image_w_, image_h_, result.score);
    return result;
}
