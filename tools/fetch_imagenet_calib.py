#!/usr/bin/env python3
"""
fetch_imagenet_calib.py
=======================

Download ~100 ImageNet sample images for calibrating the int8 quantization
of the MobileNetV1 backbone (used by build_backbone.py --calib-dir).

Source: https://github.com/EliSchwartz/imagenet-sample-images
  (a community-maintained set of 1000 representative ImageNet images,
   one per class; permissively licensed for research use.)

Usage:
  python tools/fetch_imagenet_calib.py [--out ./calib] [--n 100]
"""

import argparse
import os
import random
import ssl
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO_API = "https://api.github.com/repos/EliSchwartz/imagenet-sample-images/contents/"
RAW_BASES = [
    "https://cdn.jsdelivr.net/gh/EliSchwartz/imagenet-sample-images@master/",
    "https://raw.githubusercontent.com/EliSchwartz/imagenet-sample-images/master/",
    "https://ghproxy.com/https://raw.githubusercontent.com/EliSchwartz/imagenet-sample-images/master/",
]

# Tolerate sporadic SSL truncations on the GFW path
_SSL_CTX = ssl.create_default_context()


def list_images():
    """Return list of .JPEG filenames in the repo root."""
    import json
    req = urllib.request.Request(REPO_API, headers={"User-Agent": "calib-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=30, context=_SSL_CTX) as r:
        data = json.load(r)
    return [item["name"] for item in data
            if item.get("type") == "file" and item["name"].lower().endswith((".jpeg", ".jpg", ".png"))]


def download_one(name, out_dir, retries=3):
    dst = os.path.join(out_dir, name)
    if os.path.exists(dst) and os.path.getsize(dst) > 1024:
        return dst, True
    last_err = None
    for attempt in range(retries):
        for base in RAW_BASES:
            url = base + name
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "calib-fetch/1.0"})
                with urllib.request.urlopen(req, timeout=20, context=_SSL_CTX) as r:
                    data = r.read()
                if len(data) < 1024:
                    last_err = f"too small ({len(data)} B) from {base}"
                    continue
                with open(dst, "wb") as f:
                    f.write(data)
                return dst, True
            except Exception as e:
                last_err = f"{type(e).__name__}: {e} (via {base[:40]}...)"
                continue
        time.sleep(0.5 * (attempt + 1))
    return f"{name}: {last_err}", False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="./calib", help="Output directory.")
    ap.add_argument("--n", type=int, default=100, help="Number of images to download.")
    ap.add_argument("--seed", type=int, default=0, help="Random seed for sampling.")
    ap.add_argument("--workers", type=int, default=8, help="Parallel download workers.")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    print("Listing repo contents from GitHub API...")
    try:
        names = list_images()
    except Exception as e:
        print(f"Failed to list repo: {e}", file=sys.stderr)
        print("If GitHub is blocked, try setting HTTPS_PROXY or use a mirror.", file=sys.stderr)
        sys.exit(1)
    print(f"  found {len(names)} images")

    random.seed(args.seed)
    sample = random.sample(names, min(args.n, len(names)))

    print(f"Downloading {len(sample)} images to {args.out} with {args.workers} workers...")
    ok = 0
    fail = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futures = {ex.submit(download_one, n, args.out): n for n in sample}
        for i, fut in enumerate(as_completed(futures), 1):
            res, success = fut.result()
            if success:
                ok += 1
                if ok % 10 == 0 or ok == len(sample):
                    print(f"  [{ok}/{len(sample)}] ...")
            else:
                fail.append(res)

    print(f"\nDone. {ok}/{len(sample)} downloaded to {args.out}")
    if fail:
        print(f"{len(fail)} failures:", file=sys.stderr)
        for f in fail[:5]:
            print(f"  {f}", file=sys.stderr)

    print("\nNext step:")
    print(f"  python tools/build_backbone.py --calib-dir {args.out} --out backbone.tflite")


if __name__ == "__main__":
    main()
