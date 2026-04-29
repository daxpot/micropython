#!/usr/bin/env python3
"""
build_backbone.py
=================

Build the int8-quantized MobileNetV1 backbone for vision_ml on ESP32-P4.

Workflow:
  1. Load Keras MobileNetV1 alpha=0.25, input_shape=(224,224,3), include_top=False
     up to conv_pw_13_relu (matches the truncation point used in the browser
     trainer at lerobot-web/src/pages/ImageClassification.vue).
  2. Convert to TFLite with full-integer post-training quantization (int8).
     Input dtype: int8 (after pre-processing (pixel/127.5)-1 then quantize).
     Output dtype: int8 (feature map [1, 7, 7, 256] before GAP).
  3. Write backbone.tflite (~480KB) ready to be flashed at the ml_model
     partition offset 0xF00000.

Usage:
  python tools/build_backbone.py [--calib-dir DIR] [--out backbone.tflite]

  --calib-dir DIR  Directory containing 50-200 representative JPEG/PNG images
                   (any size, will be resized to 224x224). Used to calibrate
                   activation quantization ranges. If omitted, falls back to
                   random images (worse accuracy; OK for smoke-testing).
  --out PATH       Output path. Default: backbone.tflite in cwd.

Flash to device after building:
  esptool.py --chip esp32p4 -b 460800 write_flash 0xF00000 backbone.tflite

Requirements:
  pip install tensorflow numpy pillow
"""

import argparse
import os
import sys
from pathlib import Path


def build_truncated_backbone():
    import tensorflow as tf

    base = tf.keras.applications.MobileNet(
        input_shape=(224, 224, 3),
        alpha=0.25,
        include_top=False,
        weights='imagenet',
    )
    truncate_layer = base.get_layer('conv_pw_13_relu')
    expected_shape = (None, 7, 7, 256)
    actual_shape = tuple(truncate_layer.output.shape)
    if actual_shape != expected_shape:
        raise RuntimeError(
            f"conv_pw_13_relu output shape is {actual_shape}, expected "
            f"{expected_shape}. The browser-side frontend assumes 256 channels "
            f"(GAP -> 256-dim feature). Aborting."
        )
    truncated = tf.keras.Model(inputs=base.input, outputs=truncate_layer.output)
    return truncated


def representative_dataset_from_dir(calib_dir, n=100):
    import numpy as np
    from PIL import Image

    paths = []
    for ext in ("jpg", "jpeg", "png", "bmp"):
        paths.extend(Path(calib_dir).rglob(f"*.{ext}"))
        paths.extend(Path(calib_dir).rglob(f"*.{ext.upper()}"))
    if not paths:
        raise RuntimeError(f"No images found under {calib_dir}")
    paths = paths[:n]
    print(f"Calibration: using {len(paths)} images from {calib_dir}")

    def gen():
        for p in paths:
            try:
                img = Image.open(p).convert("RGB").resize((224, 224))
            except Exception as e:
                print(f"  skip {p}: {e}", file=sys.stderr)
                continue
            arr = np.asarray(img, dtype=np.float32)
            arr = arr / 127.5 - 1.0
            yield [arr[None, ...]]

    return gen


def representative_dataset_random(n=100):
    import numpy as np

    print(
        "WARNING: using random calibration data; quantization accuracy will "
        "suffer. Pass --calib-dir for real calibration.",
        file=sys.stderr,
    )

    def gen():
        rng = np.random.default_rng(0)
        for _ in range(n):
            arr = rng.uniform(-1.0, 1.0, size=(1, 224, 224, 3)).astype("float32")
            yield [arr]

    return gen


def quantize(model, rep_dataset_gen, out_path):
    import tensorflow as tf

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = rep_dataset_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()

    with open(out_path, "wb") as f:
        f.write(tflite_model)
    return len(tflite_model)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--calib-dir", default=None,
                    help="Directory with representative images.")
    ap.add_argument("--calib-n", type=int, default=100,
                    help="Number of calibration images to use.")
    ap.add_argument("--out", default="backbone.tflite",
                    help="Output TFLite file path.")
    args = ap.parse_args()

    print("Loading MobileNetV1 alpha=0.25, truncating to conv_pw_13_relu...")
    model = build_truncated_backbone()
    model.summary()

    if args.calib_dir:
        rep = representative_dataset_from_dir(args.calib_dir, n=args.calib_n)
    else:
        rep = representative_dataset_random(n=args.calib_n)

    print(f"Quantizing (int8 PTQ) and writing to {args.out}...")
    size = quantize(model, rep, args.out)
    print(f"Done. {args.out} = {size} bytes ({size/1024:.1f} KB)")

    if size > 0x100000:
        print(
            f"ERROR: output {size} bytes exceeds ml_model partition size "
            f"(0x100000 = 1MB). Reduce backbone or enlarge partition.",
            file=sys.stderr,
        )
        sys.exit(1)

    print("\nFlash with:")
    print(
        f"  esptool.py --chip esp32p4 -b 460800 write_flash 0xF00000 {args.out}"
    )


if __name__ == "__main__":
    main()
