# test_camera_e2e.py - Stage 3: camera + JPEG decode + classify
#
# Prereq:
#   - real model uploaded to /models/<name>/ (manifest.json + head_weights.bin)
#   - csi_camera and jpeg modules available
#
# Adjust MODEL_DIR below to match your uploaded model.

import time
import vision_ml
import csi_camera
import jpeg
import gc

MODEL_DIR = '/models/fruit/'

# Camera output resolution. Many CSI sensors (e.g. OV5647) won't go down to
# 224x224 directly; capture at a native size and let the JPEG decoder clip
# to 224x224. If your sensor supports 224x224, set CAM_W/CAM_H to 224.
CAM_W, CAM_H = 800, 640

print("=" * 50)
print("vision_ml Stage 3: camera end-to-end")
print("=" * 50)

vision_ml.init()
clf = vision_ml.ImageClassifier(MODEL_DIR)
print("classes:", clf.classes)

cam = csi_camera.CSICamera(h_res=CAM_W, v_res=CAM_H, jpeg_quality=85)
cam.init()

# JPEG decoder: output RGB888 cropped to 224x224, return raw bytes.
# clipper_* crops the center region to the given size.
dec = jpeg.Decoder(
    pixel_format='RGB888',
    clipper_width=224,
    clipper_height=224,
    return_bytes=True,
)

print("\nLooping (Ctrl-C to stop):\n")
try:
    while True:
        t0 = time.ticks_ms()
        try:
            jpg = cam.capture()
        except OSError as e:
            # Transient CSI timeouts (errno 263 = ESP_ERR_TIMEOUT) happen
            # occasionally on the P4 CSI driver. Just retry the next loop.
            print("capture retry:", e)
            time.sleep_ms(50)
            continue
        t1 = time.ticks_ms()
        rgb = dec.decode(jpg)
        cam.free_buffer()
        t2 = time.ticks_ms()

        if len(rgb) != vision_ml.INPUT_BYTES:
            print("WARN:", len(rgb), "bytes, expected", vision_ml.INPUT_BYTES,
                  "- tune clipper or sensor resolution.")
            time.sleep_ms(500)
            continue

        res = clf.classify(rgb, top_k=3)
        t3 = time.ticks_ms()

        cap_ms = time.ticks_diff(t1, t0)
        dec_ms = time.ticks_diff(t2, t1)
        inf_ms = time.ticks_diff(t3, t2)
        total  = time.ticks_diff(t3, t0)

        top1 = res[0]
        print("cap=%dms dec=%dms inf=%dms tot=%dms => %s (%.3f)" % (
            cap_ms, dec_ms, inf_ms, total, top1[0], top1[1]))
        gc.collect()
except KeyboardInterrupt:
    pass

cam.deinit()
clf.close()
vision_ml.deinit()
print("\nStopped.")
