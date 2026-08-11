// lpf2.cpp — LPF2 UART sensor protocol (ESP-IDF port of the Arduino PoweredUp library).
//
// Protocol behaviour mirrors the original byte-for-byte (handshake ordering, the magic
// capability bytes the hub validates, combo-mode framing). The Arduino I/O calls were
// replaced: Serial1 → driver/uart, pinMode/digitalWrite → driver/gpio, millis() →
// esp_timer, delay() → vTaskDelay.

#include "lpf2.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_RX_BUF 1024

// Runtime debug mask (see lpf2.h). 0 = silent. Set from the lego "debug" config.
volatile uint32_t lpf2_debug_mask = 0;
void lpf2_set_debug_mask(uint32_t mask) { lpf2_debug_mask = mask; }

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline void     delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Decode an LLL field to a payload byte count (2^LLL, clamped to 64).
static byte lllToLength(byte lll) {
    if (lll > 6) lll = 6;
    return (byte)(1 << lll);
}

// ── PoweredUpMode ──────────────────────────────────────────────────────────
void PoweredUpMode::setCallback(void (*ptr)(byte *, byte)) {
    myptr       = ptr;
    hasCallback = true;
}

// ── Construction & mode registration ───────────────────────────────────────
PoweredUpDevice::PoweredUpDevice(int uartPort, int rxPin, int txPin,
                                 byte sensorType, uint32_t baud) {
    this->port       = (uart_port_t)uartPort;
    this->rxPin      = rxPin;
    this->txPin      = txPin;
    this->sensorType = sensorType;
    this->baud       = baud;
}

void PoweredUpDevice::create_mode(const char *name, bool view,
                                  byte dataType, byte sampleSize,
                                  byte figures, byte decimals) {
    if (modeCount >= MAX_MODES) return;
    PoweredUpMode *m = &modeArray[modeCount++];
    *m = PoweredUpMode();
    strncpy(m->name, name, LPF2_NAME_LEN - 1);
    m->view        = view;
    m->data_type   = dataType;
    m->sample_size = sampleSize;
    m->figures     = figures;
    m->decimals    = decimals;
    if (view) viewCount++;
}

void PoweredUpDevice::create_mode(const char *name, bool view,
                                  byte dataType, byte sampleSize,
                                  byte figures, byte decimals,
                                  float rawLow, float rawHigh,
                                  float pctLow, float pctHigh,
                                  float siLow,  float siHigh,
                                  const char *unit, byte mapIn, byte mapOut) {
    if (modeCount >= MAX_MODES) return;
    PoweredUpMode *m = &modeArray[modeCount++];
    *m = PoweredUpMode();
    strncpy(m->name, name, LPF2_NAME_LEN - 1);
    m->view        = view;
    m->data_type   = dataType;
    m->sample_size = sampleSize;
    m->figures     = figures;
    m->decimals    = decimals;
    m->raw_low     = rawLow;
    m->raw_high    = rawHigh;
    m->pct_low     = pctLow;
    m->pct_high    = pctHigh;
    m->si_low      = siLow;
    m->si_high     = siHigh;
    strncpy(m->unit, unit, LPF2_UNIT_LEN - 1);
    m->ranges      = true;
    m->maps        = (mapIn != 0 || mapOut != 0);
    m->mapin       = mapIn;
    m->mapout      = mapOut;
    if (view) viewCount++;
}

// ── UART layer (ESP-IDF) ───────────────────────────────────────────────────
void PoweredUpDevice::uart_open(uint32_t at_baud) {
    uart_close();
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)at_baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    // Install WITH an event queue: the driver reports break / framing / parity errors
    // there, which is the only reliable way to tell "the hub stopped driving the line"
    // from "the hub sent 0x00" (they are the same byte — see heart_beat()).
    uart_driver_install(port, UART_RX_BUF, 0, LINK_ERR_EVT_QUEUE_LEN, &uartEvtQ, 0);
    uart_param_config(port, &cfg);
    uart_set_pin(port, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uartInstalled = true;

    // Fresh link state: the handshake itself (TX-low pulses, the 2400→operational baud
    // switch) legitimately produces break/framing errors, so anything from before this
    // point must not count against the connection we're about to bring up.
    linkErrMs     = 0;
    zeroWindowMs  = millis();
    zeroCount     = 0;
    sawValidFrame = false;
}

