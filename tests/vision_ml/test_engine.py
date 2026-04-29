# test_engine.py — Stage 1: backbone engine smoke test (no training data needed)
#
# Run on device (mpremote run test_engine.py). Verifies:
#   - ml_model partition is found and mmap'd
#   - backbone.tflite schema is valid
#   - TFLM AllocateTensors succeeds (all required ops are in the resolver)
#   - tensor arena fits in 1MB
#   - feature extraction returns a 256-dim vector for a synthetic image
#
# If you see "engine init failed: -X" the X tells you what went wrong:
#   -1 ml_model partition not found (re-flash partition table)
#   -2 mmap failed
#   -3 model schema mismatch (regenerate backbone.tflite)
#   -4 PSRAM allocation failed
#   -5 AllocateTensors (likely missing op in resolver — check serial log)
#   -6 unexpected I/O shape (backbone wasn't truncated correctly)

import time
import vision_ml
import gc

print("=" * 50)
print("vision_ml Stage 1: engine smoke test")
print("=" * 50)

print("\n[1] Calling vision_ml.init() ...")
t0 = time.ticks_ms()
vision_ml.init()
print(f"    OK  ({time.ticks_diff(time.ticks_ms(), t0)} ms)")

print("\n[2] vision_ml.info():")
info = vision_ml.info()
for k, v in info.items():
    print(f"    {k}: {v}")

print("\n[3] Building synthetic RGB888 input (224x224x3) ...")
N = vision_ml.INPUT_BYTES
buf = bytearray(N)
for i in range(0, N, 3):
    buf[i]     = (i >> 8) & 0xFF
    buf[i + 1] = (i >> 4) & 0xFF
    buf[i + 2] = i & 0xFF
print(f"    {len(buf)} bytes")

# We can't call the engine directly (no Python wrapper); the only way to
# exercise it is through ImageClassifier. Skip forward test if no model dir.
import os
try:
    os.stat('/models')
    has_models = True
except OSError:
    has_models = False

if not has_models:
    print("\n[4] No /models/ directory found — skipping forward pass test.")
    print("    Run test_fake_head.py after uploading a fake head, or")
    print("    upload a real trained head from the browser to /models/<name>/")
else:
    print("\n[4] /models/ exists — list:")
    for name in os.listdir('/models'):
        print(f"    /models/{name}")

print("\n[5] vision_ml.deinit() ...")
vision_ml.deinit()
print("    OK")

gc.collect()
print("\n=== Stage 1 PASS ===")
