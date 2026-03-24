# ESP-SR Wake Word Detection Module for MicroPython

This module provides wake word detection functionality for ESP32-S3 and ESP32-P4 using Espressif's esp-sr library.

## Features

- Wake word detection using WakeNet9 model
- Configurable I2S input (Standard or PDM mode)
- Pause/resume functionality for I2S resource sharing
- Python callback on wake word detection
- Adjustable detection threshold

## Supported Wake Word

Currently configured: **小宇同学** (xiaoyutongxue)

## Usage

```python
import espsr

def on_wakeup(word):
    print("Wake word detected:", word)
    wn.pause()  # Release I2S for other use
    # Now you can initialize your own I2S for recording/playback
    # ...
    # When done, call wn.resume() to restart detection

# Create WakeNet instance (singleton)
wn = espsr.WakeNet(callback=on_wakeup)

# Start detection with I2S configuration
# PDM mode (most MEMS microphones)
wn.resume(i2s_id=0, sck=42, ws=41, sd=2, pdm=True)

# Or Standard I2S mode
# wn.resume(i2s_id=0, sck=42, ws=41, sd=2, pdm=False)

# Optional: adjust detection threshold (0.5 ~ 0.99)
wn.set_threshold(0.85)

# Check if detection is running
print("Running:", wn.is_running())

# Get configured wake word name
print("Wake word:", wn.get_wake_word())

# When done, cleanup
wn.deinit()
```

## API Reference

### `espsr.WakeNet(callback=None)`

Create a WakeNet instance for wake word detection.

**Parameters:**
- `callback`: Function to call when wake word is detected. Receives the wake word name as argument.

**Note:** Only one WakeNet instance can exist at a time (singleton pattern).

### `WakeNet.resume(i2s_id, sck, ws, sd, pdm=True, sample_rate=16000)`

Initialize I2S and start wake word detection.

**Parameters:**
- `i2s_id`: I2S port number (0 or 1)
- `sck`: Serial clock pin (BCLK for STD, CLK for PDM)
- `ws`: Word select pin (LRCK for STD, not used for PDM mono but required)
- `sd`: Serial data input pin
- `pdm`: True for PDM mode, False for Standard I2S mode (default: True)
- `sample_rate`: Audio sample rate in Hz (default: 16000)

### `WakeNet.pause()`

Stop wake word detection and release I2S resources. After calling this, you can use the I2S port for other purposes (e.g., recording, playback).

### `WakeNet.set_threshold(threshold)`

Set the detection threshold.

**Parameters:**
- `threshold`: Detection sensitivity (0.5 ~ 0.99). Higher values reduce false positives but may miss some detections.

### `WakeNet.get_wake_word()`

Get the name of the configured wake word.

**Returns:** Wake word name string (e.g., "xiaoyutongxue")

### `WakeNet.is_running()`

Check if wake word detection is currently active.

**Returns:** True if running, False otherwise

### `WakeNet.set_callback(callback)`

Update the wake word detection callback.

**Parameters:**
- `callback`: New callback function, or None to disable

### `WakeNet.deinit()`

Completely deinitialize the wake word detection system. After calling this, you need to create a new WakeNet instance.

## Build Configuration

### First-time build

1. Configure esp-sr models via menuconfig:
   ```bash
   cd ports/esp32
   idf.py menuconfig
   # Navigate to: ESP Speech Recognition → WakeNet Model
   # Select: wn9_xiaoyutongxue_tts2
   ```

2. Build and flash (includes model partition):
   ```bash
   make BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=SPIRAM_OCT USER_C_MODULES=../../../modules all
   idf.py flash
   ```

### Subsequent builds

After initial flash with models, you can use faster app-only flash:
```bash
make BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=SPIRAM_OCT USER_C_MODULES=../../../modules all
idf.py app-flash
```

## Partition Table

The module requires a `model` partition for storing wake word models. See `partitions-16MiB-custom.csv`:

```csv
# Name,   Type, SubType, Offset,    Size,     Flags
nvs,      data, nvs,     0x9000,    0x6000,
phy_init, data, phy,     0xf000,    0x1000,
factory,  app,  factory, 0x10000,   0x2A0000,
font1,    data, fat,     0x2B0000,  0x60000,
font2,    data, fat,     0x310000,  0x40000,
vfs,      data, fat,     0x350000,  0x8B0000,
model,    data, spiffs,  0xC00000,  0x400000,
```

**Note:** Font partitions have been moved. Update your font flashing addresses:
- font1: `0x3A0000` → `0x2B0000`
- font2: `0x400000` → `0x310000`

## Hardware Requirements

- ESP32-S3 or ESP32-P4 with PSRAM
- MEMS microphone (PDM or I2S)
- 16MB Flash (for model partition)

## License

MIT License
