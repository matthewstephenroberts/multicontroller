// lpf2.h — LEGO Powered Up / LPF2 UART sensor emulation.
//
// The firmware drives it through the C API in lego_emit.h.
//
// ── Header byte layout ────────────────────────────────────────────────────
//
//   Bit  7  6 │ 5  4  3 │ 2  1  0
//        MSG    LLL       MMM
//
//   MSG  (bits 7–6) — message class: SYS / CMD / INFO / DATA
//   LLL  (bits 5–3) — payload length as a power of two: 2^LLL bytes
//   MMM  (bits 2–0) — mode index (INFO / DATA) or sub-command (CMD)
//
//   Every frame: [header] [payload × 2^LLL] [checksum]
//   Checksum = 0xFF XOR header XOR every payload byte.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include <stdint.h>
#include "driver/uart.h"

typedef uint8_t byte;

// ── Debug ─────────────────────────────────────────────────────────────────
// Category flags. The active set is a *runtime* mask (lpf2_debug_mask), set via
// lpf2_set_debug_mask() — driven by the lego "debug" config, so the byte-level
// handshake/TX/RX trace can be turned on and off live from the web UI without a
// rebuild. Output goes to the serial console (idf.py monitor / scripts/monitor.sh).
#define DBG_SENSOR  0x01
#define DBG_TX      0x02   // every frame transmitted to the hub
#define DBG_RX      0x04   // every byte / frame received from the hub
#define DBG_INIT    0x08
#define DBG_CONN    0x10   // handshake, connect, disconnect events
#define DBG_CAL     0x20
#define DBG_EVT     0x40   // high-level events: hub mode SELECT, combo on/off, WRITE

// Set LPF2_DEBUG_BUILD to 0 to strip all trace code from the build entirely.
// Default: compiled in, gated at runtime (a single masked test when off).
#ifndef LPF2_DEBUG_BUILD
#define LPF2_DEBUG_BUILD 1
#endif

extern volatile uint32_t lpf2_debug_mask;   // OR of the DBG_* flags currently enabled
void lpf2_set_debug_mask(uint32_t mask);

