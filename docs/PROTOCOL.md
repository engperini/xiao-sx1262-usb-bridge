# Bridge protocol, version 1

The bridge shows up as a standard USB CDC-ACM serial port. The host sends
requests and the bridge answers each one. The bridge also sends unsolicited
events when DIO1 rises. The baud rate is ignored.

All multi-byte numbers are little endian.

## Opening the port

The host **must assert DTR** (CDC `SET_CONTROL_LINE_STATE`, request `0x22`,
value `0x0001` or `0x0003`, on the communication interface). Without DTR the
bridge's USB stack discards everything the bridge sends, so the host never
sees a response. pyserial and most serial libraries assert DTR when they open
the port; a raw usbdevfs driver on Android must send the control request
itself.

When DTR drops (the host closed the port or went away), the bridge puts the
radio in a safe state: NSS high, RXEN and TXEN low, a short reset pulse (the
SX1262 comes back in standby), and DIO1 events off. A transmission in progress
stops there.

The green LED is on while DTR is asserted.

## Frame

```
offset  size  field
0       1     sync, always 0xA5
1       1     type
2       1     seq
3       2     payload length (u16), at most 600
5       n     payload
```

- **Requests** (host to bridge) use types `0x01` to `0x3F`. The host picks
  `seq`, usually a counter.
- **Responses** use `type = request type | 0x80` and the same `seq`. The first
  payload byte is a status code and the rest is data.
- **Events** (bridge to host, unsolicited) use types `0xC0` to `0xFF`. They
  can arrive between a request and its response, so the host should handle
  them wherever it reads frames.

USB already checks data integrity, so there is no checksum. The bridge drops a
partial frame when no byte arrives for 200 ms, and skips bytes until the next
`0xA5`, so a host can always resynchronise.

The bridge handles one request at a time, in order. A host can pipeline
several requests and match the responses by `seq`.

## Status codes

| Code | Name | Meaning |
| --- | --- | --- |
| `0x00` | OK | |
| `0x01` | UNKNOWN_CMD | Request type not known to this firmware |
| `0x02` | BAD_LENGTH | Payload length wrong for this request |
| `0x03` | BAD_PIN | Pin id unknown, not wired on this board, or not an output |
| `0x04` | BUSY_TIMEOUT | BUSY stayed high past the timeout |
| `0x05` | BAD_ARG | Argument out of range |

On an error the response has only the status byte.

## Logical pins

The host never sees MCU pin numbers. Pins are named by id:

| Id | Pin | Direction |
| --- | --- | --- |
| 0 | NSS (chip select) | output |
| 1 | RESET | output |
| 2 | BUSY | input |
| 3 | DIO1 | input |
| 4 | RXEN (RF switch, receive) | output, optional |
| 5 | TXEN (RF switch, transmit) | output, optional |

HELLO says which optional pins the board has.

## Requests

### `0x01` HELLO

Request payload: empty.

Response data:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | protocol version (1) |
| 1 | 1 | board id (`0x01` = XIAO nRF52840 + Wio-SX1262) |
| 2 | 3 | firmware version: major, minor, patch |
| 5 | 1 | capabilities: bit 0 DIO1 events, bit 1 RXEN present, bit 2 TXEN present |
| 6 | 1 | radio flags: bit 0 DIO2 drives the RF switch |
| 7 | 2 | TCXO voltage on DIO3 in mV, 0 if the board has a crystal |
| 9 | 2 | largest SPI transfer per request, in bytes |
| 11 | 1 | board name length |
| 12 | n | board name, ASCII |

The radio fields let a host configure the SX1262 without knowing the board.

### `0x02` SPI_TRANSFER

Request payload: `flags` (1 byte), then the bytes to send.

| Flag | Meaning |
| --- | --- |
| `0x01` AUTO_CS | Drive NSS low before the transfer and high after it |
| `0x02` WAIT_BUSY | Before the transfer, wait up to 100 ms for BUSY low |

Response data: the bytes clocked in, same length as sent. SPI mode 0, MSB
first.

A typical SX1262 command is one request with both flags set:
`02 | 03 | opcode, params...`. Without AUTO_CS the host drives NSS with
PIN_WRITE, which is how a CH341-style driver works; it costs two more round
trips per command.

### `0x03` PIN_WRITE

Request payload: `pin id`, `level` (0 or 1). Response data: none.

### `0x04` PIN_READ

Request payload: `pin id`. Response data: `level` (0 or 1).

### `0x05` PIN_READ_ALL

Request payload: empty. Response data: `levels`, `present`. Bit *n* of each
byte is pin id *n*.

### `0x06` SPI_CONFIG

Request payload: SPI clock in Hz (u32). Response data: the clock actually
used (u32).

The default is 8 MHz. The nRF52840 runs at 125 kHz times a power of two, up to
8 MHz, and the bridge picks the highest one that is not above the request.

### `0x07` RADIO_RESET

Request payload: empty, or a BUSY timeout in ms (u16, default 100).

Pulls RESET low for 1 ms, releases it, then waits for BUSY low. Answers
BUSY_TIMEOUT if the radio does not come up.

### `0x08` IRQ_CONFIG

Request payload: `enable` (0 or 1). Response data: none.

Turns DIO1 events on or off. They are off at power-up and whenever the host
disconnects.

### `0x09` WAIT_BUSY

Request payload: timeout in ms (u16). Response: OK once BUSY is low, or
BUSY_TIMEOUT.

## Events

### `0xC1` DIO1

Sent on every rising edge of DIO1 while events are enabled. The SX1262 raises
DIO1 for the interrupts mapped to it with `SetDioIrqParams` (RX done, TX done,
timeout, ...). The host reads and clears the cause with `GetIrqStatus` and
`ClearIrqStatus`.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | edge counter since power-up (u32) |
| 4 | 4 | bridge time of the edge in microseconds (u32, wraps) |

`seq` holds the low byte of the counter. The counter lets the host notice
edges it missed. Rising edges that come while the previous one has not been
sent yet are merged into one event, with the counter showing how many there
were.

## Example

GetStatus (`C0 00`) with chip select and BUSY wait:

```
host   -> A5 02 01 03 00  03 C0 00
bridge <- A5 82 01 03 00  00 xx SS
```

The response is `status OK`, then the two bytes read back. `SS` is the SX1262
status byte; bits 6:4 = 2 mean mode STBY_RC, as after a reset. The first byte
read back, while the opcode goes out, has no meaning for this command.
