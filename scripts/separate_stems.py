#!/usr/bin/env python3
"""Stem separation using audio_separator (BS-RoFormer).

Usage:
    python3 separate_stems.py --check
    python3 separate_stems.py --separate <input_wav> <output_dir> <model_dir>

Output is JSON on stdout.
"""

import sys
import os
import json


def check():
    """Verify that audio_separator is importable."""
    try:
        from audio_separator.separator import Separator  # noqa: F401
        print(json.dumps({"status": "ok"}))
    except ImportError as e:
        print(json.dumps({"status": "error", "message": str(e)}))
        sys.exit(1)


def separate(input_wav, output_dir, model_dir):
    """Run stem separation and output paths as JSON."""
    from audio_separator.separator import Separator

    separator = Separator(
        log_level=30,  # WARNING
        model_file_dir=model_dir,
        output_dir=output_dir,
        output_format="WAV",
    )

    # Find the .ckpt model in the model directory
    model_filename = None
    for f in sorted(os.listdir(model_dir)):
        if f.endswith(".ckpt"):
            model_filename = f
            break

    if model_filename is None:
        print(json.dumps({"error": "No .ckpt model found in " + model_dir}))
        sys.exit(1)

    separator.load_model(model_filename=model_filename)
    output_files = separator.separate(input_wav)

    vocals = None
    instrumental = None
    for f in output_files:
        basename = os.path.basename(f)
        if "(Vocals)" in basename or "_Vocals_" in basename:
            vocals = f
        elif "(Instrumental)" in basename or "_Instrumental_" in basename:
            instrumental = f

    if vocals and instrumental:
        print(json.dumps({"vocals": vocals, "instrumental": instrumental}))
    else:
        print(json.dumps({
            "error": "Could not identify output files",
            "files": output_files,
        }))
        sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(json.dumps({
            "error": "Usage: separate_stems.py [--check | --separate <input> <output_dir> <model_dir>]"
        }))
        sys.exit(1)

    if sys.argv[1] == "--check":
        check()
    elif sys.argv[1] == "--separate":
        if len(sys.argv) != 5:
            print(json.dumps({
                "error": "Usage: --separate <input_wav> <output_dir> <model_dir>"
            }))
            sys.exit(1)
        separate(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(json.dumps({"error": "Unknown command: " + sys.argv[1]}))
        sys.exit(1)


if __name__ == "__main__":
    main()
