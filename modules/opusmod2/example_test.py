"""
opusmod2 测试示例 - 对比编码性能
使用 78/esp-opus-encoder 后端

测试内容：
1. 编码性能测试（计时）
2. 编码+解码往返测试
3. 重采样测试
4. 与 opusmod (libopus 1.5.2) 性能对比
"""

import opusmod2
import time
import array
import math


def generate_sine_pcm(sample_rate, channels, duration_ms, frequency=1000):
    """生成正弦波 PCM 测试数据"""
    num_samples = sample_rate * duration_ms // 1000
    pcm = array.array('h', [0] * (num_samples * channels))
    for i in range(num_samples):
        val = int(30000 * math.sin(2 * math.pi * frequency * i / sample_rate))
        for ch in range(channels):
            pcm[i * channels + ch] = val
    return pcm


def test_encoder_performance():
    """测试编码性能"""
    print("=" * 50)
    print("opusmod2 编码性能测试")
    print("=" * 50)

    sample_rate = 16000
    channels = 1
    duration_ms = 60  # 每帧 60ms

    # 创建编码器 (complexity=0 最快, DTX开启)
    enc = opusmod2.Encoder(
        sample_rate=sample_rate,
        channels=channels,
        duration_ms=duration_ms,
        complexity=0,
        dtx=True
    )

    print(f"采样率: {enc.sample_rate} Hz")
    print(f"通道数: {enc.channels}")
    print(f"帧时长: {enc.duration_ms} ms")
    print(f"帧大小: {enc.frame_size} samples")
    print()

    # 生成测试 PCM 数据 (1秒)
    test_duration_sec = 1
    total_frames = test_duration_sec * 1000 // duration_ms
    pcm = generate_sine_pcm(sample_rate, channels, duration_ms)
    pcm_bytes = pcm.tobytes()

    # 预热
    enc.encode(pcm_bytes)
    enc.reset()

    # 计时编码
    opus_frames = []
    start = time.ticks_us()
    for i in range(total_frames):
        opus_data = enc.encode(pcm_bytes)
        opus_frames.append(opus_data)
    end = time.ticks_us()

    total_us = time.ticks_diff(end, start)
    avg_us = total_us // total_frames
    audio_ms = total_frames * duration_ms

    print(f"编码 {total_frames} 帧 ({audio_ms}ms 音频):")
    print(f"  总耗时: {total_us} us ({total_us / 1000:.1f} ms)")
    print(f"  平均每帧: {avg_us} us ({avg_us / 1000:.1f} ms)")
    print(f"  实时比: {avg_us / 1000 / duration_ms:.2f}x (< 1.0 = 实时)")
    print(f"  Opus 帧大小: {[len(f) for f in opus_frames[:5]]}... bytes")
    print()

    return opus_frames


def test_decoder_performance(opus_frames):
    """测试解码性能"""
    print("=" * 50)
    print("opusmod2 解码性能测试")
    print("=" * 50)

    sample_rate = 16000
    channels = 1
    duration_ms = 60

    dec = opusmod2.Decoder(
        sample_rate=sample_rate,
        channels=channels,
        duration_ms=duration_ms
    )

    print(f"采样率: {dec.sample_rate} Hz")
    print(f"帧大小: {dec.frame_size} samples")
    print()

    # 预热
    dec.decode(opus_frames[0])
    dec.reset()

    # 计时解码
    total_samples = 0
    start = time.ticks_us()
    for opus_data in opus_frames:
        pcm_bytes, samples = dec.decode(opus_data)
        total_samples += samples
    end = time.ticks_us()

    total_us = time.ticks_diff(end, start)
    avg_us = total_us // len(opus_frames)

    print(f"解码 {len(opus_frames)} 帧:")
    print(f"  总耗时: {total_us} us ({total_us / 1000:.1f} ms)")
    print(f"  平均每帧: {avg_us} us ({avg_us / 1000:.1f} ms)")
    print(f"  实时比: {avg_us / 1000 / duration_ms:.2f}x (< 1.0 = 实时)")
    print(f"  总解码样本: {total_samples}")
    print()


def test_complexity_comparison():
    """测试不同 complexity 下的编码速度"""
    print("=" * 50)
    print("complexity 对比测试")
    print("=" * 50)

    sample_rate = 16000
    channels = 1
    duration_ms = 60
    num_frames = 10

    pcm = generate_sine_pcm(sample_rate, channels, duration_ms)
    pcm_bytes = pcm.tobytes()

    for complexity in [0, 1, 2, 3, 5, 10]:
        enc = opusmod2.Encoder(
            sample_rate=sample_rate,
            channels=channels,
            duration_ms=duration_ms,
            complexity=complexity,
            dtx=True
        )

        # 预热
        enc.encode(pcm_bytes)
        enc.reset()

        start = time.ticks_us()
        for i in range(num_frames):
            enc.encode(pcm_bytes)
        end = time.ticks_us()

        avg_us = time.ticks_diff(end, start) // num_frames
        print(f"  complexity={complexity:2d}: {avg_us:6d} us/帧 ({avg_us / 1000:.1f} ms)")

    print()


