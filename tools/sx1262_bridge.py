"""Host-side client for xiao-sx1262-usb-bridge.

A small reference implementation of docs/PROTOCOL.md. It is used by
bridge_test.py and is meant as an example for other host drivers.

Requires pyserial (pip install pyserial).
"""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass

import serial

SYNC = 0xA5
HEADER_LEN = 5
RESPONSE_FLAG = 0x80

REQ_HELLO = 0x01
REQ_SPI_TRANSFER = 0x02
REQ_PIN_WRITE = 0x03
REQ_PIN_READ = 0x04
REQ_PIN_READ_ALL = 0x05
REQ_SPI_CONFIG = 0x06
REQ_RADIO_RESET = 0x07
REQ_IRQ_CONFIG = 0x08
REQ_WAIT_BUSY = 0x09

EVT_DIO1 = 0xC1

XFER_AUTO_CS = 0x01
XFER_WAIT_BUSY = 0x02

PIN_NSS = 0
PIN_RESET = 1
PIN_BUSY = 2
PIN_DIO1 = 3
PIN_RXEN = 4
PIN_TXEN = 5
PIN_NAMES = ["NSS", "RESET", "BUSY", "DIO1", "RXEN", "TXEN"]

STATUS_NAMES = {
    0x00: "OK",
    0x01: "UNKNOWN_CMD",
    0x02: "BAD_LENGTH",
    0x03: "BAD_PIN",
    0x04: "BUSY_TIMEOUT",
    0x05: "BAD_ARG",
}


class BridgeError(Exception):
    pass


class StatusError(BridgeError):
    def __init__(self, request: int, status: int):
        self.request = request
        self.status = status
        name = STATUS_NAMES.get(status, f"0x{status:02X}")
        super().__init__(f"request 0x{request:02X} failed: {name}")


@dataclass
class HelloInfo:
    protocol_version: int
    board_id: int
    firmware_version: str
    capabilities: int
    dio2_rf_switch: bool
    tcxo_mv: int
    max_transfer: int
    board_name: str


@dataclass
class Dio1Event:
    count: int
    time_us: int


class Bridge:
    def __init__(self, port: str, timeout: float = 1.0):
        # pyserial asserts DTR on open. The bridge needs DTR: without it,
        # the USB stack drops everything the bridge sends.
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        self.timeout = timeout
        self.seq = 0
        self.events: list[Dio1Event] = []
        self._buf = bytearray()
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ------------------------------------------------------------ framing

    def _read_frame(self, deadline: float) -> tuple[int, int, bytes] | None:
        while True:
            # Drop bytes until a SYNC.
            while self._buf and self._buf[0] != SYNC:
                del self._buf[0]
            if len(self._buf) >= HEADER_LEN:
                length = self._buf[3] | (self._buf[4] << 8)
                if len(self._buf) >= HEADER_LEN + length:
                    frame = bytes(self._buf[: HEADER_LEN + length])
                    del self._buf[: HEADER_LEN + length]
                    return frame[1], frame[2], frame[HEADER_LEN:]
            if time.monotonic() >= deadline:
                return None
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self._buf += chunk

    def _handle_event(self, ftype: int, payload: bytes) -> None:
        if ftype == EVT_DIO1 and len(payload) == 8:
            count, time_us = struct.unpack("<II", payload)
            self.events.append(Dio1Event(count, time_us))

    def request(self, rtype: int, payload: bytes = b"") -> bytes:
        """Sends a request and returns the response data (after the status)."""
        self.seq = (self.seq + 1) & 0xFF
        frame = struct.pack("<BBBH", SYNC, rtype, self.seq, len(payload)) + payload
        self.ser.write(frame)

        deadline = time.monotonic() + self.timeout
        while True:
            got = self._read_frame(deadline)
            if got is None:
                raise BridgeError(f"no response to request 0x{rtype:02X}")
            ftype, seq, data = got
            if ftype >= 0xC0:
                self._handle_event(ftype, data)
                continue
            if ftype != (rtype | RESPONSE_FLAG) or seq != self.seq:
                continue  # stale response
            if not data:
                raise BridgeError("empty response")
            if data[0] != 0:
                raise StatusError(rtype, data[0])
            return data[1:]

    def wait_event(self, timeout: float) -> Dio1Event | None:
        if self.events:
            return self.events.pop(0)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self._read_frame(deadline)
            if got is None:
                break
            ftype, _, data = got
            if ftype >= 0xC0:
                self._handle_event(ftype, data)
            if self.events:
                return self.events.pop(0)
        return None

    # ----------------------------------------------------------- requests

    def hello(self) -> HelloInfo:
        d = self.request(REQ_HELLO)
        proto, board, major, minor, patch, caps, rflags = d[:7]
        tcxo_mv, max_xfer = struct.unpack("<HH", d[7:11])
        name_len = d[11]
        name = d[12 : 12 + name_len].decode("ascii", "replace")
        return HelloInfo(
            protocol_version=proto,
            board_id=board,
            firmware_version=f"{major}.{minor}.{patch}",
            capabilities=caps,
            dio2_rf_switch=bool(rflags & 0x01),
            tcxo_mv=tcxo_mv,
            max_transfer=max_xfer,
            board_name=name,
        )

    def spi_transfer(self, data: bytes, auto_cs: bool = True, wait_busy: bool = True) -> bytes:
        flags = (XFER_AUTO_CS if auto_cs else 0) | (XFER_WAIT_BUSY if wait_busy else 0)
        return self.request(REQ_SPI_TRANSFER, bytes([flags]) + bytes(data))

    def pin_write(self, pin: int, high: bool) -> None:
        self.request(REQ_PIN_WRITE, bytes([pin, 1 if high else 0]))

    def pin_read(self, pin: int) -> bool:
        return self.request(REQ_PIN_READ, bytes([pin]))[0] != 0

    def pin_read_all(self) -> tuple[int, int]:
        d = self.request(REQ_PIN_READ_ALL)
        return d[0], d[1]

    def spi_config(self, hz: int) -> int:
        return struct.unpack("<I", self.request(REQ_SPI_CONFIG, struct.pack("<I", hz)))[0]

    def radio_reset(self, timeout_ms: int = 100) -> None:
        self.request(REQ_RADIO_RESET, struct.pack("<H", timeout_ms))

    def irq_config(self, enable: bool) -> None:
        self.request(REQ_IRQ_CONFIG, bytes([1 if enable else 0]))

    def wait_busy(self, timeout_ms: int) -> None:
        self.request(REQ_WAIT_BUSY, struct.pack("<H", timeout_ms))
