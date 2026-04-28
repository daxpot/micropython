"""Mixly BLE boot helper.

Reads a persisted enable flag and optionally starts the BLE UART REPL on boot.
During the first DETECT_WINDOW_MS milliseconds after boot, monitors the BOOT
button for a double-click that toggles the enable flag at runtime (no reboot
required) and gives a brief LED blink feedback.
"""

import _thread
import time
from machine import Pin

# --- Configurable constants -------------------------------------------------
# LED indicator pins. Add more entries for boards with different LED GPIOs;
# every listed pin is driven in lockstep so a single firmware works on
# multiple boards.
LED_PINS = [2, 13, 15, 38, 45, 47]
LED_ACTIVE_HIGH = True

BOOT_BTN_PIN = 0

DETECT_WINDOW_MS = 5000
DOUBLE_CLICK_GAP_MS = 500
DEBOUNCE_MS = 30

FAST_BLINK_MS = 100
SLOW_BLINK_MS = 500
BLINK_DURATION_MS = 2000

CONFIG_PATH = "/mixly_ble.cfg"


def _read_enabled():
    try:
        with open(CONFIG_PATH, "rb") as f:
            return f.read(1) == b"1"
    except OSError:
        return False


def _write_enabled(enabled):
    try:
        with open(CONFIG_PATH, "wb") as f:
            f.write(b"1" if enabled else b"0")
    except OSError:
        pass


def _init_leds():
    leds = []
    for gpio in LED_PINS:
        try:
            leds.append(Pin(gpio, Pin.OUT))
        except Exception:
            pass
    return leds


def _set_leds(leds, on):
    level = 1 if (on == LED_ACTIVE_HIGH) else 0
    for led in leds:
        try:
            led.value(level)
        except Exception:
            pass


def _blink(fast):
    period_ms = FAST_BLINK_MS if fast else SLOW_BLINK_MS
    half = period_ms // 2 if period_ms >= 2 else 1
    leds = _init_leds()
    if not leds:
        return
    end = time.ticks_add(time.ticks_ms(), BLINK_DURATION_MS)
    state = False
    try:
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            state = not state
            _set_leds(leds, state)
            time.sleep_ms(half)
    finally:
        _set_leds(leds, False)


def _toggle(current_enabled):
    new_enabled = not current_enabled
    try:
        import mixly_ble

        if new_enabled:
            mixly_ble.start()
        else:
            mixly_ble.stop()
    except Exception:
        pass
    _write_enabled(new_enabled)
    _blink(new_enabled)


def _wait_press(btn, deadline):
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if btn.value() == 0:
            time.sleep_ms(DEBOUNCE_MS)
            if btn.value() == 0:
                return True
        time.sleep_ms(10)
    return False


def _wait_release(btn, deadline):
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if btn.value() == 1:
            time.sleep_ms(DEBOUNCE_MS)
            if btn.value() == 1:
                return True
        time.sleep_ms(10)
    return False


def _monitor(initial_enabled):
    try:
        btn = Pin(BOOT_BTN_PIN, Pin.IN, Pin.PULL_UP)
    except Exception:
        return

    window_end = time.ticks_add(time.ticks_ms(), DETECT_WINDOW_MS)
    enabled = initial_enabled

    while time.ticks_diff(window_end, time.ticks_ms()) > 0:
        if not _wait_press(btn, window_end):
            return
        if not _wait_release(btn, window_end):
            return
        gap_deadline = time.ticks_add(time.ticks_ms(), DOUBLE_CLICK_GAP_MS)
        if time.ticks_diff(gap_deadline, window_end) > 0:
            gap_deadline = window_end
        if _wait_press(btn, gap_deadline):
            _wait_release(btn, time.ticks_add(time.ticks_ms(), 1000))
            _toggle(enabled)
            enabled = not enabled
            return


def run():
    enabled = _read_enabled()
    if enabled:
        try:
            import mixly_ble

            mixly_ble.start()
        except Exception:
            pass
    try:
        _thread.start_new_thread(_monitor, (enabled,))
    except Exception:
        pass
