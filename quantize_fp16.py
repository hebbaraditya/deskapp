"""
Convert the exported MobileSAM encoder (fp32) to fp16, for the web build —
Cloudflare Pages caps individual files at 25 MiB. The fp32 encoder is
~26.7MB (plus a ~26.6MB external-data sibling), over that limit either
way. Converting to fp16 both halves the size AND — since the result
(~14.2MB) comfortably fits ONNX's embed-vs-external-data threshold —
collapses it back into a single self-contained file, no external .data
sibling needed. That means the whole app (JS/WASM + both models) can ship
from one Cloudflare Pages deploy, no separate object storage (R2) needed.

The decoder (~15.8MB) is already under the 25MB cap on its own, so it's
left at fp32 — also sidesteps an internal bug in onnxconverter_common
(remove_unnecessary_cast_node) that this decoder's graph structure
happens to trip.

keep_io_types=True keeps the model's input/output tensors as fp32 even
though internal weights/compute become fp16 — so none of the C++/JS
tensor-marshalling code (which builds "float32" ort.Tensor objects) needs
to change to match.

The fp32 originals are left untouched — native keeps using those. Only
the web build points at mobile_sam_encoder_fp16.onnx.

Run from the deskapp root directory:
    python3 quantize_fp16.py
"""

from onnxconverter_common import float16
import onnx

print("Loading models/mobile_sam_encoder.onnx ...")
model = onnx.load("models/mobile_sam_encoder.onnx")
print("  Converting to fp16 ...")
model_fp16 = float16.convert_float_to_float16(model, keep_io_types=True)
print("  Saving models/mobile_sam_encoder_fp16.onnx ...")
onnx.save(model_fp16, "models/mobile_sam_encoder_fp16.onnx")
print("  -> done")

print("\nDone! (decoder left at fp32 — already under the 25MB cap)")
