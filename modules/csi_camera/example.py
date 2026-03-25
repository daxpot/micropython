"""
csi_camera 模块完整示例
适用于 ESP32-P4 + OV5647 MIPI CSI 摄像头

所有接口及参数演示
"""

from csi_camera import CSICamera
import time

# ============================================================
# 1. 创建摄像头对象 — 所有构造参数
# ============================================================
cam = CSICamera(
    h_res=320,          # 目标水平分辨率 (默认800)，小于传感器原始分辨率时自动居中裁剪
    v_res=240,          # 目标垂直分辨率 (默认640)
    jpeg_quality=80,    # JPEG 压缩质量 1~100 (默认80)，越高画质越好、文件越大
    data_lanes=2,       # MIPI CSI 数据通道数 1 或 2 (默认2)
    lane_bitrate=200,   # 通道比特率 Mbps (默认200)
    sccb_sda=-1,        # SCCB/I2C SDA 引脚，-1 使用默认 GPIO7
    sccb_scl=-1,        # SCCB/I2C SCL 引脚，-1 使用默认 GPIO8
)

# ============================================================
# 2. 初始化摄像头
# ============================================================
# 首次调用会初始化全部硬件 (LDO → 传感器 → CSI → ISP → JPEG)
# 脚本重新运行时再次调用 init()，会自动复用已有硬件，不会报错
cam.init()
print("摄像头已初始化")

# ============================================================
# 3. 查询分辨率
# ============================================================
print("输出分辨率:", cam.resolution())              # (320, 240) — 实际输出的 JPEG 尺寸
print("传感器原始分辨率:", cam.sensor_resolution())  # (800, 640) — 传感器硬件输出
print("是否已初始化:", cam.initialized())            # True

# ============================================================
# 4. 基本拍照
# ============================================================
img = cam.capture()                  # 返回 bytes，内容为 JPEG 图片
print("JPEG 大小:", len(img), "bytes")

# 保存到文件
with open("/photo.jpg", "wb") as f:
    f.write(img)
print("已保存 /photo.jpg")

# ============================================================
# 5. ISP 色彩调节 — set_color()
# ============================================================

# --- 亮度 (brightness) ---
# 范围: -128 ~ 127，默认 0
# 负值变暗，正值变亮
cam.set_color(brightness=30)         # 稍微增亮
img_bright = cam.capture()
with open("/bright.jpg", "wb") as f:
    f.write(img_bright)

cam.set_color(brightness=-30)        # 稍微变暗
img_dark = cam.capture()
with open("/dark.jpg", "wb") as f:
    f.write(img_dark)

# --- 对比度 (contrast) ---
# 范围: 0 ~ 255，默认 128 (1.0x)
# 0 = 无对比度 (灰色)，128 = 正常，255 = 最高对比度 (~2.0x)
cam.set_color(brightness=0, contrast=200)   # 高对比度
img_contrast = cam.capture()
with open("/high_contrast.jpg", "wb") as f:
    f.write(img_contrast)

# --- 饱和度 (saturation) ---
# 范围: 0 ~ 255，默认 128 (1.0x)
# 0 = 黑白，128 = 正常，255 = 超高饱和度 (~2.0x)
cam.set_color(brightness=0, contrast=128, saturation=0)     # 黑白模式
img_bw = cam.capture()
with open("/blackwhite.jpg", "wb") as f:
    f.write(img_bw)

cam.set_color(brightness=0, contrast=128, saturation=200)   # 高饱和度
img_vivid = cam.capture()
with open("/vivid.jpg", "wb") as f:
    f.write(img_vivid)

# --- 色相 (hue) ---
# 范围: 0 ~ 360 度，默认 0
# 基于色轮旋转颜色：0°=红, 120°=绿, 240°=蓝, 360°=红
cam.set_color(brightness=0, contrast=128, saturation=128, hue=180)  # 色相旋转 180°
img_hue = cam.capture()
with open("/hue180.jpg", "wb") as f:
    f.write(img_hue)

# --- 恢复默认色彩 ---
cam.set_color(brightness=0, contrast=128, saturation=128, hue=0)
print("色彩已恢复默认")

# ============================================================
# 6. 画面翻转 — set_mirror_flip()
# ============================================================

# --- 水平镜像 (hmirror) ---
# True = 水平翻转，False = 正常（默认）
cam.set_mirror_flip(hmirror=True, vflip=False)
img_mirror = cam.capture()
with open("/mirror.jpg", "wb") as f:
    f.write(img_mirror)
print("水平镜像")

# --- 垂直翻转 (vflip) ---
# True = 上下翻转，False = 正常（默认）
cam.set_mirror_flip(hmirror=False, vflip=True)
img_flip = cam.capture()
with open("/flip.jpg", "wb") as f:
    f.write(img_flip)
print("垂直翻转")

# --- 同时镜像 + 翻转（旋转 180°效果）---
cam.set_mirror_flip(hmirror=True, vflip=True)
img_rotate = cam.capture()
with open("/rotate180.jpg", "wb") as f:
    f.write(img_rotate)
print("旋转180°")

# --- 恢复正常方向 ---
cam.set_mirror_flip(hmirror=False, vflip=False)
print("方向已恢复")

# ============================================================
# 7. 连续拍照示例
# ============================================================
print("\n连续拍照 5 张:")
for i in range(5):
    img = cam.capture()
    print(f"  第 {i+1} 张: {len(img)} bytes")
    time.sleep_ms(100)

# ============================================================
# 8. 释放缓冲区（可选）
# ============================================================
# 在内存紧张时，可以手动释放内部 JPEG 缓冲区标记
cam.free_buffer()

# ============================================================
# 9. 反初始化（可选）
# ============================================================
# 调用 deinit() 会释放所有硬件资源（LDO、I2C、CSI、ISP、JPEG、缓冲区）
# 不调用也可以 —— 下次 init() 会自动复用已有硬件
cam.deinit()
print("摄像头已释放")

# ============================================================
# 10. 不同分辨率示例
# ============================================================

# 全分辨率 (传感器原始输出，无裁剪)
cam_full = CSICamera(h_res=800, v_res=640, jpeg_quality=90)
cam_full.init()
img_full = cam_full.capture()
print(f"\n全分辨率 800x640: {len(img_full)} bytes")

# 正方形裁剪 (适合 240x240 ST7789 屏幕)
cam_sq = CSICamera(h_res=240, v_res=240, jpeg_quality=80)
cam_sq.init()  # 会自动 deinit 之前的再 init（因为分辨率不同需要新 sensor 格式）
img_sq = cam_sq.capture()
print(f"正方形 240x240: {len(img_sq)} bytes")

# 320x240 裁剪 (适合 320x240 ST7789 屏幕)
cam_lcd = CSICamera(h_res=320, v_res=240, jpeg_quality=80)
cam_lcd.init()
img_lcd = cam_lcd.capture()
print(f"LCD 320x240: {len(img_lcd)} bytes")

cam_lcd.deinit()
print("\n示例完成!")
