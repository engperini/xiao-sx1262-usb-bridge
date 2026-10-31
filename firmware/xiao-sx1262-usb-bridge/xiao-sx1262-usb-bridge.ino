// xiao-sx1262-usb-bridge
//
// Turns a XIAO + SX1262 board into a plain USB-to-SPI bridge for the radio,
// like the CH341 inside a USB MeshStick. The bridge knows nothing about
// LoRa, MeshCore or Meshtastic: the host (an Android phone, a PC) runs the
// radio driver and the mesh stack, and the bridge only moves SPI bytes and
// radio control pins over USB CDC.
//
// Protocol: docs/PROTOCOL.md

#include <Arduino.h>
#include <SPI.h>

#include "boards.h"
#include "protocol.h"

namespace {

constexpr uint32_t kDefaultSpiHz = 8000000;
constexpr uint32_t kMinSpiHz = 125000;
constexpr uint32_t kMaxSpiHz = 8000000;
constexpr uint16_t kDefaultBusyTimeoutMs = 100;
constexpr uint32_t kFrameTimeoutMs = 200;  // drop a partial frame after this

const int kPinMap[PIN_ID_COUNT] = {
  PIN_RADIO_NSS,
  PIN_RADIO_RESET,
  PIN_RADIO_BUSY,
  PIN_RADIO_DIO1,
  PIN_RADIO_RXEN,
  PIN_RADIO_TXEN,
};

const bool kPinIsOutput[PIN_ID_COUNT] = {
  true,   // NSS
  true,   // RESET
  false,  // BUSY
  false,  // DIO1
  true,   // RXEN
  true,   // TXEN
};

uint8_t rx_frame[FRAME_HEADER_LEN + MAX_PAYLOAD];
size_t rx_len = 0;
uint32_t rx_last_ms = 0;

// Response frame: header, status byte, data.
uint8_t tx_frame[FRAME_HEADER_LEN + 1 + MAX_PAYLOAD];

uint32_t spi_hz = kDefaultSpiHz;
SPISettings spi_settings(kDefaultSpiHz, MSBFIRST, SPI_MODE0);

volatile bool dio1_pending = false;
volatile uint32_t dio1_count = 0;
volatile uint32_t dio1_time_us = 0;
bool irq_events_enabled = false;

bool host_connected = false;

// ---------------------------------------------------------------- helpers

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool pinPresent(uint8_t id) {
  return id < PIN_ID_COUNT && kPinMap[id] >= 0;
}

// The nRF52 SPI master only runs at 125 kHz * 2^n. Report what we really get.
uint32_t quantizeSpiHz(uint32_t hz) {
  if (hz < kMinSpiHz) hz = kMinSpiHz;
  if (hz > kMaxSpiHz) hz = kMaxSpiHz;
  uint32_t actual = kMinSpiHz;
  while (actual * 2 <= hz) actual *= 2;
  return actual;
}

bool waitBusyLow(uint16_t timeout_ms) {
  const uint32_t start = millis();
  while (digitalRead(PIN_RADIO_BUSY) == HIGH) {
    if (millis() - start >= timeout_ms) return false;
  }
  return true;
}

void sendFrame(uint8_t type, uint8_t seq, size_t payload_len) {
  tx_frame[0] = FRAME_SYNC;
  tx_frame[1] = type;
  tx_frame[2] = seq;
  putU16(&tx_frame[3], static_cast<uint16_t>(payload_len));
  Serial.write(tx_frame, FRAME_HEADER_LEN + payload_len);
  Serial.flush();
}

// Data for a response goes in respData(); then call respond(len_of_data).
uint8_t* respData() { return &tx_frame[FRAME_HEADER_LEN + 1]; }

void respond(uint8_t req_type, uint8_t seq, uint8_t status, size_t data_len = 0) {
  tx_frame[FRAME_HEADER_LEN] = status;
  sendFrame(req_type | RESPONSE_FLAG, seq, 1 + data_len);
}

// Leave the radio quiet: chip deselected, RF switch off, radio held in reset
// briefly and released, so it comes back in standby.
void radioSafeState() {
  digitalWrite(PIN_RADIO_NSS, HIGH);
  if (PIN_RADIO_RXEN >= 0) digitalWrite(PIN_RADIO_RXEN, LOW);
  if (PIN_RADIO_TXEN >= 0) digitalWrite(PIN_RADIO_TXEN, LOW);
  digitalWrite(PIN_RADIO_RESET, LOW);
  delay(2);
  digitalWrite(PIN_RADIO_RESET, HIGH);
}

void onDio1Rise() {
  dio1_count++;
  dio1_time_us = micros();
  dio1_pending = true;
}

// --------------------------------------------------------------- requests

void handleHello(uint8_t seq) {
  uint8_t* d = respData();
  size_t n = 0;
  d[n++] = PROTOCOL_VERSION;
  d[n++] = BOARD_ID;
  d[n++] = FW_VERSION_MAJOR;
  d[n++] = FW_VERSION_MINOR;
  d[n++] = FW_VERSION_PATCH;
  uint8_t caps = CAP_DIO1_EVENTS;
  if (PIN_RADIO_RXEN >= 0) caps |= CAP_RXEN;
  if (PIN_RADIO_TXEN >= 0) caps |= CAP_TXEN;
  d[n++] = caps;
  d[n++] = RADIO_DIO2_RF_SWITCH ? RADIO_FLAG_DIO2_RF_SWITCH : 0;
  putU16(&d[n], RADIO_TCXO_MV);
  n += 2;
  putU16(&d[n], MAX_PAYLOAD - 1);  // largest SPI transfer per request
  n += 2;
  const char* name = BOARD_NAME;
  const size_t name_len = strlen(name);
  d[n++] = static_cast<uint8_t>(name_len);
  memcpy(&d[n], name, name_len);
  n += name_len;
  respond(REQ_HELLO, seq, ST_OK, n);
}

void handleSpiTransfer(uint8_t seq, const uint8_t* p, size_t len) {
  if (len < 1) {
    respond(REQ_SPI_TRANSFER, seq, ST_BAD_LENGTH);
    return;
  }
  const uint8_t flags = p[0];
  const size_t count = len - 1;

  if ((flags & XFER_WAIT_BUSY) && !waitBusyLow(kDefaultBusyTimeoutMs)) {
    respond(REQ_SPI_TRANSFER, seq, ST_BUSY_TIMEOUT);
    return;
  }

  // Full duplex, in place: the bytes read back replace the bytes sent.
  uint8_t* buf = respData();
  memcpy(buf, p + 1, count);

  SPI.beginTransaction(spi_settings);
  if (flags & XFER_AUTO_CS) digitalWrite(PIN_RADIO_NSS, LOW);
  if (count > 0) SPI.transfer(buf, count);
  if (flags & XFER_AUTO_CS) digitalWrite(PIN_RADIO_NSS, HIGH);
  SPI.endTransaction();

  respond(REQ_SPI_TRANSFER, seq, ST_OK, count);
}

void handlePinWrite(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 2) {
    respond(REQ_PIN_WRITE, seq, ST_BAD_LENGTH);
    return;
  }
  const uint8_t id = p[0];
  if (!pinPresent(id) || !kPinIsOutput[id]) {
    respond(REQ_PIN_WRITE, seq, ST_BAD_PIN);
    return;
  }
  digitalWrite(kPinMap[id], p[1] ? HIGH : LOW);
  respond(REQ_PIN_WRITE, seq, ST_OK);
}

