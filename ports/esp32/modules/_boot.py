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

# Auto-start BLE UART REPL for Mixly wireless connectivity
try:
    import mixly_ble

    mixly_ble.start()
except Exception:
    pass
