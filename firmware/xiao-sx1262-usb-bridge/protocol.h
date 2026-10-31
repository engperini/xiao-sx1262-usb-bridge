// Wire protocol between the host (phone / PC) and the bridge.
// See docs/PROTOCOL.md for the full description.
#pragma once

#include <stdint.h>

#define PROTOCOL_VERSION   1

#define FW_VERSION_MAJOR   0
#define FW_VERSION_MINOR   1
#define FW_VERSION_PATCH   0

// Frame: SYNC | type | seq | len (u16 LE) | payload[len]
#define FRAME_SYNC         0xA5
#define FRAME_HEADER_LEN   5
#define MAX_PAYLOAD        600

// Requests (host -> bridge): 0x01..0x3F.
// The response uses the same seq and type = request | 0x80.
enum RequestType : uint8_t {
  REQ_HELLO        = 0x01,
  REQ_SPI_TRANSFER = 0x02,
  REQ_PIN_WRITE    = 0x03,
  REQ_PIN_READ     = 0x04,
  REQ_PIN_READ_ALL = 0x05,
  REQ_SPI_CONFIG   = 0x06,
  REQ_RADIO_RESET  = 0x07,
  REQ_IRQ_CONFIG   = 0x08,
  REQ_WAIT_BUSY    = 0x09,
};

#define RESPONSE_FLAG      0x80

// Events (bridge -> host, unsolicited): 0xC0..0xFF.
enum EventType : uint8_t {
  EVT_DIO1 = 0xC1,
};

// First payload byte of every response.
enum Status : uint8_t {
  ST_OK           = 0x00,
  ST_UNKNOWN_CMD  = 0x01,
  ST_BAD_LENGTH   = 0x02,
  ST_BAD_PIN      = 0x03,
  ST_BUSY_TIMEOUT = 0x04,
  ST_BAD_ARG      = 0x05,
};

// Logical pin ids.
enum PinId : uint8_t {
  PIN_ID_NSS   = 0,
  PIN_ID_RESET = 1,
  PIN_ID_BUSY  = 2,
  PIN_ID_DIO1  = 3,
  PIN_ID_RXEN  = 4,
  PIN_ID_TXEN  = 5,
  PIN_ID_COUNT = 6,
};

// SPI_TRANSFER flags.
#define XFER_AUTO_CS    0x01  // drive NSS low for the transfer, high after it
#define XFER_WAIT_BUSY  0x02  // wait for BUSY low before the transfer

// HELLO capability bits.
#define CAP_DIO1_EVENTS 0x01
#define CAP_RXEN        0x02
#define CAP_TXEN        0x04

// HELLO radio flag bits.
#define RADIO_FLAG_DIO2_RF_SWITCH 0x01