void PoweredUpDevice::uart_close() {
    if (uartInstalled) {
        uart_driver_delete(port);       // also frees the event queue
        uartEvtQ      = nullptr;
        uartInstalled = false;
    }
}

// ── Dead-line detection ────────────────────────────────────────────────────
void PoweredUpDevice::poll_uart_events() {
    if (!uartInstalled || !uartEvtQ) return;
    uart_event_t evt;
    while (xQueueReceive(uartEvtQ, &evt, 0) == pdTRUE) {
        switch (evt.type) {
        case UART_BREAK:
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            // Arm the grace timer (don't restart it — a dead line errors continuously,
            // and restarting on every event would push the deadline out forever).
            if (linkErrMs == 0) linkErrMs = millis();
            break;
        default:
            break;                       // UART_DATA etc. — the bytes are read via read_nb()
        }
    }
}

void PoweredUpDevice::hub_message() {
    lastNack  = millis();
    linkErrMs = 0;            // a real frame arrived: whatever the error was, the hub is alive
}

bool PoweredUpDevice::zero_flood(byte cmd) {
    uint32_t now = millis();
    if (now - zeroWindowMs >= ZERO_FLOOD_WINDOW_MS) {
        zeroWindowMs  = now;
        zeroCount     = 0;
        sawValidFrame = false;
    }
    if (cmd == 0x00) zeroCount++;
    else             sawValidFrame = true;
    return !sawValidFrame && zeroCount > ZERO_FLOOD_MAX;
}

int PoweredUpDevice::available() {
    if (!uartInstalled) return 0;
    size_t n = 0;
    uart_get_buffered_data_len(port, &n);
    return (int)n;
}

int PoweredUpDevice::read_nb() {
    if (!uartInstalled) return -1;
    uint8_t b;
    int n = uart_read_bytes(port, &b, 1, 0);
    return (n == 1) ? b : -1;
}

// ── Connection management ──────────────────────────────────────────────────
bool PoweredUpDevice::isConnected()       { return connected; }
bool PoweredUpDevice::isSelectReceived()  { return selectReceived; }
bool PoweredUpDevice::needsSend()         { return true; }
void PoweredUpDevice::clearSend()         {}

void PoweredUpDevice::disconnect() {
    uart_close();
    connected = false;
    status    = 0;
}

// Drive TX low as a plain GPIO for `ms`, then release high. Used for the reset
// signal and the operational-start pulse. The UART must be closed first; gpio_reset_pin
// detaches the pin from the UART signal matrix so the GPIO level actually takes effect
// (on a reconnect the pin is still routed to UART_TX otherwise, and the pulse is a no-op).
static void pulse_tx_low(int txPin, uint32_t ms) {
    gpio_reset_pin((gpio_num_t)txPin);
    gpio_set_direction((gpio_num_t)txPin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)txPin, 0);
    delay_ms(ms);
    gpio_set_level((gpio_num_t)txPin, 1);
    delay_ms(10);
}

