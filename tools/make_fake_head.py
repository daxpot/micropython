#!/usr/bin/env python3
"""
make_fake_head.py
=================

Generate a fake manifest.json + head_weights.bin pair for smoke-testing
the vision_ml end-to-end pipeline on device WITHOUT needing to train a
model in the browser.

The weights are random-init Glorot-uniform; predictions will be near-uniform
random (about 1/N for each class). Use this only to verify file I/O,
manifest parsing, head allocation, and forward-pass plumbing.

Usage:
  python tools/make_fake_head.py --out ./fake_head --classes apple,banana,orange
  # then upload ./fake_head/* to device:/models/fake/
"""

import argparse
import json
import math
import os
import random
import struct
import time


def glorot_uniform(rng, fan_in, fan_out):
    limit = math.sqrt(6.0 / (fan_in + fan_out))
    return [rng.uniform(-limit, limit) for _ in range(fan_in * fan_out)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="./fake_head", help="Output directory.")
    ap.add_argument("--classes", default="apple,banana,orange",
                    help="Comma-separated class labels.")
    ap.add_argument("--hidden", type=int, default=128, help="Hidden layer dim.")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--name", default="fake-smoke", help="train_name field.")
    args = ap.parse_args()

    classes = [c.strip() for c in args.classes.split(",") if c.strip()]
    N = len(classes)
    H = args.hidden
    IN = 256

    rng = random.Random(args.seed)

    fc1_W = glorot_uniform(rng, IN, H)
    fc1_b = [0.0] * H
    fc2_W = glorot_uniform(rng, H, N)
    fc2_b = [0.0] * N

    os.makedirs(args.out, exist_ok=True)

    bin_path = os.path.join(args.out, "head_weights.bin")
    with open(bin_path, "wb") as f:
        for v in fc1_W: f.write(struct.pack("<f", v))
        for v in fc1_b: f.write(struct.pack("<f", v))
        for v in fc2_W: f.write(struct.pack("<f", v))
        for v in fc2_b: f.write(struct.pack("<f", v))

    expected_bytes = (IN * H + H + H * N + N) * 4
    actual_bytes = os.path.getsize(bin_path)
    assert actual_bytes == expected_bytes, f"bin size {actual_bytes} != {expected_bytes}"

    fc1_W_bytes = IN * H * 4
    fc1_b_off = fc1_W_bytes
    fc1_b_bytes = H * 4
    fc2_W_off = fc1_b_off + fc1_b_bytes
    fc2_W_bytes = H * N * 4
    fc2_b_off = fc2_W_off + fc2_W_bytes
    fc2_b_bytes = N * 4

    manifest = {
        "version": 1,
        "task": "image_classification",
        "backbone": {
            "name": "mobilenet_v1_0.25_224",
            "input_size": 224,
            "input_norm": "minus1_to_1",
            "feature_extract": "conv_pw_13_relu + GAP",
            "feature_dim": 256,
        },
        "head": {
            "type": "dense_softmax",
            "layers": [
                {"name": "fc1", "in": IN, "out": H, "activation": "relu"},
                {"name": "dropout", "rate": 0.5},
                {"name": "fc2", "in": H, "out": N, "activation": "softmax"},
            ],
            "weights_layout": [
                {"name": "fc1.W", "shape": [IN, H], "dtype": "float32",
                 "offset": 0, "bytes": fc1_W_bytes},
                {"name": "fc1.b", "shape": [H], "dtype": "float32",
                 "offset": fc1_b_off, "bytes": fc1_b_bytes},
                {"name": "fc2.W", "shape": [H, N], "dtype": "float32",
                 "offset": fc2_W_off, "bytes": fc2_W_bytes},
                {"name": "fc2.b", "shape": [N], "dtype": "float32",
                 "offset": fc2_b_off, "bytes": fc2_b_bytes},
            ],
        },
        "classes": classes,
        "trained_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "train_id": f"fake-{args.seed}",
        "train_name": args.name,
    }

    json_path = os.path.join(args.out, "manifest.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    print(f"Wrote {json_path}")
    print(f"Wrote {bin_path}  ({actual_bytes} bytes)")
    print()
    print("Upload to device:")
    print(f"  mpremote mkdir :/models 2>/dev/null; mpremote mkdir :/models/fake")
    print(f"  mpremote cp {json_path} :/models/fake/manifest.json")
    print(f"  mpremote cp {bin_path}  :/models/fake/head_weights.bin")


if __name__ == "__main__":
    main()