void handlePinRead(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 1) {
    respond(REQ_PIN_READ, seq, ST_BAD_LENGTH);
    return;
  }
  const uint8_t id = p[0];
  if (!pinPresent(id)) {
    respond(REQ_PIN_READ, seq, ST_BAD_PIN);
    return;
  }
  respData()[0] = digitalRead(kPinMap[id]) == HIGH ? 1 : 0;
  respond(REQ_PIN_READ, seq, ST_OK, 1);
}

void handlePinReadAll(uint8_t seq, size_t len) {
  if (len != 0) {
    respond(REQ_PIN_READ_ALL, seq, ST_BAD_LENGTH);
    return;
  }
  uint8_t levels = 0;
  uint8_t present = 0;
  for (uint8_t id = 0; id < PIN_ID_COUNT; id++) {
    if (!pinPresent(id)) continue;
    present |= static_cast<uint8_t>(1u << id);
    if (digitalRead(kPinMap[id]) == HIGH) levels |= static_cast<uint8_t>(1u << id);
  }
  respData()[0] = levels;
  respData()[1] = present;
  respond(REQ_PIN_READ_ALL, seq, ST_OK, 2);
}

void handleSpiConfig(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 4) {
    respond(REQ_SPI_CONFIG, seq, ST_BAD_LENGTH);
    return;
  }
  spi_hz = quantizeSpiHz(getU32(p));
  spi_settings = SPISettings(spi_hz, MSBFIRST, SPI_MODE0);
  putU32(respData(), spi_hz);
  respond(REQ_SPI_CONFIG, seq, ST_OK, 4);
}

void handleRadioReset(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 0 && len != 2) {
    respond(REQ_RADIO_RESET, seq, ST_BAD_LENGTH);
    return;
  }
  const uint16_t timeout_ms = len == 2 ? getU16(p) : kDefaultBusyTimeoutMs;
  digitalWrite(PIN_RADIO_NSS, HIGH);
  digitalWrite(PIN_RADIO_RESET, LOW);
  delay(1);  // SX1262 needs > 100 us
  digitalWrite(PIN_RADIO_RESET, HIGH);
  delay(1);
  respond(REQ_RADIO_RESET, seq, waitBusyLow(timeout_ms) ? ST_OK : ST_BUSY_TIMEOUT);
}