def test_resampler():
    """测试重采样功能"""
    print("=" * 50)
    print("重采样测试")
    print("=" * 50)

    # 48kHz -> 16kHz
    res = opusmod2.Resampler(input_rate=48000, output_rate=16000)
    print(f"输入采样率: {res.input_rate} Hz")
    print(f"输出采样率: {res.output_rate} Hz")

    # 生成 48kHz 的 60ms 测试数据
    pcm_48k = generate_sine_pcm(48000, 1, 60)
    pcm_bytes = pcm_48k.tobytes()

    start = time.ticks_us()
    out_bytes = res.process(pcm_bytes)
    end = time.ticks_us()

    in_samples = len(pcm_48k)
    out_samples = len(out_bytes) // 2  # int16_t = 2 bytes

    print(f"输入: {in_samples} samples ({in_samples * 1000 / 48000:.1f} ms)")
    print(f"输出: {out_samples} samples ({out_samples * 1000 / 16000:.1f} ms)")
    print(f"耗时: {time.ticks_diff(end, start)} us")
    print()


def test_compare_with_opusmod():
    """与原版 opusmod 对比（如果可用）"""
    print("=" * 50)
    print("opusmod vs opusmod2 对比")
    print("=" * 50)

    sample_rate = 16000  # 使用 16kHz 对比
    channels = 1
    num_frames = 10

    # opusmod 使用 frame_size 作为样本数参数
    # opusmod2 使用 duration_ms
    # 16000Hz * 60ms = 960 samples
    frame_size = 960
    duration_ms = 60

    pcm = generate_sine_pcm(sample_rate, channels, duration_ms)
    pcm_bytes = pcm.tobytes()

    # Test opusmod2
    enc2 = opusmod2.Encoder(
        sample_rate=sample_rate,
        channels=channels,
        duration_ms=duration_ms,
        complexity=0,
        dtx=True
    )
    enc2.encode(pcm_bytes)  # warmup
    enc2.reset()

    start = time.ticks_us()
    for i in range(num_frames):
        enc2.encode(pcm_bytes)
    end = time.ticks_us()
    avg_mod2 = time.ticks_diff(end, start) // num_frames
    print(f"opusmod2 (esp-opus-encoder, complexity=0): {avg_mod2} us/帧 ({avg_mod2/1000:.1f} ms)")

    # Test opusmod (if available)
    try:
        import opusmod
        enc1 = opusmod.OpusEncoder(sample_rate, channels, opusmod.APPLICATION_VOIP, 32000)
        enc1.encode(pcm_bytes, frame_size)  # warmup

        start = time.ticks_us()
        for i in range(num_frames):
            enc1.encode(pcm_bytes, frame_size)
        end = time.ticks_us()
        avg_mod1 = time.ticks_diff(end, start) // num_frames
        print(f"opusmod  (libopus 1.5.2):                 {avg_mod1} us/帧 ({avg_mod1/1000:.1f} ms)")
        print(f"加速比: {avg_mod1 / avg_mod2:.2f}x")
    except ImportError:
        print("opusmod 不可用，跳过对比")

    print()


def test_roundtrip():
    """编码+解码往返验证"""
    print("=" * 50)
    print("编码/解码往返测试")
    print("=" * 50)

    sample_rate = 16000
    channels = 1
    duration_ms = 60

    enc = opusmod2.Encoder(sample_rate=sample_rate, channels=channels,
                           duration_ms=duration_ms, complexity=0)
    dec = opusmod2.Decoder(sample_rate=sample_rate, channels=channels,
                           duration_ms=duration_ms)

    pcm = generate_sine_pcm(sample_rate, channels, duration_ms)
    pcm_bytes = pcm.tobytes()

    # 编码
    opus_data = enc.encode(pcm_bytes)
    print(f"原始 PCM: {len(pcm_bytes)} bytes")
    print(f"Opus 编码: {len(opus_data)} bytes")
    print(f"压缩比: {len(pcm_bytes) / len(opus_data):.1f}x")

    # 解码
    pcm_out, samples = dec.decode(opus_data)
    print(f"解码 PCM: {len(pcm_out)} bytes, {samples} samples")
    print(f"数据完整: {'✓' if len(pcm_out) == len(pcm_bytes) else '✗'}")
    print()


# ===== 运行所有测试 =====
if __name__ == "__main__" or True:
    print()
    print("★★★ opusmod2 性能测试 (esp-opus-encoder backend) ★★★")
    print()

    test_roundtrip()
    opus_frames = test_encoder_performance()
    test_decoder_performance(opus_frames)
    test_complexity_comparison()
    test_resampler()
    test_compare_with_opusmod()

    print("全部测试完成!")
