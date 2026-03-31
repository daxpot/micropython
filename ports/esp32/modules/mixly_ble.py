"""
mixly_ble - BLE UART REPL service for Mixly Web Bluetooth

Provides a BLE UART service using Mixly-compatible UUIDs (0xFFF0/FFF1/FFF2).
When started, adds the BLE stream as a secondary REPL via os.dupterm(),
allowing code upload and serial monitoring over Bluetooth while keeping
USB serial fully functional.

Device name format: "Mixly-XXXXXXXXXXXX" (6-byte MAC in hex)

Usage:
    import mixly_ble
    mixly_ble.start()        # Auto-named from MAC
    mixly_ble.start("MyDev") # Custom name
    mixly_ble.stop()         # Stop BLE REPL
"""

import bluetooth
import struct
import io
import os
import machine
import micropython
from micropython import const


_IRQ_CENTRAL_CONNECT = const(1)
_IRQ_CENTRAL_DISCONNECT = const(2)
_IRQ_GATTS_WRITE = const(3)

_FLAG_WRITE = const(0x0008)
_FLAG_WRITE_NO_RESPONSE = const(0x0004)
_FLAG_NOTIFY = const(0x0010)

# UUIDs matching Mixly Web bluetooth.js
_SERVICE_UUID = bluetooth.UUID(0xFFF0)
_TX_CHAR = (bluetooth.UUID(0xFFF1), _FLAG_NOTIFY)
_RX_CHAR = (bluetooth.UUID(0xFFF2), _FLAG_WRITE | _FLAG_WRITE_NO_RESPONSE)
_UART_SERVICE = (_SERVICE_UUID, (_TX_CHAR, _RX_CHAR))

_ADV_TYPE_FLAGS = const(0x01)
_ADV_TYPE_NAME = const(0x09)
_ADV_TYPE_UUID16_COMPLETE = const(0x03)

_MP_STREAM_POLL = const(3)
_MP_STREAM_POLL_RD = const(0x0001)

_FLUSH_INTERVAL_MS = const(50)
_FLUSH_CHUNK_SIZE = const(100)


class _BLEUART:
    def __init__(self, ble, name, rxbuf=256):
        self._ble = ble
        self._ble.active(True)
        self._ble.config(mtu=256, gap_name=name)
        self._ble.irq(self._irq)
        ((self._tx_handle, self._rx_handle),) = self._ble.gatts_register_services(
            (_UART_SERVICE,)
        )
        self._ble.gatts_set_buffer(self._rx_handle, rxbuf, True)
        self._connections = set()
        self._rx_buffer = bytearray()
        self._handler = None
        self._payload = self._make_adv_payload(name)
        self._advertise()

    @staticmethod
    def _make_adv_payload(name):
        payload = bytearray()

        def _append(adv_type, value):
            nonlocal payload
            payload += struct.pack("BB", len(value) + 1, adv_type) + value

        _append(_ADV_TYPE_FLAGS, struct.pack("B", 0x06))
        _append(_ADV_TYPE_NAME, name.encode())
        _append(_ADV_TYPE_UUID16_COMPLETE, struct.pack("<H", 0xFFF0))
        return payload

    def irq(self, handler):
        self._handler = handler

    def _irq(self, event, data):
        if event == _IRQ_CENTRAL_CONNECT:
            conn_handle, _, _ = data
            self._connections.add(conn_handle)
        elif event == _IRQ_CENTRAL_DISCONNECT:
            conn_handle, _, _ = data
            self._connections.discard(conn_handle)
            try:
                self._advertise()
            except Exception:
                pass
        elif event == _IRQ_GATTS_WRITE:
            conn_handle, value_handle = data
            if value_handle == self._rx_handle:
                self._rx_buffer += self._ble.gatts_read(value_handle)
                if self._handler:
                    self._handler()

    def any(self):
        return len(self._rx_buffer)

    def read(self, sz=None):
        if not sz:
            sz = len(self._rx_buffer)
        result = self._rx_buffer[0:sz]
        self._rx_buffer = self._rx_buffer[sz:]
        return result

    def write(self, data):
        for conn_handle in self._connections:
            self._ble.gatts_notify(conn_handle, self._tx_handle, data)

    def close(self):
        for conn_handle in self._connections:
            self._ble.gap_disconnect(conn_handle)
        self._connections.clear()
        self._ble.active(False)

    def _advertise(self, interval_us=500000):
        self._ble.gap_advertise(interval_us, adv_data=self._payload)


class _BLEUARTStream(io.IOBase):
    """Stream wrapper for os.dupterm() compatibility."""

    def __init__(self, uart):
        self._uart = uart
        self._tx_buf = bytearray()
        self._uart.irq(self._on_rx)
        self._timer = None
        if hasattr(machine, "Timer"):
            try:
                self._timer = machine.Timer(-1)
            except ValueError:
                try:
                    self._timer = machine.Timer(3)
                except Exception:
                    pass

    def _on_rx(self):
        if hasattr(os, "dupterm_notify"):
            os.dupterm_notify(None)

    def read(self, sz=None):
        return self._uart.read(sz)

    def readinto(self, buf):
        avail = self._uart.read(len(buf))
        if not avail:
            return None
        for i in range(len(avail)):
            buf[i] = avail[i]
        return len(avail)

    def ioctl(self, op, arg):
        if op == _MP_STREAM_POLL:
            if self._uart.any():
                return _MP_STREAM_POLL_RD
        return 0

    def _flush(self):
        data = self._tx_buf[0:_FLUSH_CHUNK_SIZE]
        self._tx_buf = self._tx_buf[_FLUSH_CHUNK_SIZE:]
        self._uart.write(data)
        if self._tx_buf:
            self._schedule_flush()

    def write(self, buf):
        empty = not self._tx_buf
        self._tx_buf += buf
        if empty:
            self._schedule_flush()
        return len(buf)

    def _schedule_flush(self):
        def _wrap(_arg):
            self._flush()

        if self._timer:
            self._timer.init(
                mode=machine.Timer.ONE_SHOT,
                period=_FLUSH_INTERVAL_MS,
                callback=_wrap,
            )
        else:
            micropython.schedule(_wrap, None)


_uart = None
_stream = None


def _get_device_name():
    uid = machine.unique_id()
    return "Mixly-" + "".join("{:02X}".format(b) for b in uid)


def start(name=None):
    """Start BLE UART REPL service.

    Args:
        name: BLE device name. Defaults to "Mixly-XXXXXXXXXXXX" (MAC hex).
    """
    global _uart, _stream
    if _stream is not None:
        return

    if name is None:
        name = _get_device_name()

    ble = bluetooth.BLE()
    _uart = _BLEUART(ble, name=name)
    _stream = _BLEUARTStream(_uart)
    os.dupterm(_stream, 0)


def stop():
    """Stop BLE UART REPL service."""
    global _uart, _stream
    if _stream is not None:
        os.dupterm(None, 0)
        _uart.close()
        _stream = None
        _uart = None
