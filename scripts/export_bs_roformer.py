#!/usr/bin/env python3
"""
Export BS-RoFormer model to ONNX format for Sonik stem separation.

Usage:
    python scripts/export_bs_roformer.py \
        --checkpoint "/path/to/model_bs_roformer_ep_317_sdr_12.9755.ckpt" \
        --config "/path/to/model_bs_roformer_ep_317_sdr_12.9755.yaml" \
        --output "~/Library/Application Support/Sonik/Models/bs_roformer.onnx"
"""

import argparse
import os
import sys
import yaml
import torch
import torch.nn as nn
import numpy as np

# Add audio_separator's venv if needed
sys.path.insert(0, "/Users/lorenzobalboni/Developer/stems splitter/venv/lib/python3.12/site-packages")

from audio_separator.separator.uvr_lib_v5.roformer.bs_roformer import BSRoformer


def load_model(config_path: str, checkpoint_path: str) -> nn.Module:
    """Load BS-RoFormer from config + checkpoint."""
    with open(config_path, "r") as f:
        config = yaml.load(f, Loader=yaml.FullLoader)

    model_cfg = config["model"]

    # Disable flash attention for ONNX export compatibility
    model_cfg["flash_attn"] = False

    # Build the model
    model = BSRoformer(
        dim=model_cfg["dim"],
        depth=model_cfg["depth"],
        stereo=model_cfg["stereo"],
        num_stems=model_cfg["num_stems"],
        time_transformer_depth=model_cfg["time_transformer_depth"],
        freq_transformer_depth=model_cfg["freq_transformer_depth"],
        freqs_per_bands=tuple(model_cfg["freqs_per_bands"]),
        dim_head=model_cfg["dim_head"],
        heads=model_cfg["heads"],
        attn_dropout=0.0,  # No dropout at inference
        ff_dropout=0.0,
        flash_attn=False,  # Must be False for ONNX export
        dim_freqs_in=model_cfg["dim_freqs_in"],
        stft_n_fft=model_cfg["stft_n_fft"],
        stft_hop_length=model_cfg["stft_hop_length"],
        stft_win_length=model_cfg["stft_win_length"],
        stft_normalized=model_cfg["stft_normalized"],
        mask_estimator_depth=model_cfg["mask_estimator_depth"],
    )

    # Load checkpoint
    checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=False)

    # The checkpoint might have 'state_dict' key or be the state dict directly
    if isinstance(checkpoint, dict) and "state_dict" in checkpoint:
        state_dict = checkpoint["state_dict"]
    else:
        state_dict = checkpoint

    model.load_state_dict(state_dict)
    model.eval()

    print(f"Model loaded: {sum(p.numel() for p in model.parameters()) / 1e6:.1f}M parameters")
    return model, config


def export_to_onnx(model: nn.Module, config: dict, output_path: str):
    """Export model to ONNX format."""
    chunk_size = config["audio"]["chunk_size"]  # 352800
    num_channels = config["audio"]["num_channels"]  # 2

    print(f"Exporting with chunk_size={chunk_size}, channels={num_channels}")

    # Create dummy input: [batch=1, channels=2, samples]
    dummy_input = torch.randn(1, num_channels, chunk_size)

    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    print("Starting ONNX export (this may take a few minutes)...")

    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=["mix"],
        output_names=["vocals"],
        dynamic_axes={
            "mix": {0: "batch", 2: "samples"},
            "vocals": {0: "batch", 2: "samples"},
        },
        opset_version=18,
        do_constant_folding=True,
        dynamo=False,  # Use legacy TorchScript exporter for complex models
    )

    file_size = os.path.getsize(output_path) / (1024 * 1024)
    print(f"ONNX model saved to: {output_path} ({file_size:.1f} MB)")


def verify_onnx(output_path: str, config: dict):
    """Verify the exported ONNX model."""
    import onnxruntime as ort

    chunk_size = config["audio"]["chunk_size"]

    print("Verifying ONNX model with onnxruntime...")
    session = ort.InferenceSession(output_path)

    # Print input/output info
    for inp in session.get_inputs():
        print(f"  Input:  {inp.name} shape={inp.shape} type={inp.type}")
    for out in session.get_outputs():
        print(f"  Output: {out.name} shape={out.shape} type={out.type}")

    # Run inference with dummy data
    dummy = np.random.randn(1, 2, chunk_size).astype(np.float32)
    result = session.run(None, {"mix": dummy})
    print(f"  Output shape: {result[0].shape}")
    print("ONNX verification passed!")


def main():
    parser = argparse.ArgumentParser(description="Export BS-RoFormer to ONNX")
    parser.add_argument("--checkpoint", required=True, help="Path to .ckpt file")
    parser.add_argument("--config", required=True, help="Path to .yaml config")
    parser.add_argument(
        "--output",
        default=os.path.expanduser(
            "~/Library/Application Support/Sonik/Models/bs_roformer.onnx"
        ),
        help="Output ONNX path",
    )
    parser.add_argument("--verify", action="store_true", default=True)
    args = parser.parse_args()

    model, config = load_model(args.config, args.checkpoint)
    export_to_onnx(model, config, args.output)

    if args.verify:
        verify_onnx(args.output, config)

    print("\nDone! Update ModelManager::getModelFilename() to 'bs_roformer.onnx'")


if __name__ == "__main__":
    main()