void PoweredUpDevice::reset() {
    bool wasCombo = comboActive;
    const int MAX_RETRIES = 5;

    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        DBG_CONN_PRINT("Handshake attempt %d / %d\n", attempt, MAX_RETRIES);

        // Step 1: reset signal — hold TX low ≥ 500 ms.
        disconnect();
        delay_ms(50);
        pulse_tx_low(txPin, 600);

        // Step 2: open at handshake baud, flush stale bytes.
        uart_open(2400);
        delay_ms(50);
        uart_flush_input(port);

        // Step 3: sensor identity frames.
        send_byte(SYS_SYNC);
        delay_ms(10);

        byte buf[32] = {0};

        buf[0] = sensorType;
        send_cmd(CMD_TYPE, buf, 1);

        // CMD_MODES4 — [modeCount-1, viewCount-1, extModeCount, extViewCount].
        // The real Color Sensor (type 0x3D) sends [07, 07, 09, 00]; the hub validates
        // against it. byte[2]/[3] describe the "mode 8+" extended page; a device with
        // ≤8 modes and no extended page (the matrix) must report 0 there, not the colour
        // sensor's 0x09 — otherwise the hub waits for INFO frames that never come.
        bool isColorSensor = (sensorType == 0x3D);
        buf[0] = modeCount - 1;
        buf[1] = viewCount - 1;
        buf[2] = isColorSensor ? 0x09 : 0x00;
        buf[3] = 0x00;
        send_cmd(CMD_MODES4, buf, 4);

        get_long(baud, buf);
        send_cmd(CMD_SPEED, buf, 4);

        // Version: firmware 1.0.0.0, hardware 1.0.0.0 (LPF2 BCD, 0x10000000 LE).
        get_long(0x10000000UL, buf);
        get_long(0x10000000UL, buf + 4);
        send_cmd(CMD_VERSION, buf, 8);

        // Step 4: INFO frames per mode, DESCENDING order (the hub validates this).
        for (int i = modeCount - 1; i >= 0; i--) {
            PoweredUpMode *mode = get_mode(i);
            if (!mode) continue;

            // NAME — 17 payload bytes matching the real Color Sensor capture:
            //   [0]=INFO_NAME, [1..]=name, [8]=0x40, [12]=0x04, [13]=0x84.
            byte namePayload[17] = {0};
            namePayload[0] = INFO_NAME;
            strncpy((char *)&namePayload[1], mode->name, 11);
            namePayload[8]  = 0x40;
            namePayload[12] = 0x04;
            namePayload[13] = 0x84;
            send_cmd(MSG_CLASS_INFO | (4 << LLL_SHIFT) | i, namePayload, 17);
            DBG_CONN_PRINT("INFO mode=%d NAME='%s'\n", i, mode->name);

            // RAW / PCT / SI — each a 9-byte payload: sub-type + two LE floats.
            if (mode->ranges) {
                union { uint32_t asLong; float asFloat; } cvt;
                byte rangePayload[9];
                auto sendRange = [&](byte subType, float low, float high) {
                    rangePayload[0] = subType;
                    cvt.asFloat = low;  get_long(cvt.asLong, rangePayload + 1);
                    cvt.asFloat = high; get_long(cvt.asLong, rangePayload + 5);
                    send_cmd(MSG_CLASS_INFO | (3 << LLL_SHIFT) | i, rangePayload, 9);
                };
                sendRange(INFO_RAW, mode->raw_low, mode->raw_high);
                sendRange(INFO_PCT, mode->pct_low, mode->pct_high);
                sendRange(INFO_SI,  mode->si_low,  mode->si_high);
            }

            // UNIT — sub-type + 4-char unit string, zero-padded.
            {
                byte unitPayload[5] = {INFO_UNIT, 0, 0, 0, 0};
                int ulen = (int)strlen(mode->unit);
                if (ulen > 4) ulen = 4;
                memcpy(&unitPayload[1], mode->unit, ulen);
                send_cmd(MSG_CLASS_INFO | (2 << LLL_SHIFT) | i, unitPayload, 5);
            }

            // MAPPING — sub-type + in-bitmask + out-bitmask.
            if (mode->maps) {
                byte mapPayload[3] = {INFO_MAPPING, mode->mapin, mode->mapout};
                send_cmd(MSG_CLASS_INFO | (1 << LLL_SHIFT) | i, mapPayload, 3);
            }

            // FORMAT — sub-type + sample count + data type + figures + decimals.
            byte fmtPayload[5] = {
                INFO_FORMAT, mode->sample_size, mode->data_type,
                mode->figures, mode->decimals
            };
            send_cmd(MSG_CLASS_INFO | (2 << LLL_SHIFT) | i, fmtPayload, 5);

            delay_ms(10);
        }

        // Step 4b: MODE_COMBOS + the trailing INFO frame (mode 0 epilogue).
        if (comboModeBitmask != 0) {
            byte comboPayload[3] = {
                INFO_MODE_COMBOS,
                (byte)(comboModeBitmask & 0xFF),
                (byte)((comboModeBitmask >> 8) & 0xFF)
            };
            send_cmd(MSG_CLASS_INFO | (1 << LLL_SHIFT) | 0, comboPayload, 3);
            DBG_CONN_PRINT("INFO mode=0 MODE_COMBOS bitmask=0x%04X\n", comboModeBitmask);
        }

        // Trailing INFO mode-0 frame (subtype 0x08) from real Color Sensor captures — encodes
        // the colour sensor's device id/serial ("G92539"), so it is 0x3D-specific. (Tested:
        // re-sending it for the 0x40 matrix changed nothing — the hub stays silent regardless,
        // i.e. it rejects the matrix at descriptor validation, not for a missing terminator.)
        if (isColorSensor) {
            byte unkPayload[17] = {
                0x08, 0x00, 0x3C, 0x00, 0x31, 0x0A, 0x47, 0x39,
                0x32, 0x35, 0x33, 0x39, 0x39, 0x00, 0x00, 0x00, 0x00
            };
            send_cmd(MSG_CLASS_INFO | (4 << LLL_SHIFT) | 0, unkPayload, 17);
        }
        delay_ms(10);

        // Step 5: ACK, then consume HUB_CMD_SPEED at 2400, switch baud, await ACK.
        send_byte(SYS_ACK);

        bool gotAck       = false;
        bool baudSwitched = false;
        uint32_t deadline = millis() + ACK_TIMEOUT;
        while (millis() < deadline) {
            if (available() <= 0) { delay_ms(2); continue; }
            int rxi = read_nb();
            if (rxi < 0) { delay_ms(2); continue; }
            byte rx = (byte)rxi;
            DBG_RX_PRINT("ACK wait: 0x%02X\n", rx);

            if (rx == SYS_ACK) { gotAck = true; break; }

            if (rx == HUB_CMD_SPEED && !baudSwitched) {
                for (int i = 0; i < 9; i++) read_byte();   // 8 payload + 1 checksum
                send_byte(SYS_ACK);
                uart_wait_tx_done(port, pdMS_TO_TICKS(50));
                DBG_RX_PRINT("CMD_SPEED consumed — switching to %lu baud\n",
                             (unsigned long)baud);
                uart_set_baudrate(port, baud);
                delay_ms(30);
                uart_flush_input(port);
                baudSwitched = true;
                deadline = millis() + 500;
            }
        }

        if (!gotAck) {
            DBG_CONN_PRINT("ACK timeout — retrying\n");
            continue;
        }

        // Step 6: signal clean operational start — pulse TX low, reopen at baud.
        uart_close();
        delay_ms(50);
        pulse_tx_low(txPin, 10);
        uart_open(baud);
        delay_ms(50);
        uart_flush_input(port);

        lastNack       = millis();
        connected      = true;
        selectReceived = false;
        comboActive    = wasCombo;
        DBG_CONN_PRINT("Connected at %lu baud (comboActive=%d)\n",
                       (unsigned long)baud, comboActive);
        return;
    }

    DBG_CONN_PRINT("FATAL: handshake failed after %d attempts\n", MAX_RETRIES);
    connected = false;
}