void handleIrqConfig(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 1) {
    respond(REQ_IRQ_CONFIG, seq, ST_BAD_LENGTH);
    return;
  }
  noInterrupts();
  irq_events_enabled = p[0] != 0;
  dio1_pending = false;
  interrupts();
  respond(REQ_IRQ_CONFIG, seq, ST_OK);
}

void handleWaitBusy(uint8_t seq, const uint8_t* p, size_t len) {
  if (len != 2) {
    respond(REQ_WAIT_BUSY, seq, ST_BAD_LENGTH);
    return;
  }
  respond(REQ_WAIT_BUSY, seq, waitBusyLow(getU16(p)) ? ST_OK : ST_BUSY_TIMEOUT);
}

void handleFrame(uint8_t type, uint8_t seq, const uint8_t* p, size_t len) {
  switch (type) {
    case REQ_HELLO:        handleHello(seq); break;
    case REQ_SPI_TRANSFER: handleSpiTransfer(seq, p, len); break;
    case REQ_PIN_WRITE:    handlePinWrite(seq, p, len); break;
    case REQ_PIN_READ:     handlePinRead(seq, p, len); break;
    case REQ_PIN_READ_ALL: handlePinReadAll(seq, len); break;
    case REQ_SPI_CONFIG:   handleSpiConfig(seq, p, len); break;
    case REQ_RADIO_RESET:  handleRadioReset(seq, p, len); break;
    case REQ_IRQ_CONFIG:   handleIrqConfig(seq, p, len); break;
    case REQ_WAIT_BUSY:    handleWaitBusy(seq, p, len); break;
    default:
      // Only answer things that look like requests.
      if (type >= 0x01 && type <= 0x3F) respond(type, seq, ST_UNKNOWN_CMD);
      break;
  }
}

// ------------------------------------------------------------- USB input

void pollSerial() {
  if (rx_len > 0 && millis() - rx_last_ms > kFrameTimeoutMs) rx_len = 0;

  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    rx_last_ms = millis();

    if (rx_len == 0 && c != FRAME_SYNC) continue;  // resync on SYNC
    rx_frame[rx_len++] = static_cast<uint8_t>(c);

    if (rx_len < FRAME_HEADER_LEN) continue;
    const size_t payload_len = getU16(&rx_frame[3]);
    if (payload_len > MAX_PAYLOAD) {
      rx_len = 0;  // garbage header; look for the next SYNC
      continue;
    }
    if (rx_len == FRAME_HEADER_LEN + payload_len) {
      handleFrame(rx_frame[1], rx_frame[2], &rx_frame[FRAME_HEADER_LEN], payload_len);
      rx_len = 0;
    }
  }
}

void pollDio1() {
  if (!dio1_pending) return;
  noInterrupts();
  dio1_pending = false;
  const uint32_t count = dio1_count;
  const uint32_t time_us = dio1_time_us;
  interrupts();
  if (!irq_events_enabled) return;

  uint8_t* d = &tx_frame[FRAME_HEADER_LEN];
  putU32(&d[0], count);
  putU32(&d[4], time_us);
  sendFrame(EVT_DIO1, static_cast<uint8_t>(count), 8);
}

void pollHostConnection() {
  // DTR tells us whether a host has the port open.
  const bool connected = static_cast<bool>(Serial);
  if (connected == host_connected) return;
  host_connected = connected;
  digitalWrite(PIN_STATUS_LED, connected ? STATUS_LED_ON : STATUS_LED_OFF);
  if (!connected) {
    // The host went away, maybe in the middle of a transmission.
    irq_events_enabled = false;
    rx_len = 0;
    radioSafeState();
  }
}

}  // namespace

void setup() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, STATUS_LED_OFF);

  pinMode(PIN_RADIO_NSS, OUTPUT);
  digitalWrite(PIN_RADIO_NSS, HIGH);
  pinMode(PIN_RADIO_RESET, OUTPUT);
  digitalWrite(PIN_RADIO_RESET, HIGH);
  if (PIN_RADIO_RXEN >= 0) {
    pinMode(PIN_RADIO_RXEN, OUTPUT);
    digitalWrite(PIN_RADIO_RXEN, LOW);
  }
  if (PIN_RADIO_TXEN >= 0) {
    pinMode(PIN_RADIO_TXEN, OUTPUT);
    digitalWrite(PIN_RADIO_TXEN, LOW);
  }
  pinMode(PIN_RADIO_BUSY, INPUT);
  pinMode(PIN_RADIO_DIO1, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_RADIO_DIO1), onDio1Rise, RISING);

  SPI.begin();
  radioSafeState();

  Serial.begin(115200);  // baud rate is ignored on USB CDC
}

void loop() {
  pollHostConnection();
  pollSerial();
  pollDio1();
}
