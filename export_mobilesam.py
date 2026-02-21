"""
Export MobileSAM encoder and decoder to ONNX.
Run from the deskapp root directory:
    python3 export_mobilesam.py
"""

import os
os.environ["TORCH_ONNX_FORCE_LEGACY_EXPORTER"] = "1"

import torch
import warnings
warnings.filterwarnings("ignore")

from mobile_sam import sam_model_registry
from mobile_sam.utils.onnx import SamOnnxModel

CHECKPOINT  = "models/mobile_sam.pt"
MODEL_TYPE  = "vit_t"
IMAGE_SIZE  = 1024

print("Loading MobileSAM weights...")
sam = sam_model_registry[MODEL_TYPE](checkpoint=CHECKPOINT)
sam.eval()

# ── Export Image Encoder ─────────────────────────────────────────────────────
class EncoderWrapper(torch.nn.Module):
    def __init__(self, sam):
        super().__init__()
        self.encoder = sam.image_encoder
    def forward(self, x):
        return self.encoder(x)

print("Exporting encoder...")
encoder     = EncoderWrapper(sam)
dummy_image = torch.zeros(1, 3, IMAGE_SIZE, IMAGE_SIZE)

with torch.no_grad():
    torch.onnx.export(
        encoder,
        dummy_image,
        "models/mobile_sam_encoder.onnx",
        export_params=True,
        opset_version=16,
        do_constant_folding=True,
        input_names=["image"],
        output_names=["image_embeddings"],
    )
print("  -> models/mobile_sam_encoder.onnx ✓")

# ── Export Decoder via SamOnnxModel ──────────────────────────────────────────
print("Exporting decoder...")

embed_dim  = sam.prompt_encoder.embed_dim
embed_size = sam.prompt_encoder.image_embedding_size

onnx_model = SamOnnxModel(sam, return_single_mask=True)
onnx_model.eval()

dummy_embeddings   = torch.zeros(1, embed_dim, *embed_size)
dummy_point_coords = torch.zeros(1, 5, 2,      dtype=torch.float)
dummy_point_labels = torch.zeros(1, 5,         dtype=torch.float)
dummy_mask_input   = torch.zeros(1, 1, 256, 256, dtype=torch.float)
dummy_has_mask     = torch.zeros(1,            dtype=torch.float)
dummy_orig_size    = torch.tensor([IMAGE_SIZE, IMAGE_SIZE], dtype=torch.float)

with torch.no_grad():
    torch.onnx.export(
        onnx_model,
        (dummy_embeddings,
         dummy_point_coords,
         dummy_point_labels,
         dummy_mask_input,
         dummy_has_mask,
         dummy_orig_size),
        "models/mobile_sam_decoder.onnx",
        export_params=True,
        opset_version=16,
        do_constant_folding=True,
        input_names=[
            "image_embeddings",
            "point_coords",
            "point_labels",
            "mask_input",
            "has_mask_input",
            "orig_im_size",
        ],
        output_names=["masks", "iou_predictions", "low_res_masks"],
        dynamic_axes={
            "point_coords": {1: "num_points"},
            "point_labels": {1: "num_points"},
        },
    )
print("  -> models/mobile_sam_decoder.onnx ✓")
print("\nDone!")