// ── heart_beat() — service the RX buffer ───────────────────────────────────
void PoweredUpDevice::heart_beat() {
    // A hub switched off mid-session stops driving RX; the driver reports that as a
    // break/framing error while still handing us 0x00 bytes, which are indistinguishable
    // from SYS_SYNC further down and would keep refreshing the keepalive forever. Trust
    // the error events, not the byte values.
    poll_uart_events();
    if (connected && linkErrMs != 0 && (millis() - linkErrMs) > LINK_ERR_GRACE_MS) {
        DBG_CONN_PRINT("RX break/framing error, no hub traffic for %u ms — link down, disconnecting\n",
                       (unsigned)LINK_ERR_GRACE_MS);
        disconnect();
        return;
    }

    if (connected && (millis() - lastNack) > KEEPALIVE_TIMEOUT_MS) {
        DBG_CONN_PRINT("Keepalive timeout — disconnecting\n");
        disconnect();
        return;
    }

    while (available() > 0) {
        int ci = read_nb();
        if (ci < 0) break;
        byte cmd = (byte)ci;

        // Backstop for a dead line the error events didn't catch (event queue overflow,
        // or a floating rather than low line): a real hub sends SYNC occasionally, a dead
        // line delivers 0x00 at the full line rate.
        if (connected && zero_flood(cmd)) {
            DBG_CONN_PRINT("0x00 flood (%u in %u ms) — hub link down, disconnecting\n",
                           (unsigned)zeroCount, (unsigned)ZERO_FLOOD_WINDOW_MS);
            disconnect();
            return;
        }

        // System tokens.
        if (cmd == SYS_SYNC) {
            send_byte(SYS_ACK);
            uint32_t now = millis();
            if (now - lastSyncTime >= MIN_SYNC_INTERVAL_MS) {
                // Feeds the keepalive (an idle hub may send nothing else) but deliberately
                // not hub_message(): SYNC is 0x00, byte-identical to what an undriven line
                // delivers, so it must never disarm the break-error grace timer.
                lastNack     = now;
                lastSyncTime = now;
            }
            continue;
        }
        if (cmd == SYS_NACK) {
            DBG_RX_PRINT("NACK\n");
            hub_message();
            pendingNack = true;
            if (comboActive && comboCallback) {
                comboCallback();
                pendingNack = false;
            }
            continue;
        }
        if (cmd == SYS_ACK) {
            hub_message();
            continue;
        }

        // Combo-mode packets (special CMD-class frames).
        if (cmd == CMD_COMBO_RESET) {
            byte b[3];
            for (int i = 0; i < 3; i++) b[i] = read_byte();
            comboActive = false;
            byte ackBuf[1] = { 0x20 };
            send_cmd(0x44, ackBuf, 1);
            DBG_RX_PRINT("COMBO RESET\n");
            DBG_EVT_PRINT("hub COMBO RESET (combined modes off)\n");
            hub_message();
            continue;
        }
        if (cmd == CMD_COMBO_SET) {
            byte b[9];
            for (int i = 0; i < 9; i++) b[i] = read_byte();
            comboActive = true;
            comboSetPacket[0] = cmd;
            for (int i = 0; i < 9; i++) comboSetPacket[i + 1] = b[i];
            comboSetPacketLen = 10;
            send_cmd(cmd, b, 9);
            DBG_RX_PRINT("COMBO SET\n");
            DBG_EVT_PRINT("hub COMBO SET (combined modes on)\n");
            hub_message();
            continue;
        }

        // Framed CMD or DATA messages.
        if ((cmd & MSG_CLASS_MASK) == MSG_CLASS_CMD ||
            (cmd & MSG_CLASS_MASK) == MSG_CLASS_DATA) {

            byte modeIndex   = cmd & MMM_MASK;
            byte lll         = (cmd >> LLL_SHIFT) & 0x07;
            byte payloadSize = lllToLength(lll);

            if (cmd == CMD_EXT_MODE) {
                byte extMode = read_byte();
                read_byte();   // checksum (not verified)
                extModeOffset = extMode;
                hub_message();
                continue;
            }

            if (cmd == CMD_SELECT) {
                byte newMode  = read_byte();
                byte checksum = read_byte();
                if (checksum == (byte)(0xFF ^ cmd ^ newMode)) {
                    currentMode    = newMode;
                    selectReceived = true;
                    DBG_RX_PRINT("SELECT mode=%d\n", newMode);
                    DBG_EVT_PRINT("hub SELECT -> mode %d\n", newMode);
                }
                hub_message();
                continue;
            }

            // WRITE — read payload and verify checksum.
            byte expectedCk = 0xFF ^ cmd;
            byte payload[64] = {0};
            for (byte i = 0; i < payloadSize; i++) {
                payload[i]  = read_byte();
                expectedCk ^= payload[i];
            }
            byte receivedCk = read_byte();

            if (receivedCk != expectedCk) {
                DBG_RX_PRINT("WRITE checksum mismatch\n");
                // Corrupted frame: someone is talking, so keep the keepalive fed, but not
                // hub_message() — corruption is exactly what a failing line produces, so it
                // must not disarm the break-error grace timer.
                lastNack = millis();
                continue;
            }

            byte targetMode = modeIndex + extModeOffset;
            extModeOffset = 0;
            selectReceived = true;

            DBG_EVT_PRINT("hub WRITE -> mode %d (%d bytes)\n", targetMode, payloadSize);
            PoweredUpMode *m = get_mode(targetMode);
            if (m && m->hasCallback) m->myptr(payload, payloadSize);
            hub_message();

        } else {
            // Unknown byte — UART noise (e.g. baud transition). Deliberately does NOT
            // refresh lastNack: noise is not evidence that a hub is still there, and
            // treating it as such is what let a powered-off hub hold the link "up"
            // indefinitely (nothing then re-ran the handshake, so the device stayed
            // invisible when the hub came back). Genuine hub traffic — NACK/ACK/SELECT/
            // WRITE/COMBO above — is the only thing that feeds the watchdog now.
            DBG_RX_PRINT("noise byte 0x%02X (ignored)\n", cmd);
        }
    }
}

