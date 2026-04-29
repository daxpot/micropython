# vision_ml

ESP32-P4 端图像分类推理模块。配合浏览器训练（lerobot-web 的 ImageClassification 页）→ Mixly 上传 → P4 端 MicroPython 调用的整体流程。

## 架构

```
浏览器训练（V1 0.25 主干，自训练 Dense 头）
   │ 导出 manifest.json + head_weights.bin
   ▼
Mixly 上传到 /models/<name>/
   ▼
P4 端 MicroPython:
   import vision_ml
   vision_ml.init()                                   # 加载主干
   clf = vision_ml.ImageClassifier('/models/fruits/')
   res = clf.classify(rgb_bytes, top_k=3)
```

主干：MobileNetV1 alpha=0.25, 输入 224×224, 截断到 `conv_pw_13_relu`，输出 [1,7,7,256]，TFLM int8 PTQ。
分类头：Dense(256→H, relu) + Dropout(忽略) + Dense(H→N, softmax)，float32 在 PSRAM。

## 文件存放

| 内容 | 位置 | 烧录方式 |
|------|------|---------|
| 主干 .tflite (~480KB) | partition `ml_model` (0x40 @ 0xF00000, 1MB) | esptool 一次性 |
| 学生模型 manifest.json | vfs `/models/<name>/manifest.json` | Mixly 上传 |
| 学生模型 head_weights.bin | vfs `/models/<name>/head_weights.bin` | Mixly 上传 |

## Python API

```python
import vision_ml
import csi_camera
import mp_jpeg

vision_ml.init()                                # 一次性加载主干
clf = vision_ml.ImageClassifier('/models/fruits/')
print(clf.classes)                              # ['苹果','香蕉','橘子']

cam = csi_camera.CSICamera(h_res=224, v_res=224); cam.init()
dec = mp_jpeg.Decoder(...)
jpg = cam.capture()
rgb = dec.decode(jpg)                           # 224*224*3 = 150528 字节 RGB888

result = clf.classify(rgb, top_k=3)
for label, score in result:
    print(label, score)

clf.close()
vision_ml.deinit()

print(vision_ml.info())  # {initialized, arena_used, arena_size, feature_dim, input_size}
```

异常：
- `vision_ml.init()` → `OSError`：partition 缺失/损坏 / 主干 schema 不匹配 / arena 分配失败
- `ImageClassifier(path)` → `OSError`（找不到文件）/ `ValueError`（manifest 不匹配主干 / 权重大小不对）
- `clf.classify(rgb, ...)` → `ValueError`（rgb 长度不是 150528）/ `OSError`（推理失败）

## 烧录主干

1. 跑量化脚本生成 `backbone.tflite`（确保 ≤ 1MB）：
   ```bash
   pip install tensorflow numpy pillow
   python tools/build_backbone.py --calib-dir /path/to/sample/images --out backbone.tflite
   ```
   不传 `--calib-dir` 会用随机数据校准，仅用于冒烟测试。

2. 烧到 `ml_model` partition：
   ```bash
   esptool.py --chip esp32p4 -b 460800 write_flash 0xF00000 backbone.tflite
   ```

## Partition 改动

`ports/esp32/partitions-16MiB-custom.csv` 已经把 `model` 从 4MB 缩到 3MB 并新增 `ml_model` 1MB：
```
model,      data, spiffs,  0xC00000,  0x300000,
ml_model,   data, 0x40,    0xF00000,  0x100000,
```

⚠️ 副作用：multinet 的 `model` SPIFFS 镜像现在只能放 3MB。如果 esp-sr 模型镜像超 3MB，第一次烧固件会失败，需要重新生成精简的镜像。
⚠️ 重刷 partition 表会清掉 vfs，请提前备份 `/models/`。

## 内存预算（运行时）

| 项 | 大小 | 位置 |
|---|---|---|
| backbone .tflite | mmap 在 flash | flash, 不占 RAM |
| TFLM tensor arena | 1MB（兜底） | PSRAM |
| 输入 buffer（rgb） | ~150KB | 调用者持有 |
| Head 权重 + scratch | ~70KB | PSRAM |

第一次推理后查 `vision_ml.info()['arena_used']`，之后可以把 `TENSOR_ARENA_SIZE` 调小。

## manifest.json 格式（前端导出，必须严格匹配）

```json
{
  "version": 1,
  "task": "image_classification",
  "backbone": {
    "name": "mobilenet_v1_0.25_224",
    "input_size": 224,
    "input_norm": "minus1_to_1",
    "feature_extract": "conv_pw_13_relu + GAP",
    "feature_dim": 256
  },
  "head": {
    "type": "dense_softmax",
    "layers": [
      {"name":"fc1","in":256,"out":128,"activation":"relu"},
      {"name":"dropout","rate":0.5},
      {"name":"fc2","in":128,"out":N,"activation":"softmax"}
    ],
    "weights_layout": [
      {"name":"fc1.W","shape":[256,128],"dtype":"float32","offset":0,"bytes":131072},
      {"name":"fc1.b","shape":[128],"dtype":"float32","offset":131072,"bytes":512},
      {"name":"fc2.W","shape":[128,N],"dtype":"float32","offset":131584,"bytes":512*N},
      {"name":"fc2.b","shape":[N],"dtype":"float32","offset":...,"bytes":4*N}
    ]
  },
  "classes": ["类1","类2",...],
  "trained_at": "ISO-8601",
  "train_id": "...",
  "train_name": "..."
}
```

`head_weights.bin`：float32 little-endian，按 `weights_layout` 顺序拼接（fc1.W → fc1.b → fc2.W → fc2.b），无 header。

## 限制（v1）

- 只支持 `dense_softmax` 头，固定两层 Dense（fc1 + fc2）；其它结构需扩展 `vision_ml_head.c`。
- 类别数 ≤ 32。
- 隐层维度由 manifest 决定，无范围检查（建议 64-256）。
- 没有流式 API；每次 `classify` 都跑一次完整主干。
