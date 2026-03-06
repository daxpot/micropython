# opusmod2 - MicroPython Opus Module (esp-opus-encoder backend)

基于 [78/esp-opus-encoder](https://github.com/78/esp-opus-encoder) 的 MicroPython Opus 编解码模块。

相比 opusmod（使用 libopus 1.5.2 直接编译），opusmod2 使用虾哥优化过的 esp-opus 库，
底层包含 Xtensa LX7 优化，默认 complexity=0，编码速度显著提升。

## 依赖

- ESP-IDF >= 5.3
- `78/esp-opus-encoder ^2.4.1`（自动拉取 `78/esp-opus ^1.0.5`）

## Python API

```python
import opusmod2

# ===== 编码器 =====
enc = opusmod2.Encoder(
    sample_rate=16000,   # 8000/16000/24000/48000
    channels=1,          # 1 或 2
    duration_ms=60,      # 帧时长(ms)，默认60
    complexity=0,        # 0-10，越低越快，默认0
    dtx=True,            # 不连续传输，默认开启
    bitrate=32000        # 码率(bps)，默认0(自动)
)

# 编码一帧 PCM (bytes 长度必须 = frame_size * 2)
opus_bytes = enc.encode(pcm_bytes)

# 控制方法
enc.set_complexity(3)
enc.set_dtx(False)
enc.reset()

# 属性
print(enc.frame_size)    # int16_t 采样点数
print(enc.sample_rate)
print(enc.channels)
print(enc.duration_ms)

# ===== 解码器 =====
dec = opusmod2.Decoder(
    sample_rate=16000,
    channels=1,
    duration_ms=60
)

pcm_bytes, samples = dec.decode(opus_bytes)
dec.reset()

# ===== 重采样器 =====
res = opusmod2.Resampler(input_rate=48000, output_rate=16000)
out_pcm = res.process(in_pcm_bytes)
```

## 与 opusmod 的区别

| 特性 | opusmod | opusmod2 |
|------|---------|----------|
| Opus 后端 | libopus 1.5.2 (直接编译) | 78/esp-opus (ESP-IDF组件) |
| 平台优化 | 无 (DISABLE_OPTIMIZATIONS) | Xtensa LX7 优化 |
| 默认 complexity | 未设置 | 0 (最快) |
| DTX | 不支持 | ✅ |
| 复杂度控制 | 不支持 | ✅ set_complexity() |
| 重采样 | 不支持 | ✅ Resampler |
| 析构函数 | ❌ 无 | ✅ __del__ |
| 帧参数 | frame_size (samples) | duration_ms (毫秒) |
| 默认 Application | AUDIO | VOIP |