// ── Data transmission ──────────────────────────────────────────────────────
void PoweredUpDevice::send_data8_mode(byte *data, int len, byte mode) {
    send_cmd(MSG_CLASS_DATA | (payloadLLL(len) << LLL_SHIFT) | mode, data, len);
}

// ── Low-level helpers ──────────────────────────────────────────────────────

// Hex-dump a transmitted frame (gated by DBG_TX). Matches the original's
// "[TX] cmd=.. len=.. ck=.. data:.." format used by docs/debugging.md.
static void dbg_tx_frame(byte cmd, const byte *data, byte len, byte checksum) {
#if LPF2_DEBUG_BUILD
    if (!(lpf2_debug_mask & DBG_TX)) return;
    printf("[LPF2] [TX]     cmd=0x%02X len=%d ck=0x%02X data:", cmd, len, checksum);
    for (int i = 0; i < len; i++) printf(" %02X", data[i]);
    printf("\n");
#else
    (void)cmd; (void)data; (void)len; (void)checksum;
#endif
}

void PoweredUpDevice::send_cmd(byte cmd, byte *data, byte len) {
    byte frame[1 + 64 + 1];
    if (len > 64) len = 64;
    byte checksum = 0xFF ^ cmd;
    frame[0] = cmd;
    for (int i = 0; i < len; i++) {
        checksum ^= data[i];
        frame[1 + i] = data[i];
    }
    frame[1 + len] = checksum;
    if (uartInstalled) uart_write_bytes(port, (const char *)frame, len + 2);
    dbg_tx_frame(cmd, data, len, checksum);
}

