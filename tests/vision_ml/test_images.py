# test_images.py - classify 4 still images on device
#
# Put images/1.jpg .. images/4.jpg on the device first, e.g.:
#   mpremote cp images/1.jpg :images/1.jpg
#   mpremote cp images/2.jpg :images/2.jpg
#   mpremote cp images/3.jpg :images/3.jpg
#   mpremote cp images/4.jpg :images/4.jpg
#
# Then:  mpremote run tests/vision_ml/test_images.py

import vision_ml
import jpeg
import gc

MODEL_DIR = '/models/fruit/'
IMAGES = ['images/1.jpg', 'images/2.jpg', 'images/3.jpg', 'images/4.jpg']

print("=" * 50)
print("vision_ml: classify still images")
print("=" * 50)

vision_ml.init()
clf = vision_ml.ImageClassifier(MODEL_DIR)
print("classes:", clf.classes)
print()

dec = jpeg.Decoder(
    pixel_format='RGB888',
    clipper_width=224,
    clipper_height=224,
    return_bytes=True,
)

for path in IMAGES:
    try:
        with open(path, 'rb') as f:
            jpg = f.read()
    except OSError as e:
        print("%s: OPEN FAILED (%s)" % (path, e))
        continue

    try:
        rgb = dec.decode(jpg)
    except Exception as e:
        print("%s: DECODE FAILED (%s)" % (path, e))
        continue

    if len(rgb) != vision_ml.INPUT_BYTES:
        print("%s: WRONG SIZE %d (expected %d)" % (path, len(rgb), vision_ml.INPUT_BYTES))
        continue

    res = clf.classify(rgb, top_k=3)
    print("%s ->" % path)
    for label, score in res:
        print("    %-12s  %.3f" % (label, score))
    print()
    gc.collect()

clf.close()
vision_ml.deinit()
print("Done.")
