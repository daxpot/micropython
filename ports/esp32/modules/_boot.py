import gc
import vfs
from flashbdev import bdev

try:
    if bdev:
        vfs.mount(bdev, "/")
except OSError:
    import inisetup

    inisetup.setup()

gc.collect()

# Mixly BLE: start if enabled in config, monitor BOOT button for double-click
# toggle during the first few seconds (non-blocking; runs in background thread).
try:
    import mixly_ble_boot

    mixly_ble_boot.run()
except Exception:
    pass
