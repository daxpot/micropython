# espsrmod — ESP-SR Wake Word Detection (Feed Mode)

MicroPython C module for wake word detection using Espressif's esp-sr library.

**Feed mode**: Python reads PCM audio via `machine.I2S` and feeds it to this
module for detection. No I2S management in C — Python has full control over
audio hardware.

## Supported Platforms

- ESP32-S3 (with PSRAM)
- ESP32-P4 (with PSRAM)

## Quick Start

```python
import espsrmod
from machine import I2S, Pin

# Initialize I2S microphone (you control this)
audio_in = I2S(0, sck=Pin(42), ws=Pin(41), sd=Pin(2),
               mode=I2S.PDM_RX, bits=16, format=I2S.MONO,
               rate=16000, ibuf=15360)

# Create WakeNet detector
wn = espsrmod.WakeNet()
print(f"Wake word: {wn.wake_word}")
print(f"Chunk: {wn.chunk_samples} samples, Rate: {wn.sample_rate} Hz")

# Detection loop
buf = bytearray(960)  # Read buffer
while True:
    n = audio_in.readinto(buf)
    if n > 0:
        result = wn.detect(buf)
        if result:
            print(f"Detected: {result}")
            break

# I2S stays alive — switch to recording/streaming without re-init
```

## API Reference

### `espsrmod.WakeNet()`

Create a WakeNet detector instance (singleton — only one at a time).

Loads the wake word model from the `model` flash partition on creation.

### Properties (read-only)

| Property | Type | Description |
|---|---|---|
| `chunk_samples` | int | Samples per WakeNet detection chunk (e.g. 480) |
| `chunk_bytes` | int | `chunk_samples × 2` (convenience) |
| `sample_rate` | int | Expected sample rate in Hz (e.g. 16000) |
| `wake_word` | str | Configured wake word name |

### Methods

#### `detect(pcm_data) → str | None`

Feed PCM audio data for wake word detection.

- **pcm_data**: `bytes` or `bytearray` containing 16-bit signed mono PCM samples
- **Returns**: Wake word name string if detected, `None` otherwise

Internally buffers data until a full WakeNet chunk is available, then runs
detection. A single call may process multiple chunks if enough data is provided.

#### `clear()`

Reset the internal accumulation buffer. Call this when discarding audio
continuity (e.g. after a long pause in recording).

#### `set_threshold(threshold)`

Set detection sensitivity.

- **threshold**: `float` between 0.5 and 0.99. Higher = fewer false positives.
  Default is 0.9.

#### `deinit()`

Release all resources. After this, create a new `WakeNet()` instance to
restart detection.

## Integration with esp32_client.py

```python
import espsrmod

class XiaoZhiClient:
    def __init__(self):
        self.wn = espsrmod.WakeNet()
        # ... other init ...

    async def wake_task(self):
        """Detect wake word during 'wait' state."""
        buf = bytearray(self.pcm_frame_bytes)
        while True:
            if self.status == 'wait':
                self.init_audio_in()  # Ensure I2S is active
                n = self.audio_in.readinto(buf)
                if n > 0:
                    result = self.wn.detect(buf)
                    if result:
                        self.wn.clear()
                        await self.send_wake_text(result)
            else:
                self.wn.clear()  # Discard stale buffer on state change
            await asyncio.sleep_ms(0)
```

## Build

```bash
cd ports/esp32
make BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=SPIRAM_OCT \
     USER_C_MODULES=../../../modules all
```

Ensure `sdkconfig` includes the esp-sr wake word model configuration
(see `sdkconfig.espsrmod` in the old `tmp/espsrmod/` for reference).

## Requirements

- esp-sr managed component (`espressif__esp-sr`)
- `model` flash partition with WakeNet model data
- PSRAM enabled