#if LPF2_DEBUG_BUILD
  #include <stdio.h>
  #define _DBG(cat, fmt, ...) \
      do { if (lpf2_debug_mask & (cat)) printf("[LPF2] " fmt, ##__VA_ARGS__); } while (0)
#else
  #define _DBG(cat, fmt, ...)  do {} while (0)
#endif

#define DBG_SENSOR_PRINT(fmt, ...)  _DBG(DBG_SENSOR, "[SENSOR] " fmt, ##__VA_ARGS__)
#define DBG_TX_PRINT(fmt, ...)      _DBG(DBG_TX,     "[TX]     " fmt, ##__VA_ARGS__)
#define DBG_RX_PRINT(fmt, ...)      _DBG(DBG_RX,     "[RX]     " fmt, ##__VA_ARGS__)
#define DBG_INIT_PRINT(fmt, ...)    _DBG(DBG_INIT,   "[INIT]   " fmt, ##__VA_ARGS__)
#define DBG_CONN_PRINT(fmt, ...)    _DBG(DBG_CONN,   "[CONN]   " fmt, ##__VA_ARGS__)
#define DBG_CAL_PRINT(fmt, ...)     _DBG(DBG_CAL,    "[CAL]    " fmt, ##__VA_ARGS__)
#define DBG_EVT_PRINT(fmt, ...)     _DBG(DBG_EVT,    "[EVT]    " fmt, ##__VA_ARGS__)

// ── Protocol constants ─────────────────────────────────────────────────────
#define MSG_CLASS_MASK  0xC0
#define LLL_MASK        0x38
#define MMM_MASK        0x07
#define LLL_SHIFT       3

#define MSG_CLASS_SYS   0x00
#define MSG_CLASS_CMD   0x40
#define MSG_CLASS_INFO  0x80
#define MSG_CLASS_DATA  0xC0

#define SYS_SYNC        0x00
#define SYS_NACK        0x02
#define SYS_ACK         0x04

#define CMD_TYPE        0x40
#define CMD_MODES       0x49
#define CMD_MODES4      0x51
#define CMD_SPEED       0x52
#define CMD_SELECT      0x43
#define CMD_EXT_MODE    0x46
#define CMD_VERSION     0x5F

#define CMD_COMBO_RESET 0x4C
#define CMD_COMBO_SET   0x5C
#define HUB_CMD_SPEED   0x5A
#define EXT_MODE_8      0x08

#define INFO_NAME        0x00
#define INFO_RAW         0x01
#define INFO_PCT         0x02
#define INFO_SI          0x03
#define INFO_UNIT        0x04
#define INFO_MAPPING     0x05
#define INFO_MODE_COMBOS 0x06
#define INFO_FORMAT      0x80

#define ABSOLUTE        16

#define DATA8           0x00
#define DATA16          0x01

#define ACK_TIMEOUT            2000
#define HEARTBEAT_PERIOD      10000
#define KEEPALIVE_TIMEOUT_MS   5000
#define MIN_SYNC_INTERVAL_MS     10
#define MAX_MODES                10

#define LPF2_NAME_LEN  12
#define LPF2_UNIT_LEN   8

// ── PoweredUpMode — one sensor mode descriptor ─────────────────────────────
class PoweredUpMode {
public:
    char  name[LPF2_NAME_LEN] = {0};
    byte  sample_size = 0;
    byte  data_type   = DATA8;
    byte  figures     = 0;
    byte  decimals    = 0;
    bool  view        = false;

    bool  ranges = false;
    float raw_low = 0, raw_high = 0;
    float pct_low = 0, pct_high = 0;
    float si_low  = 0, si_high  = 0;
    char  unit[LPF2_UNIT_LEN] = {0};

    bool  maps   = false;
    byte  mapin  = 0;
    byte  mapout = 0;

    bool  hasCallback = false;
    void (*myptr)(byte *data, byte len) = nullptr;
    void setCallback(void (*ptr)(byte *data, byte len));
};

// ── PoweredUpDevice — full LPF2 sensor protocol emulation ──────────────────
class PoweredUpDevice {
public:
    // uartPort: ESP-IDF UART_NUM_x. rxPin/txPin: GPIO numbers. sensorType: LPF2 type
    // byte (0x3D = Color Sensor). baud: operational rate after the handshake.
    PoweredUpDevice(int uartPort, int rxPin, int txPin, byte sensorType, uint32_t baud);

    void create_mode(const char *name, bool view,
                     byte dataType, byte sampleSize, byte figures, byte decimals);
    void create_mode(const char *name, bool view,
                     byte dataType, byte sampleSize, byte figures, byte decimals,
                     float rawLow, float rawHigh,
                     float pctLow, float pctHigh,
                     float siLow,  float siHigh,
                     const char *unit, byte mapIn = 0, byte mapOut = 0);

    void reset();          // run / retry the LPF2 handshake (blocks)
    void heart_beat();     // service the UART RX buffer; call every loop iteration

    // Transmit a DATA frame for the given mode. len must be 1,2,4,8,16,32.
    void send_data8_mode(byte *data, int len, byte mode);

    PoweredUpMode *get_mode(byte mode);
    byte get_current_mode();

    void set_combo_modes(uint16_t modeBitmask);
    void set_combo_callback(void (*cb)());
    bool isComboActive();
    void setComboActive(bool active);
    // Parse the hub's last CMD_COMBO_SET into (mode,dataset) pairs in request order.
    // Returns the pair count; fills modes[i]/datasets[i]. Empty if the hub hasn't
    // sent a combo-set yet — caller should fall back.
    int  getComboPairs(byte *modes, byte *datasets, int max);

    // Seed the combo state as if the hub had already sent CMD_COMBO_SET with these 9
    // payload bytes ({flags, index, pairs..., checksum}) and activate combo. Used to start
    // in the hub's standard combo from connect; a real CMD_COMBO_SET from the hub overwrites it.
    void seedComboSet(const byte payload[9]);

    bool isNackPending();
    void clearNack();
    bool isConnected();
    bool isSelectReceived();
    bool needsSend();
    void clearSend();
    void disconnect();

private:
    // Hardware
    uart_port_t   port;
    int           rxPin, txPin;
    uint32_t      baud;
    bool          uartInstalled = false;

    byte          sensorType;

    byte            modeCount = 0;
    byte            viewCount = 0;
    PoweredUpMode   modeArray[MAX_MODES];

    byte          status         = 0;
    bool          connected      = false;
    byte          currentMode    = 0;
    bool          selectReceived = false;
    uint32_t      lastNack       = 0;
    uint32_t      lastSyncTime   = 0;
    byte          extModeOffset  = 0;

    uint16_t      comboModeBitmask = 0;
    bool          pendingNack      = false;
    bool          comboActive      = false;
    void        (*comboCallback)() = nullptr;
    byte          comboSetPacket[10] = {};
    byte          comboSetPacketLen  = 0;

    // UART layer (ESP-IDF)
    void uart_open(uint32_t at_baud);
    void uart_close();
    int  available();            // buffered RX byte count
    int  read_nb();              // read one byte, non-blocking; -1 if none

    // Protocol helpers
    void send_cmd(byte cmd, byte *data, byte len);
    void send_byte(byte b);
    byte read_byte();            // read one byte, ~20 ms timeout; 0 on timeout
    void get_long(uint32_t value, byte *bb);
    int  payloadLLL(int len);
};
