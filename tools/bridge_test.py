#!/usr/bin/env python3
"""Checks a xiao-sx1262-usb-bridge board from a PC, without a phone.

It talks to the SX1262 through the bridge and never transmits:

  1. HELLO: board, firmware and radio hints
  2. radio reset and GetStatus
  3. read, write and restore an SX1262 register (sync word)
  4. the same read with chip select driven by hand (PIN_WRITE)
  5. a short receive with timeout, to see the DIO1 event arrive
  6. round-trip latency

Usage:
  pip install pyserial
  python tools/bridge_test.py /dev/ttyACM0      (Linux)
  python tools/bridge_test.py /dev/cu.usbmodem1101   (macOS)
  python tools/bridge_test.py COM5              (Windows)
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time

from sx1262_bridge import (
    PIN_NAMES,
    PIN_NSS,
    PIN_RXEN,
    Bridge,
    BridgeError,
)

# SX1262 opcodes used here.
GET_STATUS = 0xC0
READ_REGISTER = 0x1D
WRITE_REGISTER = 0x0D
SET_STANDBY = 0x80
SET_DIO3_AS_TCXO = 0x97
CALIBRATE = 0x89
SET_DIO_IRQ_PARAMS = 0x08
CLEAR_IRQ_STATUS = 0x02
GET_IRQ_STATUS = 0x12
SET_RX = 0x82
GET_DEVICE_ERRORS = 0x17
CLEAR_DEVICE_ERRORS = 0x07

REG_SYNC_WORD = 0x0740
IRQ_TIMEOUT = 0x0200

CHIP_MODES = {2: "STBY_RC", 3: "STBY_XOSC", 4: "FS", 5: "RX", 6: "TX"}


def ok(msg: str) -> None:
    print(f"  [ok]   {msg}")


def fail(msg: str) -> None:
    print(f"  [FAIL] {msg}")


def read_register(b: Bridge, addr: int, n: int, auto_cs: bool = True) -> bytes:
    cmd = bytes([READ_REGISTER, addr >> 8, addr & 0xFF, 0x00]) + bytes(n)
    return b.spi_transfer(cmd, auto_cs=auto_cs)[4:]


def write_register(b: Bridge, addr: int, data: bytes) -> None:
    b.spi_transfer(bytes([WRITE_REGISTER, addr >> 8, addr & 0xFF]) + data)


def chip_mode(status: int) -> str:
    return CHIP_MODES.get((status >> 4) & 0x07, f"unknown ({status:#04x})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial port of the bridge")
    ap.add_argument("--spi-hz", type=int, default=8_000_000)
    ap.add_argument("--latency-rounds", type=int, default=200)
    args = ap.parse_args()

    failures = 0
    with Bridge(args.port) as b:
        print("HELLO")
        info = b.hello()
        print(f"  board      : {info.board_name} (id {info.board_id})")
        print(f"  firmware   : {info.firmware_version}, protocol v{info.protocol_version}")
        print(f"  radio      : TCXO {info.tcxo_mv} mV on DIO3, DIO2 RF switch: {info.dio2_rf_switch}")
        print(f"  max xfer   : {info.max_transfer} bytes, capabilities 0x{info.capabilities:02X}")
        print(f"  SPI clock  : {b.spi_config(args.spi_hz)} Hz")

        print("Reset and status")
        b.radio_reset()
        status = b.spi_transfer(bytes([GET_STATUS, 0x00]))[1]
        mode = chip_mode(status)
        if mode == "STBY_RC":
            ok(f"SX1262 answered, mode {mode}")
        else:
            fail(f"unexpected mode {mode} (status 0x{status:02X}); check the radio board")
            failures += 1

        levels, present = b.pin_read_all()
        pins = ", ".join(
            f"{PIN_NAMES[i]}={(levels >> i) & 1}" for i in range(len(PIN_NAMES)) if present & (1 << i)
        )
        print(f"  pins: {pins}")

        print("Register read/write (sync word 0x0740)")
        original = read_register(b, REG_SYNC_WORD, 2)
        print(f"  read: {original.hex()}")
        if original == bytes([0x14, 0x24]):
            ok("reset value 0x1424 as expected")
        else:
            fail("expected 0x1424 after reset")
            failures += 1
        write_register(b, REG_SYNC_WORD, bytes([0x34, 0x44]))
        back = read_register(b, REG_SYNC_WORD, 2)
        write_register(b, REG_SYNC_WORD, original)
        if back == bytes([0x34, 0x44]):
            ok("write and read back 0x3444, restored")
        else:
            fail(f"wrote 3444, read back {back.hex()}")
            failures += 1

        print("Manual chip select")
        b.wait_busy(100)
        b.pin_write(PIN_NSS, False)
        manual = read_register(b, REG_SYNC_WORD, 2, auto_cs=False)
        b.pin_write(PIN_NSS, True)
        if manual == original:
            ok("same value with NSS driven by PIN_WRITE")
        else:
            fail(f"read {manual.hex()} with manual NSS")
            failures += 1

        print("DIO1 event (receive with 10 ms timeout, nothing is transmitted)")
        tcxo_code = {1600: 0, 1700: 1, 1800: 2, 2200: 3, 2400: 4, 2700: 5, 3000: 6, 3300: 7}.get(info.tcxo_mv)
        b.spi_transfer(bytes([SET_STANDBY, 0x00]))
        if tcxo_code is not None:
            # 5 ms TCXO start-up timeout, in 15.625 us steps.
            b.spi_transfer(bytes([SET_DIO3_AS_TCXO, tcxo_code, 0x00, 0x01, 0x40]))
            b.spi_transfer(bytes([CALIBRATE, 0x7F]))
            b.wait_busy(100)
            # The first calibration after reset ran before the TCXO was on,
            # which leaves a stale XOSC start error. Clear it.
            b.spi_transfer(bytes([CLEAR_DEVICE_ERRORS, 0x00, 0x00]))
        b.spi_transfer(bytes([CLEAR_IRQ_STATUS, 0xFF, 0xFF]))
        b.spi_transfer(bytes([SET_DIO_IRQ_PARAMS, 0x02, 0x00, 0x02, 0x00, 0, 0, 0, 0]))
        b.irq_config(True)
        if info.capabilities & 0x02:
            b.pin_write(PIN_RXEN, True)
        b.spi_transfer(bytes([SET_RX, 0x00, 0x02, 0x80]))  # 640 * 15.625 us = 10 ms
        event = b.wait_event(1.0)
        if info.capabilities & 0x02:
            b.pin_write(PIN_RXEN, False)
        irq = b.spi_transfer(bytes([GET_IRQ_STATUS, 0, 0, 0]))
        irq_flags = (irq[2] << 8) | irq[3]
        errors = b.spi_transfer(bytes([GET_DEVICE_ERRORS, 0, 0, 0]))
        err_flags = (errors[2] << 8) | errors[3]
        b.spi_transfer(bytes([CLEAR_IRQ_STATUS, 0xFF, 0xFF]))
        b.irq_config(False)
        if event is not None:
            ok(f"DIO1 event #{event.count} received")
        else:
            fail("no DIO1 event within 1 s")
            failures += 1
        if irq_flags & IRQ_TIMEOUT:
            ok("radio reported RX timeout, as expected")
        else:
            fail(f"IRQ status 0x{irq_flags:04X}, expected the timeout bit")
            failures += 1
        if err_flags:
            fail(f"device errors 0x{err_flags:04X} (bit 5 = XOSC start: check the TCXO setting)")
            failures += 1

        print("Latency")
        times = []
        for _ in range(args.latency_rounds):
            t0 = time.perf_counter()
            b.spi_transfer(bytes([GET_STATUS, 0x00]))
            times.append((time.perf_counter() - t0) * 1000)
        print(
            f"  GetStatus round trip over {args.latency_rounds} runs: "
            f"median {statistics.median(times):.2f} ms, max {max(times):.2f} ms"
        )

    print()
    if failures:
        print(f"{failures} check(s) failed")
        return 1
    print("All checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BridgeError as e:
        print(f"error: {e}")
        sys.exit(2)