void PoweredUpDevice::send_byte(byte b) {
    if (uartInstalled) uart_write_bytes(port, (const char *)&b, 1);
    DBG_TX_PRINT("byte=0x%02X\n", b);
}

byte PoweredUpDevice::read_byte() {
    if (!uartInstalled) return 0;
    uint8_t b;
    int n = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(20));
    return (n == 1) ? b : 0;
}

void PoweredUpDevice::get_long(uint32_t value, byte *bb) {
    for (int i = 0; i < 4; i++) bb[i] = (value >> (i * 8)) & 0xFF;
}

int PoweredUpDevice::payloadLLL(int len) {
    switch (len) {
        case 1:  return 0;
        case 2:  return 1;
        case 4:  return 2;
        case 8:  return 3;
        case 16: return 4;
        case 32: return 5;
        default: return 0;
    }
}

PoweredUpMode *PoweredUpDevice::get_mode(byte mode) {
    if (mode >= MAX_MODES || mode >= modeCount) return nullptr;
    return &modeArray[mode];
}

byte PoweredUpDevice::get_current_mode() { return currentMode; }

void PoweredUpDevice::set_combo_modes(uint16_t modeBitmask) { comboModeBitmask = modeBitmask; }
void PoweredUpDevice::set_combo_callback(void (*cb)())      { comboCallback = cb; }
bool PoweredUpDevice::isComboActive()        { return comboActive; }
void PoweredUpDevice::setComboActive(bool a) { comboActive = a; }

void PoweredUpDevice::seedComboSet(const byte payload[9]) {
    comboSetPacket[0] = CMD_COMBO_SET;
    for (int i = 0; i < 9; i++) comboSetPacket[i + 1] = payload[i];
    comboSetPacketLen = 10;
    comboActive = true;
}

// comboSetPacket = { CMD_COMBO_SET, (0x20|num_pairs), combo_index, pair0, pair1, ... }
// each pair byte = (mode << 4) | dataset. See pybricks technical-info/uart-protocol.md.
int PoweredUpDevice::getComboPairs(byte *modes, byte *datasets, int max) {
    if (comboSetPacketLen < 3) return 0;
    int n = comboSetPacket[1] & 0x0F;        // low nibble = number of pairs
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        int idx = 3 + i;                      // pairs start after cmd, flags, combo-index
        if (idx >= comboSetPacketLen) break;
        byte p = comboSetPacket[idx];
        modes[out]    = (p >> 4) & 0x0F;
        datasets[out] = p & 0x0F;
        out++;
    }
    return out;
}
bool PoweredUpDevice::isNackPending()        { return pendingNack; }
void PoweredUpDevice::clearNack()            { pendingNack = false; }
