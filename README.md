# xiao-sx1262-usb-bridge

Firmware that turns a **Seeed XIAO + Wio-SX1262** kit into a plain **USB-to-SPI
bridge** for the LoRa radio, the same job the CH341 chip does inside a USB
MeshStick.

The bridge does not run LoRa, MeshCore or Meshtastic. The host does: an
Android phone running [AndroidMeshcore](https://github.com/engperini/AndroidMeshcore),
or a PC. The bridge only moves SPI bytes and the radio's control pins over USB.

```
Host (phone or PC): mesh stack + SX1262 driver
   │  USB CDC serial (this protocol)
XIAO nRF52840: xiao-sx1262-usb-bridge
   │  SPI + NSS / RESET / BUSY / DIO1 / RXEN
Wio-SX1262: radio
```

## Why

- **The node lives on the host.** Keys, contacts and messages stay on the
  phone. You can swap the radio and remain the same node.
- **Updates are host updates.** A new mesh firmware version is an app update;
  the bridge never needs reflashing for it.
- **One host driver, many radios.** A MeshStick (CH341) and a XIAO kit look
  the same to the SX1262 driver on the host.
- **No Bluetooth pairing.** Plug it in and it works.

Compared with a CH341, the bridge also reports DIO1 edges as events (no
polling) and can run a whole SX1262 command, chip select and BUSY wait
included, in one USB round trip.

## Supported boards

| Board | Status | Firmware file |
| --- | --- | --- |
| XIAO nRF52840 + Wio-SX1262 kit | ✅ first target | `xiao-sx1262-usb-bridge-xiao_nrf52840.uf2` |
| XIAO ESP32S3 + Wio-SX1262 kit | planned | |

Pin mapping for the nRF52840 kit (same as the MeshCore `Xiao_nrf52` variant):

| SX1262 | XIAO pin |
| --- | --- |
| NSS | D4 |
| RESET | D2 |
| BUSY | D3 |
| DIO1 | D1 |
| RXEN | D5 |
| SCK / MISO / MOSI | D8 / D9 / D10 |

The radio uses a 1.8 V TCXO on DIO3, and DIO2 drives the RF switch. The bridge
reports both to the host.

## Flashing (XIAO nRF52840)

1. Download `xiao-sx1262-usb-bridge-xiao_nrf52840.uf2` from the
   [latest release](https://github.com/engperini/xiao-sx1262-usb-bridge/releases/latest),
   or from the artifacts of the latest
   [build](https://github.com/engperini/xiao-sx1262-usb-bridge/actions).
2. Plug the XIAO into a computer and **double-press the reset button**. A USB
   drive named `XIAO-SENSE` (or similar) appears.
3. Copy the `.uf2` file to that drive. The board reboots into the bridge.

To go back to MeshCore or Meshtastic later, flash their `.uf2` the same way.

**Connect the antenna before using the radio.** The bridge never transmits on
its own, but the host can.

## Testing from a PC

`tools/bridge_test.py` checks the board without a phone. It never transmits:

```
pip install pyserial
python tools/bridge_test.py /dev/ttyACM0        # Linux
python tools/bridge_test.py /dev/cu.usbmodem1101 # macOS
python tools/bridge_test.py COM5                 # Windows
```

It reads the board info, resets the radio, reads and writes an SX1262
register, runs a 10 ms receive to see a DIO1 event arrive, and measures the
USB round-trip time. `tools/sx1262_bridge.py` is a small Python client for the
protocol that you can reuse.

## Protocol

See [docs/PROTOCOL.md](docs/PROTOCOL.md). In short: framed requests and
responses over USB CDC (`SPI_TRANSFER`, `PIN_WRITE`, `PIN_READ`,
`RADIO_RESET`, ...), plus a `DIO1` event. The host must assert DTR when it
opens the port.

## Building

The firmware is an Arduino sketch built with `arduino-cli` and the Seeed nRF52
core (`Seeeduino:nrf52`).

```
pip install adafruit-nrfutil
scripts/build.sh            # writes build/xiao-sx1262-usb-bridge-xiao_nrf52840.uf2
```

GitHub Actions builds every push and attaches the `.uf2` to releases tagged
`v*`.

## Layout

| Path | What |
| --- | --- |
| `firmware/xiao-sx1262-usb-bridge/` | Sketch: `boards.h` (pins per board), `protocol.h`, main loop |
| `docs/PROTOCOL.md` | Wire protocol |
| `tools/` | Python client and PC test |
| `scripts/build.sh` | Build and `.uf2` conversion |

## License

MIT, see [LICENSE](LICENSE).
