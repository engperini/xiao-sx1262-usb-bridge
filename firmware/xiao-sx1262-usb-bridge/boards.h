// Board definitions: which MCU pin drives which SX1262 line.
//
// Each supported board gets one block. The host never sees MCU pin numbers;
// it only uses the logical pin ids from protocol.h, so the same host driver
// works with every board.
#pragma once

#include <Arduino.h>

#if defined(ARDUINO_Seeed_XIAO_nRF52840) || defined(ARDUINO_Seeed_XIAO_nRF52840_Sense)

// Seeed XIAO nRF52840 + Wio-SX1262 kit.
// Same wiring as the MeshCore "Xiao_nrf52" variant.
#define BOARD_ID          0x01
#define BOARD_NAME        "XIAO nRF52840 + Wio-SX1262"

#define PIN_RADIO_NSS     D4
#define PIN_RADIO_RESET   D2
#define PIN_RADIO_BUSY    D3
#define PIN_RADIO_DIO1    D1
#define PIN_RADIO_RXEN    D5
#define PIN_RADIO_TXEN    -1   // not wired on this board

// Radio hints reported in HELLO, so the host can configure the SX1262.
#define RADIO_TCXO_MV         1800  // TCXO powered from DIO3 at 1.8 V
#define RADIO_DIO2_RF_SWITCH  1     // DIO2 drives the RF switch

// Status LED (active low). Green while a host has the port open.
#define PIN_STATUS_LED    LED_GREEN
#define STATUS_LED_ON     LOW
#define STATUS_LED_OFF    HIGH

#else
#error "Unsupported board. Add a block for it in boards.h."
#endif
