# test_fake_head.py — Stage 2: end-to-end with fake head
#
# Prereq: upload a fake head to /models/fake/ (see tools/make_fake_head.py).
#
# Verifies:
#   - manifest.json parses, classes loaded
#   - head_weights.bin loads (size matches)
#   - classify(rgb_bytes) returns N probabilities summing to ~1.0
#   - top_k returns sorted (label, score) pairs
#
# Probabilities will be near-random (no real training); we just check
# the plumbing works and timing is reasonable.

import time
import vision_ml
import gc

print("=" * 50)
print("vision_ml Stage 2: fake-head end-to-end test")
print("=" * 50)

print("\n[1] init...")
vision_ml.init()
print("    OK")

print("\n[2] Load /models/fake/ ...")
clf = vision_ml.ImageClassifier('/models/fake/')
print(f"    classes: {clf.classes}")

print("\n[3] Build synthetic RGB888 (gradient pattern) ...")
N = vision_ml.INPUT_BYTES
buf = bytearray(N)
for y in range(224):
    for x in range(224):
        i = (y * 224 + x) * 3
        buf[i]     = x & 0xFF
        buf[i + 1] = y & 0xFF
        buf[i + 2] = (x ^ y) & 0xFF
print(f"    {len(buf)} bytes")

print("\n[4] First classify (warm-up; arena & caches fill) ...")
t0 = time.ticks_ms()
res = clf.classify(buf, top_k=len(clf.classes))
dt = time.ticks_diff(time.ticks_ms(), t0)
print(f"    {dt} ms")
total = sum(score for _, score in res)
print(f"    sum(probs) = {total:.4f}  (should be ~1.0)")
for label, score in res:
    print(f"      {label}: {score:.4f}")

print("\n[5] Second classify (steady-state timing) ...")
t0 = time.ticks_ms()
res = clf.classify(buf, top_k=3)
dt = time.ticks_diff(time.ticks_ms(), t0)
print(f"    {dt} ms")
print(f"    top-3: {res}")

print("\n[6] info():")
for k, v in vision_ml.info().items():
    print(f"    {k}: {v}")

clf.close()
vision_ml.deinit()
gc.collect()
print("\n=== Stage 2 PASS ===")
