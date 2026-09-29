/*
 * CCCP - Cardputer Communication Connector for PocketPostPet
 * Copyright (c) 2026 Layer812
 * SPDX-License-Identifier: MIT
 */

#include <Arduino.h>
#include <M5Cardputer.h>
#include <USB.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>
#include <stdarg.h>
#undef KEY_BACKSPACE
#undef KEY_TAB

namespace {
constexpr int kIrRxPin = 1;       // Grove white / G1, connect to TTL-IR TXD.
constexpr int kIrTxPin = 2;       // Grove yellow / G2, connect to TTL-IR RXD.
constexpr uint32_t kBaudRates[] = {9600, 19200, 38400, 57600, 115200};

constexpr int kScreenW = 240;
constexpr int kScreenH = 135;
constexpr int kCharW = 6;
constexpr int kLineH = 8;
constexpr int kTop = 14;
constexpr int kBottom = 10;
constexpr int kCols = kScreenW / kCharW;
constexpr int kRows = (kScreenH - kTop - kBottom) / kLineH;
constexpr size_t kRxLogSize = 8192;
constexpr size_t kIrRxBufferSize = 2048;
constexpr size_t kIrCommTxQueueSize = 2048;
constexpr size_t kIrdaFrameMax = 256;
constexpr size_t kParserLogSize = 32768;
constexpr size_t kSerialDumpChunk = 256;
constexpr size_t kIrInputChunkBytes = 192;
constexpr uint32_t kDisplayRefreshIntervalMs = 8;
constexpr uint32_t kXidResponseDelayMs = 2;
constexpr uint32_t kUaResponseDelayMs = 5;
constexpr uint8_t kCardputerIrdaAddress[4] = {0x43, 0x50, 0x49, 0x52};
constexpr uint8_t kIrdaHintComputer = 0x04;
constexpr uint8_t kIrdaHintExtension = 0x80;
constexpr uint8_t kIrdaHintIrComm = 0x04;
constexpr uint8_t kIrdaCharsetAscii = 0x00;
constexpr char kCardputerIrdaName[] = "Cardputer IR";
constexpr char kFirmwareTag[] = "R2E-PPP115";
constexpr uint8_t kIrCommLsap = 0x02;
constexpr uint8_t kIrCommAdditionalCredit = 0x7f;
constexpr size_t kTelnetInputChunkBytes = 256;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kIrCommTxPokeIntervalMs = 20;
constexpr int kWifiScanMaxItems = 12;
constexpr size_t kSshStreamBufferSize = 4096;
constexpr size_t kSshIoChunkSize = 256;
constexpr uint32_t kSshTaskStackSize = 16 * 1024;
constexpr uint32_t kSshStatusDisplayMs = 900;
constexpr char kNetPrefsNamespace[] = "net";
constexpr char kNetPrefsSsidKey[] = "ssid";
constexpr char kNetPrefsPasswordKey[] = "pass";
constexpr char kNetPrefsTelnetHostKey[] = "thost";
constexpr char kNetPrefsTelnetPortKey[] = "tport";
constexpr char kNetPrefsSshHostKey[] = "shost";
constexpr char kNetPrefsSshPortKey[] = "sport";
constexpr char kNetPrefsSshUserKey[] = "suser";
constexpr char kNetPrefsSshPasswordKey[] = "spass";
constexpr char kNetPrefsSshPrivateKeyKey[] = "skey";
constexpr size_t kUsbCommandLineMax = 160;
constexpr size_t kSshPrivateKeyMax = 6144;
constexpr uint8_t kTelnetIac = 255;
constexpr uint8_t kTelnetWill = 251;
constexpr uint8_t kTelnetWont = 252;
constexpr uint8_t kTelnetDo = 253;
constexpr uint8_t kTelnetDont = 254;
constexpr char kKeyArrowUp = ';';
constexpr char kKeyArrowDown = '.';
constexpr uint8_t kIrCommInitialControlParams[] = {
    0x00, 0x01, 0x02,             // Service type: 3-Wire cooked.
    0x10, 0x04, 0x00, 0x01, 0xC2, 0x00, // Data rate: 115200 (0x0001C200, big-endian).
    0x11, 0x01, 0x03,             // Data format: 8N1, mirrored from Psion.
    0x12, 0x01, 0x00,             // Flow control: disabled, mirrored from Psion.
    0x13, 0x02, 0x00, 0x00,       // XON/XOFF disabled, mirrored from Psion.
    0x14, 0x02, 0x00, 0x00,       // ENQ/ACK disabled, mirrored from Psion.
};
constexpr uint8_t kIrdaQos[] = {
    0x01, 0x01, 0x20, // Baud rate: 115200 only (IrLAP QoS IR_115200).
    0x82, 0x01, 0x01, // Max turnaround time: 500 ms.
    0x83, 0x01, 0x01, // Data size: 64 bytes.
    0x84, 0x01, 0x01, // Window size: 1.
    0x85, 0x01, 0x40, // Additional BOFs: 1.
    0x86, 0x01, 0x01, // Minimum turnaround time: 10000 us.
    0x08, 0x01, 0x04, // Link disconnect threshold: 12 s.
};

HardwareSerial IrSerial(1);
WiFiClient TelnetClient;
Preferences NetPrefs;
TaskHandle_t SshTaskHandle = nullptr;
StreamBufferHandle_t SshOutbound = nullptr;
StreamBufferHandle_t SshInbound = nullptr;

char screen[kRows][kCols];
char psionScreen[kRows][kCols];
char parserLog[kParserLogSize];
uint8_t rxLog[kRxLogSize];
uint8_t irdaFrame[kIrdaFrameMax];
int cursorX = 0;
int cursorY = 0;
int psionCursorX = 0;
int psionCursorY = 0;
size_t rxLogStart = 0;
size_t rxLogLen = 0;
size_t parserLogLen = 0;
size_t irdaFrameLen = 0;
uint32_t irdaFrameCount = 0;
uint32_t irdaCrcOkCount = 0;
uint32_t irdaCrcBadCount = 0;
bool localEcho = true;
bool hexView = false;
enum class IrLinkMode : uint8_t { RawSir, IrdaIrComm };
IrLinkMode irLinkMode = IrLinkMode::RawSir;
bool irdaParserView = false;
bool psionScreenView = false;
bool lastWasCr = false;
bool irdaInFrame = false;
bool irdaEscaped = false;
bool irdaFrameOverflow = false;
size_t baudIndex = 4; // R2E PocketPostPet proof: start at 115200
enum class PsionTermState : uint8_t { Normal, Esc, Csi };
PsionTermState psionTermState = PsionTermState::Normal;
int psionCsiParam = 0;
bool psionCsiHasParam = false;
uint8_t irlapVr = 0;
uint8_t irlapVs = 0;
uint8_t lastTxIInfo[64];
size_t lastTxIInfoLen = 0;
bool irCommConnected = false;
uint8_t irCommTxCredit = 0;
uint8_t irCommRemoteLsap = 0x01;
uint8_t irCommLocalLsap = kIrCommLsap;
uint8_t irCommTxQueue[kIrCommTxQueueSize];
size_t irCommTxQueueStart = 0;
size_t irCommTxQueueLen = 0;
uint8_t irCommPeerAddress = 0;
uint8_t irCommPeerNr = 0;
bool irCommPeerSeen = false;
bool irCommResponseWindowOpen = false;
uint8_t lastIrCommRxData[32];
size_t lastIrCommRxDataLen = 0;
uint8_t rxRrSuppressAddress = 0;
uint8_t rxRrSuppressNr = 0;
uint32_t rxRrSuppressCount = 0;
uint8_t txRrSuppressAddress = 0;
uint8_t txRrSuppressNr = 0;
uint32_t txRrSuppressCount = 0;
uint32_t screenDirtyRows = 0;
uint32_t psionDirtyRows = 0;
uint32_t lastTerminalDrawMs = 0;
uint32_t lastIrCommTxPokeMs = 0;
bool terminalRedrawPending = false;
bool psionPayloadDrawPending = false;
bool telnetBridgeEnabled = false;
bool telnetBridgeConnected = false;
bool telnetIacPending = false;
bool telnetCommandPending = false;
uint8_t telnetPendingCommand = 0;
volatile bool sshBridgeEnabled = false;
volatile bool sshBridgeConnected = false;
volatile bool sshStopRequested = false;
volatile bool sshStatusPending = false;
bool sshStatusShowing = false;
uint32_t sshStatusShownAtMs = 0;
String configuredWifiSsid;
String configuredWifiPassword;
String configuredTelnetHost;
uint16_t configuredTelnetPort = 23;
String configuredSshHost;
uint16_t configuredSshPort = 22;
String configuredSshUser;
String configuredSshPassword;
String configuredSshPrivateKey;
char sshStatusMessage[96] = "";
char usbCommandLine[kUsbCommandLineMax];
size_t usbCommandLineLen = 0;
bool usbCommandLineActive = false;
bool usbSshKeyCapture = false;
bool bridgeInputLastWasCr = false;
String usbSshKeyBuffer;
bool cursorDrawn = false;
bool cursorDrawnPsionView = false;
int cursorDrawnX = 0;
int cursorDrawnY = 0;

bool irdaModeEnabled() {
    return irLinkMode == IrLinkMode::IrdaIrComm;
}

uint32_t allRowsMask() {
    return (1UL << kRows) - 1;
}

void stopTelnetBridge(const char *message = nullptr);
void stopSshBridge(const char *message = nullptr);
void resetPsionTermParser();
void resetIrdaParser();
void waitForKeyboardRelease();

void writeUsbByteNonBlocking(uint8_t value) {
    if (Serial.availableForWrite() > 0) {
        Serial.write(value);
    }
}

void scheduleTerminalRedraw(bool forceFull = false) {
    if (forceFull) {
        if (psionScreenView) {
            psionDirtyRows = allRowsMask();
        } else {
            screenDirtyRows = allRowsMask();
        }
    }
    terminalRedrawPending = true;
}

void markTerminalRowDirty(bool psion, int row) {
    if (row < 0 || row >= kRows) {
        return;
    }
    if (psion) {
        psionDirtyRows |= 1UL << row;
    } else {
        screenDirtyRows |= 1UL << row;
    }
    if (psion == psionScreenView) {
        terminalRedrawPending = true;
    }
}

void markAllTerminalRowsDirty(bool psion) {
    if (psion) {
        psionDirtyRows = allRowsMask();
    } else {
        screenDirtyRows = allRowsMask();
    }
    if (psion == psionScreenView) {
        terminalRedrawPending = true;
    }
}

void clearBuffer() {
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            screen[row][col] = ' ';
        }
    }
    cursorX = 0;
    cursorY = 0;
    markAllTerminalRowsDirty(false);
}

void clearPsionBuffer() {
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            psionScreen[row][col] = ' ';
        }
    }
    psionCursorX = 0;
    psionCursorY = 0;
    resetPsionTermParser();
    markAllTerminalRowsDirty(true);
}

void clearRxLog() {
    rxLogStart = 0;
    rxLogLen = 0;
}

void clearParserLog() {
    parserLogLen = 0;
    parserLog[0] = '\0';
    irdaFrameCount = 0;
    irdaCrcOkCount = 0;
    irdaCrcBadCount = 0;
    rxRrSuppressCount = 0;
    txRrSuppressCount = 0;
}

void appendRxLog(uint8_t value) {
    if (rxLogLen < kRxLogSize) {
        rxLog[(rxLogStart + rxLogLen) % kRxLogSize] = value;
        ++rxLogLen;
        return;
    }

    rxLog[rxLogStart] = value;
    rxLogStart = (rxLogStart + 1) % kRxLogSize;
}

void appendParserLogLine(const char *line) {
    if (psionScreenView) {
        return;
    }

    size_t lineLen = strlen(line);
    if (lineLen + 2 >= kParserLogSize) {
        line += lineLen - (kParserLogSize - 2);
        lineLen = strlen(line);
    }

    while (parserLogLen + lineLen + 2 >= kParserLogSize && parserLogLen > 0) {
        size_t drop = 0;
        while (drop < parserLogLen && parserLog[drop] != '\n') {
            ++drop;
        }
        if (drop < parserLogLen) {
            ++drop;
        }
        memmove(parserLog, parserLog + drop, parserLogLen - drop);
        parserLogLen -= drop;
        parserLog[parserLogLen] = '\0';
    }

    memcpy(parserLog + parserLogLen, line, lineLen);
    parserLogLen += lineLen;
    parserLog[parserLogLen++] = '\n';
    parserLog[parserLogLen] = '\0';
}

void drawStatus() {
    auto &display = M5Cardputer.Display;
    display.fillRect(0, 0, kScreenW, kTop, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setCursor(2, 3);
    const char *link = irdaModeEnabled() ? "IDA" : "SIR";
    const char *mode = psionScreenView ? "PSN" : (irdaParserView ? "LOG" : (hexView ? "HEX" : "TXT"));
    const char *net = telnetBridgeConnected ? "T+" : (telnetBridgeEnabled ? "T." : "T-");
    const char *ssh = sshBridgeConnected ? "S+" : (sshBridgeEnabled ? "S." : "S-");
    display.printf("%s %lu %s E:%s %s %s", link, kBaudRates[baudIndex], mode, localEcho ? "On" : "Off", net, ssh);
}

void redrawTerminal() {
    auto &display = M5Cardputer.Display;
    display.fillRect(0, kTop, kScreenW, kScreenH - kTop, TFT_BLACK);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    char (*activeScreen)[kCols] = psionScreenView ? psionScreen : screen;
    int activeCursorX = psionScreenView ? psionCursorX : cursorX;
    int activeCursorY = psionScreenView ? psionCursorY : cursorY;

    for (int row = 0; row < kRows; ++row) {
        display.setCursor(0, kTop + row * kLineH);
        for (int col = 0; col < kCols; ++col) {
            display.write(activeScreen[row][col]);
        }
    }

    int cursorPixelX = activeCursorX * kCharW;
    int cursorPixelY = kTop + activeCursorY * kLineH + kLineH - 1;
    display.drawFastHLine(cursorPixelX, cursorPixelY, kCharW, TFT_GREEN);
    if (psionScreenView) {
        psionDirtyRows = 0;
    } else {
        screenDirtyRows = 0;
    }
    cursorDrawn = true;
    cursorDrawnPsionView = psionScreenView;
    cursorDrawnX = activeCursorX;
    cursorDrawnY = activeCursorY;
    terminalRedrawPending = false;
    lastTerminalDrawMs = millis();
}

void refreshTerminalIfNeeded(bool ignoreInterval = false) {
    if (!ignoreInterval && (!terminalRedrawPending || millis() - lastTerminalDrawMs < kDisplayRefreshIntervalMs)) {
        return;
    }

    auto &display = M5Cardputer.Display;
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    char (*activeScreen)[kCols] = psionScreenView ? psionScreen : screen;
    int activeCursorX = psionScreenView ? psionCursorX : cursorX;
    int activeCursorY = psionScreenView ? psionCursorY : cursorY;
    uint32_t &dirtyRows = psionScreenView ? psionDirtyRows : screenDirtyRows;

    if (cursorDrawn && cursorDrawnPsionView == psionScreenView) {
        int oldCursorPixelX = cursorDrawnX * kCharW;
        int oldCursorPixelY = kTop + cursorDrawnY * kLineH + kLineH - 1;
        display.drawFastHLine(oldCursorPixelX, oldCursorPixelY, kCharW, TFT_BLACK);
    }

    for (int row = 0; row < kRows; ++row) {
        if ((dirtyRows & (1UL << row)) == 0) {
            continue;
        }
        int y = kTop + row * kLineH;
        display.fillRect(0, y, kScreenW, kLineH, TFT_BLACK);
        display.setCursor(0, y);
        for (int col = 0; col < kCols; ++col) {
            display.write(activeScreen[row][col]);
        }
    }

    int cursorPixelX = activeCursorX * kCharW;
    int cursorPixelY = kTop + activeCursorY * kLineH + kLineH - 1;
    display.drawFastHLine(cursorPixelX, cursorPixelY, kCharW, TFT_GREEN);
    dirtyRows = 0;
    cursorDrawn = true;
    cursorDrawnPsionView = psionScreenView;
    cursorDrawnX = activeCursorX;
    cursorDrawnY = activeCursorY;
    terminalRedrawPending = false;
    lastTerminalDrawMs = millis();
}

void scrollUp() {
    for (int row = 1; row < kRows; ++row) {
        memcpy(screen[row - 1], screen[row], kCols);
    }
    memset(screen[kRows - 1], ' ', kCols);
    cursorY = kRows - 1;
    markAllTerminalRowsDirty(false);
}

void newline() {
    cursorX = 0;
    ++cursorY;
    if (cursorY >= kRows) {
        scrollUp();
    }
    scheduleTerminalRedraw();
}

void backspace() {
    if (cursorX > 0) {
        --cursorX;
    } else if (cursorY > 0) {
        --cursorY;
        cursorX = kCols - 1;
    }
    screen[cursorY][cursorX] = ' ';
    markTerminalRowDirty(false, cursorY);
}

void psionScrollUp() {
    for (int row = 1; row < kRows; ++row) {
        memcpy(psionScreen[row - 1], psionScreen[row], kCols);
    }
    memset(psionScreen[kRows - 1], ' ', kCols);
    psionCursorY = kRows - 1;
    markAllTerminalRowsDirty(true);
}

void psionNewline() {
    psionCursorX = 0;
    ++psionCursorY;
    if (psionCursorY >= kRows) {
        psionScrollUp();
    }
    scheduleTerminalRedraw();
}

void psionBackspace() {
    if (psionCursorX > 0) {
        --psionCursorX;
    } else if (psionCursorY > 0) {
        --psionCursorY;
        psionCursorX = kCols - 1;
    }
    psionScreen[psionCursorY][psionCursorX] = ' ';
    markTerminalRowDirty(true, psionCursorY);
}

void psionMoveLeft(int count = 1) {
    while (count-- > 0) {
        if (psionCursorX > 0) {
            --psionCursorX;
        } else if (psionCursorY > 0) {
            --psionCursorY;
            psionCursorX = kCols - 1;
        }
    }
    scheduleTerminalRedraw();
}

void psionMoveRight(int count = 1) {
    while (count-- > 0) {
        ++psionCursorX;
        if (psionCursorX >= kCols) {
            psionCursorX = 0;
            if (psionCursorY + 1 < kRows) {
                ++psionCursorY;
            } else {
                psionScrollUp();
            }
        }
    }
    scheduleTerminalRedraw();
}

void psionMoveVertical(int delta, int count = 1) {
    psionCursorY += delta * count;
    if (psionCursorY < 0) {
        psionCursorY = 0;
    }
    if (psionCursorY >= kRows) {
        psionCursorY = kRows - 1;
    }
    scheduleTerminalRedraw();
}

void psionEraseToEndOfLine() {
    memset(psionScreen[psionCursorY] + psionCursorX, ' ', kCols - psionCursorX);
    markTerminalRowDirty(true, psionCursorY);
}

void psionEraseToEndOfScreen() {
    psionEraseToEndOfLine();
    for (int row = psionCursorY + 1; row < kRows; ++row) {
        memset(psionScreen[row], ' ', kCols);
        markTerminalRowDirty(true, row);
    }
}

void psionEraseScreen() {
    clearPsionBuffer();
}

void resetPsionTermParser() {
    psionTermState = PsionTermState::Normal;
    psionCsiParam = 0;
    psionCsiHasParam = false;
}

void handlePsionCsiFinal(char finalByte) {
    int count = psionCsiHasParam ? psionCsiParam : 1;
    if (count < 1) {
        count = 1;
    }

    switch (finalByte) {
        case 'A':
            psionMoveVertical(-1, count);
            break;
        case 'B':
            psionMoveVertical(1, count);
            break;
        case 'C':
            psionMoveRight(count);
            break;
        case 'D':
            psionMoveLeft(count);
            break;
        case 'H':
        case 'f':
            psionCursorX = 0;
            psionCursorY = 0;
            scheduleTerminalRedraw();
            break;
        case 'J':
            if (psionCsiHasParam && psionCsiParam == 2) {
                psionEraseScreen();
            } else {
                psionEraseToEndOfScreen();
            }
            break;
        case 'K':
            psionEraseToEndOfLine();
            break;
        default:
            break;
    }
    resetPsionTermParser();
}

void putPsionChar(char c) {
    if (c == '\r') {
        psionNewline();
        return;
    }
    if (c == '\n') {
        return;
    }
    if (c == '\b' || c == 0x7f) {
        psionBackspace();
        return;
    }
    if (c < 0x20) {
        return;
    }
    psionScreen[psionCursorY][psionCursorX] = c;
    markTerminalRowDirty(true, psionCursorY);
    ++psionCursorX;
    if (psionCursorX >= kCols) {
        psionNewline();
    }
}

void putPsionByte(uint8_t value) {
    if (hexView) {
        const char *hex = "0123456789ABCDEF";
        putPsionChar(hex[value >> 4]);
        putPsionChar(hex[value & 0x0f]);
        putPsionChar(' ');
        return;
    }

    if (psionTermState == PsionTermState::Esc) {
        if (value == '[') {
            psionTermState = PsionTermState::Csi;
            psionCsiParam = 0;
            psionCsiHasParam = false;
            return;
        }
        resetPsionTermParser();
        return;
    }

    if (psionTermState == PsionTermState::Csi) {
        if (value >= '0' && value <= '9') {
            psionCsiParam = psionCsiParam * 10 + (value - '0');
            psionCsiHasParam = true;
            return;
        }
        if (value == ';' || value == '?') {
            return;
        }
        if (value >= 0x40 && value <= 0x7e) {
            handlePsionCsiFinal(static_cast<char>(value));
            return;
        }
        resetPsionTermParser();
        return;
    }

    if (value == 0x1b) {
        psionTermState = PsionTermState::Esc;
        return;
    }

    putPsionChar(static_cast<char>(value));
}

void putTerminalChar(char c) {
    if (c == '\r') {
        newline();
        lastWasCr = true;
        return;
    }

    if (c == '\n') {
        if (!lastWasCr) {
            newline();
        }
        lastWasCr = false;
        return;
    }

    lastWasCr = false;

    if (c == '\b' || c == 0x7f) {
        backspace();
        return;
    }

    if (c < 0x20) {
        return;
    }

    screen[cursorY][cursorX] = c;
    markTerminalRowDirty(false, cursorY);
    ++cursorX;
    if (cursorX >= kCols) {
        newline();
    }
}

void showNotice(const char *message) {
    clearBuffer();
    for (const char *p = message; *p; ++p) {
        putTerminalChar(*p);
    }
    redrawTerminal();
}

void showNoticeLine(const char *message) {
    for (const char *p = message; *p; ++p) {
        putTerminalChar(*p);
    }
    putTerminalChar('\r');
    refreshTerminalIfNeeded(true);
}

void putText(const char *text) {
    for (const char *p = text; *p; ++p) {
        putTerminalChar(*p);
    }
}

void putDebugText(const char *text) {
    if (!psionScreenView) {
        putText(text);
    }
}

void putDebugNewline() {
    if (!psionScreenView) {
        putTerminalChar('\r');
    }
}

void logLine(const char *line) {
    if (psionScreenView) {
        return;
    }

    appendParserLogLine(line);
    putDebugText(line);
    putDebugNewline();
}

void flushRepeatedRr() {
    if (psionScreenView) {
        rxRrSuppressCount = 0;
        txRrSuppressCount = 0;
        return;
    }

    char line[96];
    if (rxRrSuppressCount > 0) {
        snprintf(
            line,
            sizeof(line),
            "RX RR suppressed x%lu last ca=%02X nr=%u",
            static_cast<unsigned long>(rxRrSuppressCount),
            rxRrSuppressAddress,
            rxRrSuppressNr);
        logLine(line);
        rxRrSuppressCount = 0;
    }
    if (txRrSuppressCount > 0) {
        snprintf(
            line,
            sizeof(line),
            "TX RR suppressed x%lu last ca=%02X nr=%u",
            static_cast<unsigned long>(txRrSuppressCount),
            txRrSuppressAddress,
            txRrSuppressNr);
        logLine(line);
        txRrSuppressCount = 0;
    }
}

bool compressRepeatedRr(bool tx, uint8_t address, uint8_t nr) {
    address &= 0xfe;
    if (tx) {
        txRrSuppressAddress = address;
        txRrSuppressNr = nr;
        ++txRrSuppressCount;
    } else {
        rxRrSuppressAddress = address;
        rxRrSuppressNr = nr;
        ++rxRrSuppressCount;
    }
    return true;
}

void putHexByte(uint8_t value) {
    const char *hex = "0123456789ABCDEF";
    putTerminalChar(hex[value >> 4]);
    putTerminalChar(hex[value & 0x0f]);
    putTerminalChar(' ');
}

void putHexCompact(uint8_t value) {
    const char *hex = "0123456789ABCDEF";
    putTerminalChar(hex[value >> 4]);
    putTerminalChar(hex[value & 0x0f]);
}

size_t appendFormat(char *out, size_t outSize, size_t pos, const char *format, ...) {
    if (pos >= outSize) {
        return pos;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + pos, outSize - pos, format, args);
    va_end(args);

    if (written < 0) {
        return pos;
    }

    size_t used = static_cast<size_t>(written);
    if (used >= outSize - pos) {
        return outSize - 1;
    }
    return pos + used;
}

void putDisplayByte(uint8_t value) {
    if (hexView) {
        putHexByte(value);
    } else {
        putTerminalChar(static_cast<char>(value));
    }
}

void putLocalEchoByte(uint8_t value) {
    if (psionScreenView) {
        putPsionByte(value);
    } else {
        putDisplayByte(value);
    }
}

uint16_t updateFcs(uint16_t fcs, uint8_t value) {
    fcs ^= value;
    for (int bit = 0; bit < 8; ++bit) {
        if (fcs & 1) {
            fcs = (fcs >> 1) ^ 0x8408;
        } else {
            fcs >>= 1;
        }
    }
    return fcs;
}

bool frameFcsOk(const uint8_t *frame, size_t len) {
    if (len < 4) {
        return false;
    }

    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        fcs = updateFcs(fcs, frame[i]);
    }
    return fcs == 0xf0b8;
}

uint16_t finishFcs(const uint8_t *frame, size_t len) {
    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        fcs = updateFcs(fcs, frame[i]);
    }
    return ~fcs;
}

void writeIrdaEscaped(uint8_t value) {
    if (value == 0xc0 || value == 0xc1 || value == 0x7d) {
        IrSerial.write(0x7d);
        IrSerial.write(value ^ 0x20);
        return;
    }
    IrSerial.write(value);
}

void sendIrdaFrame(const uint8_t *payload, size_t len) {
    uint16_t fcs = finishFcs(payload, len);
    IrSerial.write(0xc0);
    IrSerial.write(0xc0);
    for (size_t i = 0; i < len; ++i) {
        writeIrdaEscaped(payload[i]);
    }
    writeIrdaEscaped(static_cast<uint8_t>(fcs & 0xff));
    writeIrdaEscaped(static_cast<uint8_t>(fcs >> 8));
    IrSerial.write(0xc1);
}

void sendXidResponse(const uint8_t remoteAddress[4], uint8_t flags, uint8_t slot) {
    uint8_t payload[48];
    size_t pos = 0;
    payload[pos++] = 0xfe;
    payload[pos++] = 0xbf;
    payload[pos++] = 0x01;

    for (uint8_t value : kCardputerIrdaAddress) {
        payload[pos++] = value;
    }
    for (size_t i = 0; i < 4; ++i) {
        payload[pos++] = remoteAddress[i];
    }

    payload[pos++] = flags & 0x03;
    payload[pos++] = slot;
    payload[pos++] = 0x00;
    payload[pos++] = kIrdaHintComputer | kIrdaHintExtension;
    payload[pos++] = kIrdaHintIrComm;
    payload[pos++] = kIrdaCharsetAscii;

    for (size_t i = 0; i < sizeof(kCardputerIrdaName) && pos < sizeof(payload); ++i) {
        payload[pos++] = static_cast<uint8_t>(kCardputerIrdaName[i]);
    }

    delay(kXidResponseDelayMs);
    sendIrdaFrame(payload, pos);
}

void sendUaResponseFromSnrm(const uint8_t *snrmFrame, size_t snrmLen) {
    if (snrmLen < 12) {
        return;
    }

    uint8_t payload[64];
    size_t pos = 0;
    uint8_t connectionAddress = snrmFrame[10] & 0xfe;
    payload[pos++] = connectionAddress;
    payload[pos++] = 0x73;

    for (size_t i = 6; i < 10 && pos < sizeof(payload); ++i) {
        payload[pos++] = snrmFrame[i];
    }
    for (size_t i = 2; i < 6 && pos < sizeof(payload); ++i) {
        payload[pos++] = snrmFrame[i];
    }
    for (uint8_t value : kIrdaQos) {
        if (pos >= sizeof(payload)) {
            break;
        }
        payload[pos++] = value;
    }

    delay(kUaResponseDelayMs);
    sendIrdaFrame(payload, pos);
}

void sendRrResponse(uint8_t address, uint8_t nr) {
    uint8_t payload[2];
    payload[0] = address & 0xfe;
    payload[1] = 0x11 | ((nr & 0x07) << 5);
    sendIrdaFrame(payload, sizeof(payload));
}

void sendIFrame(uint8_t address, uint8_t ns, uint8_t nr, const uint8_t *info, size_t infoLen) {
    uint8_t payload[66];
    size_t pos = 0;
    lastTxIInfoLen = min(infoLen, sizeof(lastTxIInfo));
    memcpy(lastTxIInfo, info, lastTxIInfoLen);
    payload[pos++] = address & 0xfe;
    payload[pos++] = 0x10 | ((nr & 0x07) << 5) | ((ns & 0x07) << 1);
    for (size_t i = 0; i < infoLen && pos < sizeof(payload); ++i) {
        payload[pos++] = info[i];
    }
    sendIrdaFrame(payload, pos);
}

bool isIFrame(uint8_t control) {
    return (control & 0x01) == 0;
}

void appendLastTxIInfoHex(char *line, size_t lineSize, size_t &pos) {
    for (size_t i = 0; i < lastTxIInfoLen && pos + 2 < lineSize; ++i) {
        pos += snprintf(line + pos, lineSize - pos, "%02X", lastTxIInfo[i]);
    }
}

void appendLastIrCommRxDataHex(char *line, size_t lineSize, size_t &pos) {
    for (size_t i = 0; i < lastIrCommRxDataLen && pos + 2 < lineSize; ++i) {
        pos += snprintf(line + pos, lineSize - pos, "%02X", lastIrCommRxData[i]);
    }
}

void clearIrCommTxQueue() {
    irCommTxQueueStart = 0;
    irCommTxQueueLen = 0;
}

void resetIrdaLinkState() {
    resetIrdaParser();
    irlapVr = 0;
    irlapVs = 0;
    irCommConnected = false;
    irCommTxCredit = 0;
    irCommPeerAddress = 0;
    irCommPeerNr = 0;
    irCommPeerSeen = false;
    irCommResponseWindowOpen = false;
    lastIrCommRxDataLen = 0;
    clearIrCommTxQueue();
    rxRrSuppressCount = 0;
    txRrSuppressCount = 0;
}

void setIrLinkMode(IrLinkMode mode) {
    if (irLinkMode == mode) {
        drawStatus();
        redrawTerminal();
        return;
    }

    irLinkMode = mode;
    resetIrdaLinkState();
    if (irdaModeEnabled()) {
        irdaParserView = true;
        hexView = false;
        psionScreenView = false;
        clearParserLog();
        clearPsionBuffer();
    } else {
        irdaParserView = false;
        psionScreenView = false;
        clearParserLog();
        clearBuffer();
    }
    drawStatus();
    redrawTerminal();
}

void showCharacterView() {
    if (irdaModeEnabled()) {
        psionScreenView = !psionScreenView;
        if (psionScreenView) {
            irdaParserView = false;
            hexView = false;
        }
    } else {
        psionScreenView = false;
        irdaParserView = false;
        hexView = false;
    }
    drawStatus();
    redrawTerminal();
}

bool enqueueIrCommByte(uint8_t value) {
    if (irCommTxQueueLen >= kIrCommTxQueueSize) {
        return false;
    }
    irCommTxQueue[(irCommTxQueueStart + irCommTxQueueLen) % kIrCommTxQueueSize] = value;
    ++irCommTxQueueLen;
    return true;
}

size_t popIrCommBytes(uint8_t *out, size_t maxLen) {
    size_t count = min(irCommTxQueueLen, maxLen);
    for (size_t i = 0; i < count; ++i) {
        out[i] = irCommTxQueue[(irCommTxQueueStart + i) % kIrCommTxQueueSize];
    }
    irCommTxQueueStart = (irCommTxQueueStart + count) % kIrCommTxQueueSize;
    irCommTxQueueLen -= count;
    return count;
}

void appendIrCommInitialControl(uint8_t *response, size_t &responseLen, size_t responseSize) {
    if (responseLen >= responseSize) {
        return;
    }
    response[responseLen++] = sizeof(kIrCommInitialControlParams);
    for (uint8_t value : kIrCommInitialControlParams) {
        if (responseLen >= responseSize) {
            break;
        }
        response[responseLen++] = value;
    }
}

void sendIrCommData(uint8_t address) {
    uint8_t response[64];
    size_t responseLen = 0;
    response[responseLen++] = irCommRemoteLsap;
    response[responseLen++] = irCommLocalLsap;
    response[responseLen++] = 0x04; // TinyTP additional credit for Psion.
    response[responseLen++] = 0x00; // IrCOMM control length: no control parameters.
    responseLen += popIrCommBytes(response + responseLen, sizeof(response) - responseLen);

    sendIFrame(address, irlapVs, irlapVr, response, responseLen);
    irlapVs = (irlapVs + 1) & 0x07;
}

void noteIrLapPeerState(uint8_t address, uint8_t nr) {
    irCommPeerAddress = address;
    irCommPeerNr = nr & 0x07;
    irCommPeerSeen = true;
}

bool trySendQueuedIrCommData() {
    if (!irCommConnected || irCommTxQueueLen == 0 || !irCommPeerSeen || !irCommResponseWindowOpen || irCommPeerNr != irlapVs) {
        return false;
    }
    sendIrCommData(irCommPeerAddress);
    lastIrCommTxPokeMs = millis();
    return true;
}

bool trySendQueuedIrCommDataInResponse() {
    irCommResponseWindowOpen = true;
    bool sent = trySendQueuedIrCommData();
    irCommResponseWindowOpen = false;
    return sent;
}

void pokeIrCommTx(uint8_t address) {
    if (!irCommConnected || irCommTxQueueLen == 0) {
        return;
    }
    uint32_t now = millis();
    if (now - lastIrCommTxPokeMs < kIrCommTxPokeIntervalMs) {
        return;
    }
    lastIrCommTxPokeMs = now;
    sendRrResponse(address, irlapVr);
}

void loadNetworkConfig() {
    NetPrefs.begin(kNetPrefsNamespace, true);
    configuredWifiSsid = NetPrefs.getString(kNetPrefsSsidKey, "");
    configuredWifiPassword = NetPrefs.getString(kNetPrefsPasswordKey, "");
    configuredTelnetHost = NetPrefs.getString(kNetPrefsTelnetHostKey, "");
    configuredTelnetPort = static_cast<uint16_t>(NetPrefs.getUShort(kNetPrefsTelnetPortKey, 23));
    configuredSshHost = NetPrefs.getString(kNetPrefsSshHostKey, "");
    configuredSshPort = static_cast<uint16_t>(NetPrefs.getUShort(kNetPrefsSshPortKey, 22));
    configuredSshUser = NetPrefs.getString(kNetPrefsSshUserKey, "");
    configuredSshPassword = NetPrefs.getString(kNetPrefsSshPasswordKey, "");
    configuredSshPrivateKey = NetPrefs.getString(kNetPrefsSshPrivateKeyKey, "");
    NetPrefs.end();
}

void saveNetworkConfig(const String &ssid, const String &password) {
    NetPrefs.begin(kNetPrefsNamespace, false);
    NetPrefs.putString(kNetPrefsSsidKey, ssid);
    NetPrefs.putString(kNetPrefsPasswordKey, password);
    NetPrefs.end();
    configuredWifiSsid = ssid;
    configuredWifiPassword = password;
}

void saveTelnetConfig(const String &host, uint16_t port) {
    NetPrefs.begin(kNetPrefsNamespace, false);
    NetPrefs.putString(kNetPrefsTelnetHostKey, host);
    NetPrefs.putUShort(kNetPrefsTelnetPortKey, port);
    NetPrefs.end();
    configuredTelnetHost = host;
    configuredTelnetPort = port;
}

bool hasTelnetConfig() {
    return !configuredTelnetHost.isEmpty() && configuredTelnetPort > 0;
}

void saveSshConfig(const String &host, uint16_t port, const String &user, const String &password) {
    NetPrefs.begin(kNetPrefsNamespace, false);
    NetPrefs.putString(kNetPrefsSshHostKey, host);
    NetPrefs.putUShort(kNetPrefsSshPortKey, port);
    NetPrefs.putString(kNetPrefsSshUserKey, user);
    NetPrefs.putString(kNetPrefsSshPasswordKey, password);
    NetPrefs.end();
    configuredSshHost = host;
    configuredSshPort = port;
    configuredSshUser = user;
    configuredSshPassword = password;
}

bool hasSshConfig() {
    return !configuredSshHost.isEmpty() && configuredSshPort > 0 && !configuredSshUser.isEmpty();
}

void saveSshPrivateKey(const String &privateKey) {
    NetPrefs.begin(kNetPrefsNamespace, false);
    NetPrefs.putString(kNetPrefsSshPrivateKeyKey, privateKey);
    NetPrefs.end();
    configuredSshPrivateKey = privateKey;
}

void clearSshPrivateKey() {
    NetPrefs.begin(kNetPrefsNamespace, false);
    NetPrefs.remove(kNetPrefsSshPrivateKeyKey);
    NetPrefs.end();
    configuredSshPrivateKey = "";
}

bool hasWifiConfig() {
    return !configuredWifiSsid.isEmpty();
}

void waitForKeyboardRelease() {
    do {
        M5Cardputer.update();
        delay(10);
    } while (M5Cardputer.Keyboard.isPressed());
}

void drawLinkModeMenu(int selected) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.fillRect(0, 0, kScreenW, 18, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setCursor(4, 5);
    display.print("IRTerminal");

    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(4, 24);
    display.print("Choose infrared mode");

    struct MenuItem {
        const char *tag;
        const char *title;
        const char *detail;
    };
    const MenuItem items[] = {
        {"SIR", "HP 200LX raw serial", "Datacomm + Telnet/SSH"},
        {"IDA", "Psion IrDA/IrCOMM", "Comms discovery + bridge"},
    };

    for (int i = 0; i < 2; ++i) {
        int y = 44 + i * 32;
        bool active = i == selected;
        uint16_t bg = active ? TFT_CYAN : TFT_BLACK;
        uint16_t fg = active ? TFT_BLACK : TFT_WHITE;
        uint16_t sub = active ? TFT_BLACK : TFT_GREEN;
        display.fillRect(4, y, kScreenW - 8, 26, bg);
        display.drawRect(4, y, kScreenW - 8, 26, active ? TFT_WHITE : TFT_DARKGREY);
        display.setTextColor(fg, bg);
        display.setCursor(8, y + 3);
        display.printf("%s  %s", items[i].tag, items[i].title);
        display.setTextColor(sub, bg);
        display.setCursor(8, y + 14);
        display.print(items[i].detail);
    }

    display.fillRect(0, kScreenH - 14, kScreenW, 14, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setCursor(4, kScreenH - 11);
    display.print("Up/Down select  Enter ok");
}

void chooseIrLinkModeMenu() {
    int selected = irdaModeEnabled() ? 1 : 0;
    waitForKeyboardRelease();
    while (true) {
        drawLinkModeMenu(selected);
        while (!M5Cardputer.Keyboard.isChange()) {
            M5Cardputer.update();
            delay(10);
        }

        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        for (char c : keys.word) {
            if (c == kKeyArrowUp || c == '1') {
                selected = 0;
            }
            if (c == kKeyArrowDown || c == '2') {
                selected = 1;
            }
        }
        if (keys.enter) {
            waitForKeyboardRelease();
            setIrLinkMode(selected == 0 ? IrLinkMode::RawSir : IrLinkMode::IrdaIrComm);
            return;
        }
    }
}

String inputTextModal(const char *title, bool passwordMode) {
    String input;
    waitForKeyboardRelease();
    auto &display = M5Cardputer.Display;

    while (true) {
        M5Cardputer.update();
        display.fillScreen(TFT_BLACK);
        drawStatus();
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.setCursor(2, kTop + 4);
        display.print(title);
        display.setCursor(2, kTop + 18);
        display.print(passwordMode ? "Password:" : "Input:");
        display.setCursor(2, kTop + 34);
        if (passwordMode) {
            for (size_t i = 0; i < input.length(); ++i) {
                display.print('*');
            }
        } else {
            display.print(input);
        }
        display.setCursor(2, kScreenH - kBottom - kLineH);
        display.print("Enter=save Del=backspace Fn+Q=cancel");

        while (!M5Cardputer.Keyboard.isChange()) {
            M5Cardputer.update();
            delay(10);
        }

        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        if (keys.fn && keys.word.size() == 1 && (keys.word[0] == 'q' || keys.word[0] == 'Q')) {
            waitForKeyboardRelease();
            return "\x1B";
        }
        if (keys.del && input.length() > 0) {
            input.remove(input.length() - 1);
        }
        for (char c : keys.word) {
            if (c >= 0x20 && c <= 0x7e) {
                input += c;
            }
        }
        if (keys.enter) {
            waitForKeyboardRelease();
            return input;
        }
    }
}

void drawWifiScanList(String ssids[], int rssis[], int count, int selected, int topIndex) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    drawStatus();
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(2, kTop + 2);
    display.print("WiFi scan: Up/Down Enter");

    int rows = min(8, count - topIndex);
    for (int row = 0; row < rows; ++row) {
        int index = topIndex + row;
        int y = kTop + 16 + row * kLineH;
        uint16_t fg = index == selected ? TFT_BLACK : TFT_GREEN;
        uint16_t bg = index == selected ? TFT_GREEN : TFT_BLACK;
        display.fillRect(0, y, kScreenW, kLineH, bg);
        display.setTextColor(fg, bg);
        display.setCursor(2, y);
        String ssid = ssids[index];
        if (ssid.length() > 25) {
            ssid = ssid.substring(0, 25);
        }
        display.printf("%2d %4d %s", index + 1, rssis[index], ssid.c_str());
    }

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(2, kScreenH - kBottom - kLineH);
    display.print("Fn+Q cancel");
}

bool chooseWifiNetwork(String &selectedSsid) {
    showNotice("Scanning WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);
    int found = WiFi.scanNetworks();
    if (found <= 0) {
        showNotice("No WiFi networks found.");
        delay(800);
        return false;
    }

    String ssids[kWifiScanMaxItems];
    int rssis[kWifiScanMaxItems];
    int count = min(found, kWifiScanMaxItems);
    for (int i = 0; i < count; ++i) {
        ssids[i] = WiFi.SSID(i);
        rssis[i] = WiFi.RSSI(i);
    }
    WiFi.scanDelete();

    int selected = 0;
    int topIndex = 0;
    waitForKeyboardRelease();
    while (true) {
        drawWifiScanList(ssids, rssis, count, selected, topIndex);
        while (!M5Cardputer.Keyboard.isChange()) {
            M5Cardputer.update();
            delay(10);
        }
        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        if (keys.fn && keys.word.size() == 1 && (keys.word[0] == 'q' || keys.word[0] == 'Q')) {
            waitForKeyboardRelease();
            return false;
        }
        for (char c : keys.word) {
            if (c == kKeyArrowUp && selected > 0) {
                --selected;
            }
            if (c == kKeyArrowDown && selected + 1 < count) {
                ++selected;
            }
        }
        if (selected < topIndex) {
            topIndex = selected;
        }
        if (selected >= topIndex + 8) {
            topIndex = selected - 7;
        }
        if (keys.enter) {
            selectedSsid = ssids[selected];
            waitForKeyboardRelease();
            return true;
        }
    }
}

void setupWifiFromKeyboard() {
    String ssid;
    if (!chooseWifiNetwork(ssid)) {
        showNotice("WiFi setup canceled.");
        delay(600);
        redrawTerminal();
        return;
    }

    String password = inputTextModal(ssid.c_str(), true);
    if (password == "\x1B") {
        showNotice("WiFi setup canceled.");
        delay(600);
        redrawTerminal();
        return;
    }

    saveNetworkConfig(ssid, password);
    showNotice("WiFi config saved.");
    delay(600);
    redrawTerminal();
}

bool setupTelnetTargetFromKeyboard() {
    String hostTitle = configuredTelnetHost.isEmpty() ? "Telnet host/IP" : ("Host: " + configuredTelnetHost);
    String host = inputTextModal(hostTitle.c_str(), false);
    if (host == "\x1B" || host.isEmpty()) {
        showNotice("Telnet setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }

    String portTitle = "Port: " + String(configuredTelnetPort);
    String portText = inputTextModal(portTitle.c_str(), false);
    if (portText == "\x1B") {
        showNotice("Telnet setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }

    uint16_t port = static_cast<uint16_t>(portText.toInt());
    if (port == 0) {
        port = 23;
    }

    saveTelnetConfig(host, port);
    showNotice("Telnet target saved.");
    delay(600);
    redrawTerminal();
    return true;
}

void resetTelnetTargetFromKeyboard() {
    if (telnetBridgeEnabled || telnetBridgeConnected) {
        stopTelnetBridge();
    }
    setupTelnetTargetFromKeyboard();
    redrawTerminal();
}

bool setupSshTargetFromKeyboard() {
    String hostTitle = configuredSshHost.isEmpty() ? "SSH host/IP" : ("SSH host: " + configuredSshHost);
    String host = inputTextModal(hostTitle.c_str(), false);
    if (host == "\x1B" || host.isEmpty()) {
        showNotice("SSH setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }

    String portTitle = "SSH port: " + String(configuredSshPort);
    String portText = inputTextModal(portTitle.c_str(), false);
    if (portText == "\x1B") {
        showNotice("SSH setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }
    uint16_t port = static_cast<uint16_t>(portText.toInt());
    if (port == 0) {
        port = 22;
    }

    String userTitle = configuredSshUser.isEmpty() ? "SSH user" : ("SSH user: " + configuredSshUser);
    String user = inputTextModal(userTitle.c_str(), false);
    if (user == "\x1B" || user.isEmpty()) {
        showNotice("SSH setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }

    String password = inputTextModal("SSH password", true);
    if (password == "\x1B") {
        showNotice("SSH setup canceled.");
        delay(600);
        redrawTerminal();
        return false;
    }

    saveSshConfig(host, port, user, password);
    showNotice("SSH target saved.");
    delay(600);
    redrawTerminal();
    return true;
}

void resetSshTargetFromKeyboard() {
    if (sshBridgeEnabled || sshBridgeConnected) {
        stopSshBridge();
    }
    setupSshTargetFromKeyboard();
    redrawTerminal();
}

void showNetworkInfo() {
    clearBuffer();
    showNoticeLine("Network info");
    showNoticeLine(("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "connected" : "not connected")).c_str());
    showNoticeLine(("SSID: " + (configuredWifiSsid.isEmpty() ? String("(not set)") : configuredWifiSsid)).c_str());
    if (WiFi.status() == WL_CONNECTED) {
        showNoticeLine(("IP: " + WiFi.localIP().toString()).c_str());
        showNoticeLine(("RSSI: " + String(WiFi.RSSI()) + " dBm").c_str());
    }
    String target = configuredTelnetHost.isEmpty() ? String("(not set)") : configuredTelnetHost + ":" + String(configuredTelnetPort);
    showNoticeLine(("Telnet: " + target).c_str());
    String sshTarget = configuredSshHost.isEmpty() ? String("(not set)") : configuredSshUser + "@" + configuredSshHost + ":" + String(configuredSshPort);
    showNoticeLine(("SSH: " + sshTarget).c_str());
    showNoticeLine(("SSH key: " + String(configuredSshPrivateKey.isEmpty() ? "not set" : "set")).c_str());
    showNoticeLine(("T bridge: " + String(telnetBridgeConnected ? "connected" : (telnetBridgeEnabled ? "enabled" : "off"))).c_str());
    showNoticeLine(("S bridge: " + String(sshBridgeConnected ? "connected" : (sshBridgeEnabled ? "enabled" : "off"))).c_str());
    delay(2500);
    redrawTerminal();
}

void stopTelnetBridge(const char *message) {
    if (TelnetClient.connected()) {
        TelnetClient.stop();
    }
    telnetBridgeEnabled = false;
    telnetBridgeConnected = false;
    telnetIacPending = false;
    telnetCommandPending = false;
    telnetPendingCommand = 0;
    bridgeInputLastWasCr = false;
    drawStatus();
    if (message != nullptr) {
        showNotice(message);
    }
}

void setSshStatus(const char *message) {
    strncpy(sshStatusMessage, message, sizeof(sshStatusMessage) - 1);
    sshStatusMessage[sizeof(sshStatusMessage) - 1] = '\0';
    sshStatusPending = true;
}

bool ensureSshBuffers() {
    if (SshOutbound == nullptr) {
        SshOutbound = xStreamBufferCreate(kSshStreamBufferSize, 1);
    }
    if (SshInbound == nullptr) {
        SshInbound = xStreamBufferCreate(kSshStreamBufferSize, 1);
    }
    return SshOutbound != nullptr && SshInbound != nullptr;
}

void resetSshBuffers() {
    if (SshOutbound != nullptr) {
        xStreamBufferReset(SshOutbound);
    }
    if (SshInbound != nullptr) {
        xStreamBufferReset(SshInbound);
    }
}

String normalizeSshPrivateKeyForImport(const String &storedKey) {
    if (storedKey.indexOf("BEGIN OPENSSH PRIVATE KEY") >= 0) {
        return storedKey;
    }

    String out;
    out.reserve(storedKey.length());
    for (size_t i = 0; i < storedKey.length(); ++i) {
        char c = storedKey[i];
        if (c == '-') {
            while (i < storedKey.length() && storedKey[i] != '\n' && storedKey[i] != '\r') {
                ++i;
            }
            continue;
        }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        out += c;
    }
    return out;
}

void sshWorkerTask(void *pvParameters) {
    (void)pvParameters;
    char buffer[kSshIoChunkSize];
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    ssh_key privateKey = nullptr;
    int strictHostKeyChecking = 0;
    String host = configuredSshHost;
    String user = configuredSshUser;
    String password = configuredSshPassword;
    String privateKeyText = normalizeSshPrivateKeyForImport(configuredSshPrivateKey);
    uint16_t port = configuredSshPort;

    sshBridgeConnected = false;
    setSshStatus("SSH connecting...");

    session = ssh_new();
    if (session == nullptr) {
        setSshStatus("SSH session creation failed.");
        goto SSH_EXIT;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strictHostKeyChecking);
    ssh_set_blocking(session, 1);

    if (ssh_connect(session) != SSH_OK) {
        setSshStatus("SSH connect failed.");
        goto SSH_EXIT;
    }

    if (!privateKeyText.isEmpty()) {
        int keyImport = ssh_pki_import_privkey_base64(
            privateKeyText.c_str(),
            password.isEmpty() ? nullptr : password.c_str(),
            nullptr,
            nullptr,
            &privateKey);
        if (keyImport == SSH_OK && privateKey != nullptr &&
            ssh_userauth_publickey(session, nullptr, privateKey) == SSH_AUTH_SUCCESS) {
            setSshStatus("SSH key auth ok.");
        } else if (password.isEmpty() || ssh_userauth_password(session, nullptr, password.c_str()) != SSH_AUTH_SUCCESS) {
            setSshStatus(keyImport == SSH_OK ? "SSH key auth failed." : "SSH key import failed.");
            goto SSH_EXIT;
        }
    } else {
        if (ssh_userauth_password(session, nullptr, password.c_str()) != SSH_AUTH_SUCCESS) {
            setSshStatus("SSH auth failed.");
            goto SSH_EXIT;
        }
    }

    channel = ssh_channel_new(session);
    if (channel == nullptr || ssh_channel_open_session(channel) != SSH_OK) {
        setSshStatus("SSH channel open failed.");
        goto SSH_EXIT;
    }

    if (ssh_channel_request_pty_size(channel, "vt100", 80, 24) != SSH_OK) {
        setSshStatus("SSH PTY request failed.");
        goto SSH_EXIT;
    }

    if (ssh_channel_request_shell(channel) != SSH_OK) {
        setSshStatus("SSH shell request failed.");
        goto SSH_EXIT;
    }
    ssh_set_blocking(session, 0);

    sshBridgeConnected = true;
    setSshStatus("SSH bridge connected.");

    while (!sshStopRequested) {
        size_t outboundLen = xStreamBufferReceive(SshOutbound, buffer, sizeof(buffer), 0);
        if (outboundLen > 0) {
            int written = ssh_channel_write(channel, buffer, outboundLen);
            if (written == SSH_AGAIN) {
                xStreamBufferSend(SshOutbound, buffer, outboundLen, 0);
            } else if (written == SSH_ERROR) {
                setSshStatus("SSH write failed.");
                break;
            } else if (written >= 0 && static_cast<size_t>(written) < outboundLen) {
                xStreamBufferSend(SshOutbound, buffer + written, outboundLen - static_cast<size_t>(written), 0);
            }
        }

        int available = ssh_channel_poll(channel, 0);
        if (available == SSH_AGAIN) {
            available = 0;
        } else if (available == SSH_ERROR) {
            setSshStatus("SSH poll failed.");
            break;
        }
        if (available > 0) {
            int readSize = min<int>(available, sizeof(buffer));
            int nbytes = ssh_channel_read(channel, buffer, readSize, 0);
            if (nbytes > 0) {
                xStreamBufferSend(SshInbound, buffer, nbytes, 0);
            } else if (nbytes == SSH_AGAIN) {
                // No data ready in non-blocking mode.
            } else if (nbytes == SSH_ERROR) {
                setSshStatus("SSH read failed.");
                break;
            }
        }

        if (ssh_channel_is_closed(channel) || ssh_channel_is_eof(channel)) {
            setSshStatus("SSH session closed.");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

SSH_EXIT:
    sshBridgeConnected = false;
    sshBridgeEnabled = false;
    if (privateKey != nullptr) {
        ssh_key_free(privateKey);
    }
    if (channel != nullptr) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (session != nullptr) {
        if (ssh_is_connected(session)) {
            ssh_disconnect(session);
        }
        ssh_free(session);
    }
    SshTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

void stopSshBridge(const char *message) {
    sshStopRequested = true;
    uint32_t startMs = millis();
    while (SshTaskHandle != nullptr && millis() - startMs < 2000) {
        delay(20);
    }
    if (SshTaskHandle != nullptr) {
        vTaskDelete(SshTaskHandle);
        SshTaskHandle = nullptr;
    }
    sshBridgeEnabled = false;
    sshBridgeConnected = false;
    resetSshBuffers();
    drawStatus();
    if (message != nullptr) {
        showNotice(message);
    }
}

bool ensureWifiConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        clearBuffer();
        showNoticeLine("WiFi already connected.");
        showNoticeLine(("SSID: " + WiFi.SSID()).c_str());
        showNoticeLine(("IP: " + WiFi.localIP().toString()).c_str());
        delay(800);
        return true;
    }
    if (!hasWifiConfig()) {
        setupWifiFromKeyboard();
        if (!hasWifiConfig()) {
            return false;
        }
    }

    WiFi.mode(WIFI_STA);
    clearBuffer();
    showNoticeLine("Connecting WiFi...");
    showNoticeLine(("SSID: " + configuredWifiSsid).c_str());
    WiFi.begin(configuredWifiSsid.c_str(), configuredWifiPassword.c_str());
    uint32_t startMs = millis();
    uint32_t lastDotMs = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
        delay(100);
        M5Cardputer.update();
        if (millis() - lastDotMs >= 1000) {
            lastDotMs = millis();
            putTerminalChar('.');
            refreshTerminalIfNeeded(true);
        }
    }
    putTerminalChar('\r');
    if (WiFi.status() == WL_CONNECTED) {
        showNoticeLine("WiFi connected.");
        showNoticeLine(("IP: " + WiFi.localIP().toString()).c_str());
        delay(1000);
        return true;
    }

    showNoticeLine("WiFi connect failed.");
    showNoticeLine(("Status: " + String(static_cast<int>(WiFi.status()))).c_str());
    delay(1500);
    return false;
}

void startTelnetBridge() {
    if (!hasTelnetConfig()) {
        if (!setupTelnetTargetFromKeyboard()) {
            drawStatus();
            return;
        }
    }

    if (sshBridgeEnabled || sshBridgeConnected) {
        stopSshBridge();
    }

    if (!ensureWifiConnected()) {
        stopTelnetBridge("WiFi connect failed.");
        return;
    }

    if (TelnetClient.connected()) {
        TelnetClient.stop();
    }
    telnetBridgeEnabled = true;
    clearBuffer();
    showNoticeLine("Connecting Telnet...");
    showNoticeLine(("Target: " + configuredTelnetHost + ":" + String(configuredTelnetPort)).c_str());
    showNoticeLine(("WiFi IP: " + WiFi.localIP().toString()).c_str());
    telnetBridgeConnected = TelnetClient.connect(configuredTelnetHost.c_str(), configuredTelnetPort);
    telnetIacPending = false;
    telnetCommandPending = false;
    telnetPendingCommand = 0;
    drawStatus();

    if (telnetBridgeConnected) {
        clearBuffer();
        showNoticeLine("Telnet bridge connected.");
        showNoticeLine(("Target: " + configuredTelnetHost + ":" + String(configuredTelnetPort)).c_str());
        showNoticeLine(("WiFi IP: " + WiFi.localIP().toString()).c_str());
        delay(1000);
    } else {
        clearBuffer();
        showNoticeLine("Telnet connect failed.");
        showNoticeLine(("Target: " + configuredTelnetHost + ":" + String(configuredTelnetPort)).c_str());
        showNoticeLine(("WiFi IP: " + WiFi.localIP().toString()).c_str());
        showNoticeLine("Check host, port, firewall.");
        delay(1800);
        stopTelnetBridge();
    }
}

void toggleTelnetBridge() {
    if (telnetBridgeEnabled || telnetBridgeConnected) {
        stopTelnetBridge("Telnet bridge stopped.");
    } else {
        startTelnetBridge();
    }
}

void startSshBridge() {
    if (!hasSshConfig()) {
        if (!setupSshTargetFromKeyboard()) {
            drawStatus();
            return;
        }
    }

    if (telnetBridgeEnabled || telnetBridgeConnected) {
        stopTelnetBridge();
    }

    if (!ensureWifiConnected()) {
        stopSshBridge("WiFi connect failed.");
        return;
    }

    if (!ensureSshBuffers()) {
        showNotice("SSH buffer allocation failed.");
        return;
    }

    resetSshBuffers();
    sshStopRequested = false;
    sshStatusPending = false;
    bridgeInputLastWasCr = false;
    sshBridgeEnabled = true;
    sshBridgeConnected = false;
    clearBuffer();
    showNoticeLine("Starting SSH...");
    showNoticeLine(("Target: " + configuredSshUser + "@" + configuredSshHost + ":" + String(configuredSshPort)).c_str());
    showNoticeLine(("WiFi IP: " + WiFi.localIP().toString()).c_str());
    drawStatus();

    BaseType_t result = xTaskCreate(
        sshWorkerTask,
        "SSH bridge",
        kSshTaskStackSize,
        nullptr,
        1,
        &SshTaskHandle);
    if (result != pdPASS || SshTaskHandle == nullptr) {
        sshBridgeEnabled = false;
        showNotice("SSH task creation failed.");
    }
}

void toggleSshBridge() {
    if (sshBridgeEnabled || sshBridgeConnected) {
        stopSshBridge("SSH bridge stopped.");
    } else {
        startSshBridge();
    }
}

void writePsionDataToTelnet(const uint8_t *data, size_t len) {
    if (!telnetBridgeEnabled || !telnetBridgeConnected || data == nullptr || len == 0) {
        return;
    }

    if (!TelnetClient.connected()) {
        stopTelnetBridge("Telnet disconnected.");
        return;
    }

    TelnetClient.write(data, len);
}

void writePsionDataToSsh(const uint8_t *data, size_t len) {
    if (!sshBridgeEnabled || !sshBridgeConnected || SshOutbound == nullptr || data == nullptr || len == 0) {
        return;
    }
    xStreamBufferSend(SshOutbound, data, len, 0);
}

bool writeInputByteToActiveBridge(uint8_t value) {
    uint8_t out = value;
    if (value == '\r') {
        out = '\r';
        bridgeInputLastWasCr = true;
    } else if (value == '\n') {
        if (bridgeInputLastWasCr) {
            bridgeInputLastWasCr = false;
            return true;
        }
        out = '\r';
        bridgeInputLastWasCr = false;
    } else {
        bridgeInputLastWasCr = false;
    }

    if (sshBridgeEnabled && sshBridgeConnected) {
        writePsionDataToSsh(&out, 1);
        return true;
    }
    if (telnetBridgeEnabled && telnetBridgeConnected) {
        writePsionDataToTelnet(&out, 1);
        return true;
    }
    return false;
}

bool writeInputToActiveBridge(const uint8_t *data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }
    if (!(sshBridgeEnabled && sshBridgeConnected) && !(telnetBridgeEnabled && telnetBridgeConnected)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        writeInputByteToActiveBridge(data[i]);
    }
    return true;
}

void displayPsionInputPayload(const uint8_t *data, size_t dataLen) {
    for (size_t i = 0; i < dataLen; ++i) {
        appendRxLog(data[i]);
        writeUsbByteNonBlocking(data[i]);
        putPsionByte(data[i]);
    }
    if (dataLen > 0 && psionScreenView) {
        psionPayloadDrawPending = true;
    }
}

void deliverPsionInputPayload(const uint8_t *data, size_t dataLen) {
    if (writeInputToActiveBridge(data, dataLen)) {
        return;
    }

    displayPsionInputPayload(data, dataLen);
}

void enqueueTelnetByteForPsion(uint8_t value) {
    appendRxLog(value);
    writeUsbByteNonBlocking(value);
    if (!irdaModeEnabled()) {
        IrSerial.write(value);
        putDisplayByte(value);
        return;
    }

    if (!enqueueIrCommByte(value)) {
        const char *marker = "\r\n[IR TX queue full]\r\n";
        for (const char *p = marker; *p; ++p) {
            putPsionByte(*p);
        }
        psionPayloadDrawPending = true;
        return;
    }
    putPsionByte(value);
    if (psionScreenView) {
        psionPayloadDrawPending = true;
    }
}

void respondToTelnetCommand(uint8_t command, uint8_t option) {
    uint8_t response = 0;
    if (command == kTelnetDo || command == kTelnetDont) {
        response = kTelnetWont;
    } else if (command == kTelnetWill || command == kTelnetWont) {
        response = kTelnetDont;
    } else {
        return;
    }

    TelnetClient.write(kTelnetIac);
    TelnetClient.write(response);
    TelnetClient.write(option);
}

void handleTelnetInputByte(uint8_t value) {
    if (telnetCommandPending) {
        respondToTelnetCommand(telnetPendingCommand, value);
        telnetCommandPending = false;
        telnetIacPending = false;
        return;
    }

    if (telnetIacPending) {
        if (value == kTelnetIac) {
            enqueueTelnetByteForPsion(value);
        } else if (value == kTelnetDo || value == kTelnetDont || value == kTelnetWill || value == kTelnetWont) {
            telnetPendingCommand = value;
            telnetCommandPending = true;
            return;
        }
        telnetIacPending = false;
        return;
    }

    if (value == kTelnetIac) {
        telnetIacPending = true;
        return;
    }

    enqueueTelnetByteForPsion(value);
}

void handleTelnetBridge() {
    if (!telnetBridgeEnabled) {
        return;
    }

    if (!TelnetClient.connected()) {
        stopTelnetBridge("Telnet disconnected.");
        return;
    }

    telnetBridgeConnected = true;
    size_t processed = 0;
    while (TelnetClient.available() && processed < kTelnetInputChunkBytes) {
        handleTelnetInputByte(static_cast<uint8_t>(TelnetClient.read()));
        ++processed;
    }
    if (processed > 0) {
        refreshTerminalIfNeeded();
    }
}

void handleSshBridge() {
    if (sshStatusPending) {
        sshStatusPending = false;
        clearBuffer();
        showNoticeLine(sshStatusMessage);
        if (sshBridgeConnected) {
            showNoticeLine(("Target: " + configuredSshUser + "@" + configuredSshHost + ":" + String(configuredSshPort)).c_str());
            showNoticeLine(("WiFi IP: " + WiFi.localIP().toString()).c_str());
        }
        sshStatusShowing = true;
        sshStatusShownAtMs = millis();
        drawStatus();
    }

    if (sshStatusShowing && millis() - sshStatusShownAtMs >= kSshStatusDisplayMs) {
        sshStatusShowing = false;
        redrawTerminal();
    }

    if (SshInbound == nullptr) {
        return;
    }

    uint8_t buffer[kSshIoChunkSize];
    size_t received = xStreamBufferReceive(SshInbound, buffer, sizeof(buffer), 0);
    for (size_t i = 0; i < received; ++i) {
        enqueueTelnetByteForPsion(buffer[i]);
    }
    if (received > 0) {
        refreshTerminalIfNeeded();
    }
}

bool sendIrLmpConnectConfirm(uint8_t address, const uint8_t *info, size_t infoLen) {
    if (infoLen < 4 || !(info[0] & 0x80) || info[2] != 0x01) {
        return false;
    }

    uint8_t dsap = info[0] & 0x7f;
    uint8_t response[64];
    size_t responseLen = 0;
    response[0] = info[1] | 0x80;
    response[1] = info[0] & 0x7f;
    response[2] = 0x81;
    response[3] = 0x00;

    if (dsap == kIrCommLsap) {
        responseLen = 4;
        response[responseLen++] = kIrCommAdditionalCredit;
        appendIrCommInitialControl(response, responseLen, sizeof(response));
        irCommRemoteLsap = info[1] & 0x7f;
        irCommLocalLsap = dsap;
        irCommConnected = true;
        irCommTxCredit = infoLen > 4 ? (info[4] & 0x7f) : 0;
        clearIrCommTxQueue();
    } else {
        responseLen = 4;
    }

    sendIFrame(address, irlapVs, irlapVr, response, responseLen);
    irlapVs = (irlapVs + 1) & 0x07;
    return true;
}

bool handleIrCommData(const uint8_t *info, size_t infoLen) {
    if (!irCommConnected || infoLen < 4) {
        return false;
    }
    if ((info[0] & 0x7f) != irCommLocalLsap || (info[1] & 0x7f) != irCommRemoteLsap) {
        return false;
    }

    irCommTxCredit = min<uint8_t>(static_cast<uint8_t>(irCommTxCredit + (info[2] & 0x7f)), 0x7f);
    size_t controlLen = info[3];
    if (4 + controlLen > infoLen) {
        return true;
    }

    const uint8_t *data = info + 4 + controlLen;
    size_t dataLen = infoLen - 4 - controlLen;
    lastIrCommRxDataLen = min(dataLen, sizeof(lastIrCommRxData));
    memcpy(lastIrCommRxData, data, lastIrCommRxDataLen);
    deliverPsionInputPayload(data, dataLen);
    return true;
}

bool parseIrCommPayload(const uint8_t *info, size_t infoLen, const uint8_t *&data, size_t &dataLen) {
    data = nullptr;
    dataLen = 0;
    if (!irCommConnected || infoLen < 4) {
        return false;
    }
    if ((info[0] & 0x7f) != irCommLocalLsap || (info[1] & 0x7f) != irCommRemoteLsap) {
        return false;
    }

    irCommTxCredit = min<uint8_t>(static_cast<uint8_t>(irCommTxCredit + (info[2] & 0x7f)), 0x7f);
    size_t controlLen = info[3];
    if (4 + controlLen > infoLen) {
        return true;
    }

    data = info + 4 + controlLen;
    dataLen = infoLen - 4 - controlLen;
    lastIrCommRxDataLen = min(dataLen, sizeof(lastIrCommRxData));
    memcpy(lastIrCommRxData, data, lastIrCommRxDataLen);
    return true;
}

void displayIrCommPayload(const uint8_t *data, size_t dataLen) {
    deliverPsionInputPayload(data, dataLen);
}

bool asciiEquals(const uint8_t *data, size_t len, const char *text) {
    size_t textLen = strlen(text);
    return len == textLen && memcmp(data, text, textLen) == 0;
}

bool sendIasResult(uint8_t address, const uint8_t *info, size_t infoLen) {
    if (infoLen < 6 || info[0] != 0x00 || info[2] != 0x84) {
        return false;
    }

    size_t classLen = info[3];
    if (4 + classLen >= infoLen) {
        return false;
    }

    const uint8_t *className = info + 4;
    size_t attrLenPos = 4 + classLen;
    size_t attrLen = info[attrLenPos];
    if (attrLenPos + 1 + attrLen > infoLen) {
        return false;
    }

    const uint8_t *attrName = info + attrLenPos + 1;
    if (!asciiEquals(className, classLen, "IrDA:IrCOMM")) {
        return false;
    }

    uint8_t response[24];
    size_t pos = 0;
    response[pos++] = info[1] & 0x7f;
    response[pos++] = info[0] & 0x7f;
    response[pos++] = 0x84;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x01;
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    if (asciiEquals(attrName, attrLen, "Parameters")) {
        response[pos++] = 0x02;
        response[pos++] = 0x00;
        response[pos++] = 0x03;
        response[pos++] = 0x00;
        response[pos++] = 0x01;
        response[pos++] = 0x02;
    } else if (
        asciiEquals(attrName, attrLen, "IrDA:TinyTP:LsapSel") ||
        asciiEquals(attrName, attrLen, "IrDA:IrLMP:LsapSel") ||
        asciiEquals(attrName, attrLen, "IrDA:IrLMP:LsapSec")) {
        response[pos++] = 0x01;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = kIrCommLsap;
    } else {
        return false;
    }

    sendIFrame(address, irlapVs, irlapVr, response, pos);
    irlapVs = (irlapVs + 1) & 0x07;
    return true;
}

bool extractDeviceName(const uint8_t *frame, size_t len, char *name, size_t nameSize) {
    size_t bestStart = len;
    size_t bestLen = 0;

    for (size_t i = 0; i < len;) {
        if (frame[i] < 0x20 || frame[i] > 0x7e) {
            ++i;
            continue;
        }

        size_t start = i;
        while (i < len && frame[i] >= 0x20 && frame[i] <= 0x7e) {
            ++i;
        }

        size_t runLen = i - start;
        if (runLen > bestLen) {
            bestStart = start;
            bestLen = runLen;
        }
    }

    if (bestLen < 3 || nameSize == 0) {
        return false;
    }

    size_t copyLen = bestLen;
    if (copyLen >= nameSize) {
        copyLen = nameSize - 1;
    }
    memcpy(name, frame + bestStart, copyLen);
    name[copyLen] = '\0';
    return true;
}

bool acceptIncomingIFrame(uint8_t address, uint8_t ns, bool logEnabled) {
    if (ns == irlapVr) {
        irlapVr = (ns + 1) & 0x07;
        return true;
    }

    sendRrResponse(address, irlapVr);
    if (logEnabled) {
        char txLine[96];
        snprintf(txLine, sizeof(txLine), "TX RR duplicate/drop I ns=%u expected=%u", ns, irlapVr);
        appendParserLogLine(txLine);
        putDebugText("TX RR dup I");
        putDebugNewline();
    }
    return false;
}

bool handleFastPsionDataFrame() {
    if (!psionScreenView || !irCommConnected || irdaFrameLen < 4 || !(irdaFrame[0] & 0x01) || !isIFrame(irdaFrame[1])) {
        return false;
    }

    uint8_t ns = (irdaFrame[1] >> 1) & 0x07;
    uint8_t peerNr = (irdaFrame[1] >> 5) & 0x07;
    noteIrLapPeerState(irdaFrame[0], peerNr);
    if (!acceptIncomingIFrame(irdaFrame[0], ns, false)) {
        return true;
    }

    const uint8_t *info = irdaFrame + 2;
    size_t infoLen = irdaFrameLen - 4;
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    if (!parseIrCommPayload(info, infoLen, data, dataLen)) {
        return false;
    }

    if (!trySendQueuedIrCommDataInResponse()) {
        sendRrResponse(irdaFrame[0], irlapVr);
    }
    displayIrCommPayload(data, dataLen);
    return true;
}

void printIrdaFrameSummary() {
    ++irdaFrameCount;
    bool logEnabled = !psionScreenView;

    if (irdaFrameOverflow) {
        if (logEnabled) {
            char line[160];
            size_t pos = 0;
            pos = appendFormat(line, sizeof(line), pos, "#%lu len=%u overflow", static_cast<unsigned long>(irdaFrameCount), static_cast<unsigned>(irdaFrameLen));
            flushRepeatedRr();
            logLine(line);
        }
        return;
    }

    bool crcOk = frameFcsOk(irdaFrame, irdaFrameLen);
    if (crcOk) {
        ++irdaCrcOkCount;
    } else {
        ++irdaCrcBadCount;
    }

    if (crcOk && handleFastPsionDataFrame()) {
        return;
    }

    if (logEnabled) {
        char line[160];
        size_t pos = 0;
        pos = appendFormat(line, sizeof(line), pos, "#%lu len=%u ", static_cast<unsigned long>(irdaFrameCount), static_cast<unsigned>(irdaFrameLen));
        if (irdaFrameLen >= 2) {
            pos = appendFormat(line, sizeof(line), pos, "A=%02X C=%02X ", irdaFrame[0], irdaFrame[1]);
        }

        bool isRr = irdaFrameLen >= 2 && (irdaFrame[1] & 0x1f) == 0x11;
        if (!isRr) {
            flushRepeatedRr();
        }
        bool suppressFrameLog = false;

        bool isXid = irdaFrameLen >= 3 && (irdaFrame[1] == 0x3f || irdaFrame[1] == 0xbf) && irdaFrame[2] == 0x01;
        if (isXid) {
            pos = appendFormat(line, sizeof(line), pos, "XID");
            if (irdaFrameLen >= 14) {
                pos = appendFormat(
                    line,
                    sizeof(line),
                    pos,
                    " src=%02X%02X%02X%02X dst=%02X%02X%02X%02X flags=%02X slot=%02X ver=%02X",
                    irdaFrame[3],
                    irdaFrame[4],
                    irdaFrame[5],
                    irdaFrame[6],
                    irdaFrame[7],
                    irdaFrame[8],
                    irdaFrame[9],
                    irdaFrame[10],
                    irdaFrame[11],
                    irdaFrame[12],
                    irdaFrame[13]);
            }
            char name[48];
            if (extractDeviceName(irdaFrame, irdaFrameLen > 2 ? irdaFrameLen - 2 : irdaFrameLen, name, sizeof(name))) {
                pos = appendFormat(line, sizeof(line), pos, " name=%s", name);
            }
        } else if (irdaFrameLen >= 2 && irdaFrame[1] == 0x93) {
            pos = appendFormat(line, sizeof(line), pos, "SNRM");
            if (irdaFrameLen >= 12) {
                pos = appendFormat(
                    line,
                    sizeof(line),
                    pos,
                    " peer=%02X%02X%02X%02X local=%02X%02X%02X%02X ca=%02X",
                    irdaFrame[2],
                    irdaFrame[3],
                    irdaFrame[4],
                    irdaFrame[5],
                    irdaFrame[6],
                    irdaFrame[7],
                    irdaFrame[8],
                    irdaFrame[9],
                    irdaFrame[10]);
            }
        } else if (irdaFrameLen >= 2 && irdaFrame[1] == 0x73) {
            pos = appendFormat(line, sizeof(line), pos, "UA");
        } else if (irdaFrameLen >= 2 && irdaFrame[1] == 0x53) {
            pos = appendFormat(line, sizeof(line), pos, "DISC");
        } else if (isRr) {
            uint8_t rrNr = irdaFrame[1] >> 5;
            pos = appendFormat(line, sizeof(line), pos, "RR nr=%u", rrNr);
            if (compressRepeatedRr(false, irdaFrame[0], rrNr)) {
                suppressFrameLog = true;
            }
        } else if (irdaFrameLen >= 2 && isIFrame(irdaFrame[1])) {
            flushRepeatedRr();
            uint8_t ns = (irdaFrame[1] >> 1) & 0x07;
            uint8_t nr = (irdaFrame[1] >> 5) & 0x07;
            pos = appendFormat(line, sizeof(line), pos, "I ns=%u nr=%u infoLen=%u", ns, nr, static_cast<unsigned>(irdaFrameLen >= 4 ? irdaFrameLen - 4 : 0));
            if (irdaFrameLen > 4) {
                pos = appendFormat(line, sizeof(line), pos, " info=");
                for (size_t i = 2; i + 2 < irdaFrameLen && pos + 2 < sizeof(line); ++i) {
                    pos = appendFormat(line, sizeof(line), pos, "%02X", irdaFrame[i]);
                }
            }
        } else {
            flushRepeatedRr();
            pos = appendFormat(line, sizeof(line), pos, "IrLAP");
        }

        pos = appendFormat(line, sizeof(line), pos, crcOk ? " crc=ok" : " crc=?");
        line[sizeof(line) - 1] = '\0';
        if (!suppressFrameLog) {
            logLine(line);
        }
    }

    if (crcOk && irdaFrameLen >= 16 && irdaFrame[0] == 0xff && irdaFrame[1] == 0x3f && irdaFrame[2] == 0x01) {
        const uint8_t *xidSource = irdaFrame + 3;
        uint8_t xidFlags = irdaFrame[11];
        uint8_t xidSlot = irdaFrame[12];
        if (xidSlot == 0x00) {
            sendXidResponse(xidSource, xidFlags, xidSlot);
            if (logEnabled) {
                char txLine[96];
                snprintf(
                    txLine,
                    sizeof(txLine),
                    "TX XID response flags=%02X slot=%02X delay=%lums hints=%02X%02X name=Cardputer IR fw=%s",
                    xidFlags & 0x03,
                    xidSlot,
                    static_cast<unsigned long>(kXidResponseDelayMs),
                    kIrdaHintComputer | kIrdaHintExtension,
                    kIrdaHintIrComm,
                    kFirmwareTag);
                appendParserLogLine(txLine);
                putDebugText("TX XID response");
                putDebugNewline();
            }
        }
    }

    if (crcOk && irdaFrameLen >= 12 && irdaFrame[0] == 0xff && irdaFrame[1] == 0x93) {
        irlapVr = 0;
        irlapVs = 0;
        irCommConnected = false;
        irCommTxCredit = 0;
        irCommPeerAddress = 0;
        irCommPeerNr = 0;
        irCommPeerSeen = false;
        irCommResponseWindowOpen = false;
        lastIrCommRxDataLen = 0;
        clearIrCommTxQueue();
        sendUaResponseFromSnrm(irdaFrame, irdaFrameLen);
        if (logEnabled) {
            char txLine[64];
            snprintf(txLine, sizeof(txLine), "TX UA response ca=%02X qos=trim-bof", irdaFrame[10] & 0xfe);
            appendParserLogLine(txLine);
            putDebugText("TX UA response");
            putDebugNewline();
        }
    }

    if (crcOk && irdaFrameLen == 4 && (irdaFrame[0] & 0x01) && ((irdaFrame[1] & 0x1f) == 0x11)) {
        uint8_t peerNr = irdaFrame[1] >> 5;
        noteIrLapPeerState(irdaFrame[0], peerNr);
        if (trySendQueuedIrCommDataInResponse()) {
            if (logEnabled) {
                char txLine[160];
                size_t txPos = snprintf(txLine, sizeof(txLine), "TX I IrCOMM data ca=%02X ns=%u nr=%u infoLen=%u q=%u credit=%u info=", irdaFrame[0] & 0xfe, (irlapVs + 7) & 0x07, irlapVr, static_cast<unsigned>(lastTxIInfoLen), static_cast<unsigned>(irCommTxQueueLen), irCommTxCredit);
                appendLastTxIInfoHex(txLine, sizeof(txLine), txPos);
                appendParserLogLine(txLine);
                putDebugText("TX I IrCOMM data");
                putDebugNewline();
            }
        } else {
            sendRrResponse(irdaFrame[0], irlapVr);
            pokeIrCommTx(irdaFrame[0]);
            if (logEnabled) {
                char txLine[160];
                if (!compressRepeatedRr(true, irdaFrame[0], irlapVr)) {
                    snprintf(txLine, sizeof(txLine), "TX RR response ca=%02X nr=%u", irdaFrame[0] & 0xfe, irlapVr);
                    appendParserLogLine(txLine);
                    putDebugText("TX RR response");
                    putDebugNewline();
                }
            }
        }
    }

    if (crcOk && irdaFrameLen >= 4 && (irdaFrame[0] & 0x01) && isIFrame(irdaFrame[1])) {
        uint8_t ns = (irdaFrame[1] >> 1) & 0x07;
        uint8_t peerNr = (irdaFrame[1] >> 5) & 0x07;
        noteIrLapPeerState(irdaFrame[0], peerNr);
        if (!acceptIncomingIFrame(irdaFrame[0], ns, logEnabled)) {
            return;
        }

        const uint8_t *info = irdaFrame + 2;
        size_t infoLen = irdaFrameLen - 4;
        bool lmpHandled = sendIrLmpConnectConfirm(irdaFrame[0], info, infoLen);
        bool irCommHandled = false;
        bool iasHandled = false;
        if (!lmpHandled) {
            irCommHandled = handleIrCommData(info, infoLen);
        }
        if (!lmpHandled && !irCommHandled) {
            iasHandled = sendIasResult(irdaFrame[0], info, infoLen);
        }
        if (lmpHandled) {
            if (logEnabled) {
                char txLine[160];
                size_t txPos = snprintf(txLine, sizeof(txLine), "TX I LMP CONNECT_CNF ca=%02X ns=%u nr=%u dsap=%02X infoLen=%u info=", irdaFrame[0] & 0xfe, (irlapVs + 7) & 0x07, irlapVr, info[0] & 0x7f, static_cast<unsigned>(lastTxIInfoLen));
                appendLastTxIInfoHex(txLine, sizeof(txLine), txPos);
                appendParserLogLine(txLine);
                putDebugText("TX I LMP CNF");
                putDebugNewline();
            }
        } else if (iasHandled) {
            if (logEnabled) {
                char txLine[160];
                size_t txPos = snprintf(txLine, sizeof(txLine), "TX I IAS result ca=%02X ns=%u nr=%u infoLen=%u info=", irdaFrame[0] & 0xfe, (irlapVs + 7) & 0x07, irlapVr, static_cast<unsigned>(lastTxIInfoLen));
                appendLastTxIInfoHex(txLine, sizeof(txLine), txPos);
                appendParserLogLine(txLine);
                putDebugText("TX I IAS result");
                putDebugNewline();
            }
        } else if (irCommHandled) {
            bool sentQueuedData = trySendQueuedIrCommDataInResponse();
            if (!sentQueuedData) {
                sendRrResponse(irdaFrame[0], irlapVr);
                pokeIrCommTx(irdaFrame[0]);
            }
            if (logEnabled) {
                char txLine[160];
                size_t txPos = snprintf(txLine, sizeof(txLine), "RX IrCOMM data ca=%02X rxLen=%u rx=", irdaFrame[0] & 0xfe, static_cast<unsigned>(lastIrCommRxDataLen));
                appendLastIrCommRxDataHex(txLine, sizeof(txLine), txPos);
                snprintf(
                    txLine + txPos,
                    sizeof(txLine) - txPos,
                    sentQueuedData ? " queued=%u TX I nr=%u" : " queued=%u TX RR nr=%u",
                    static_cast<unsigned>(irCommTxQueueLen),
                    irlapVr);
                appendParserLogLine(txLine);
                putDebugText("RX IrCOMM data");
                putDebugNewline();
            }
        } else {
            sendRrResponse(irdaFrame[0], irlapVr);
            if (logEnabled) {
                char txLine[160];
                snprintf(txLine, sizeof(txLine), "TX RR response ca=%02X nr=%u ack=I ns=%u", irdaFrame[0] & 0xfe, irlapVr, ns);
                appendParserLogLine(txLine);
                putDebugText("TX RR ack I");
                putDebugNewline();
            }
        }
    }
}

void resetIrdaParser() {
    irdaFrameLen = 0;
    irdaInFrame = false;
    irdaEscaped = false;
    irdaFrameOverflow = false;
}

void feedIrdaParser(uint8_t value) {
    if (value == 0xc0) {
        irdaInFrame = true;
        irdaEscaped = false;
        irdaFrameOverflow = false;
        irdaFrameLen = 0;
        return;
    }

    if (!irdaInFrame) {
        return;
    }

    if (value == 0xc1) {
        if (irdaFrameLen > 0 || irdaFrameOverflow) {
            printIrdaFrameSummary();
        }
        resetIrdaParser();
        return;
    }

    if (value == 0x7d) {
        irdaEscaped = true;
        return;
    }

    if (irdaEscaped) {
        value ^= 0x20;
        irdaEscaped = false;
    }

    if (irdaFrameLen < kIrdaFrameMax) {
        irdaFrame[irdaFrameLen++] = value;
    } else {
        irdaFrameOverflow = true;
    }
}

void sendToIr(char c) {
    uint8_t value = static_cast<uint8_t>(c);
    if (writeInputByteToActiveBridge(value)) {
        if (localEcho) {
            putLocalEchoByte(value);
        }
        return;
    }
    if (irdaModeEnabled() && irCommConnected) {
        enqueueIrCommByte(static_cast<uint8_t>(c));
    } else {
        IrSerial.write(c);
    }
    if (localEcho) {
        putLocalEchoByte(static_cast<uint8_t>(c));
    }
}

void sendEnter() {
    uint8_t value = '\r';
    if (writeInputByteToActiveBridge(value)) {
        if (localEcho) {
            putLocalEchoByte(value);
        }
        return;
    }
    if (irdaModeEnabled() && irCommConnected) {
        enqueueIrCommByte('\r');
        enqueueIrCommByte('\n');
    } else {
        IrSerial.print("\r\n");
    }
    if (localEcho) {
        putLocalEchoByte('\r');
    }
}

void restartIrSerial() {
    IrSerial.end();
    IrSerial.setRxBufferSize(kIrRxBufferSize);
    IrSerial.begin(kBaudRates[baudIndex], SERIAL_8N1, kIrRxPin, kIrTxPin);
}

void dumpParserLogToSerial() {
    if (parserLogLen == 0) {
        const char *empty = "No parser log. Press Fn+P and receive IrDA frames first.\n";
        Serial.print(empty);
        return;
    }

    Serial.println();
    Serial.println("----- BEGIN IRDA PARSER LOG -----");
    for (size_t offset = 0; offset < parserLogLen; offset += kSerialDumpChunk) {
        size_t chunk = min(kSerialDumpChunk, parserLogLen - offset);
        Serial.write(reinterpret_cast<const uint8_t *>(parserLog + offset), chunk);
        delay(0);
    }
    Serial.println("----- END IRDA PARSER LOG -----");
    Serial.flush();
}

void showHelp() {
    clearBuffer();
    const char *lines[] = {
        "Grove: G1=RX, G2=TX, GND common",
        "PocketPostPet ref test: IrDA/IrCOMM 115200",
        "R2E default: 115200 for PocketPostPet.",
        "Fn+B/Fn+H help Psion debugging.",
        "Keyboard -> IR, IR -> screen + USB.",
        "",
        "Fn+E: toggle local echo",
        "Fn+M: choose SIR raw / IrDA IrCOMM",
        "Fn+V: show 200LX/Psion text screen",
        "Fn+H: toggle text/hex view",
        "Fn+P: toggle IrDA parser view",
        "Fn+W: scan WiFi and save password",
        "Fn+T: toggle Telnet bridge",
        "Fn+R: reset Telnet host/port",
        "Fn+Y: toggle SSH bridge",
        "Fn+U: reset SSH host/user/pass",
        "Fn+I: show network info",
        "Fn+B: cycle baud rate",
        "Fn+S: dump parser log to USB serial",
        "Fn+C: clear screen",
        "BtnA: restart, hold key at boot for Launcher",
    };

    for (const char *line : lines) {
        for (const char *p = line; *p; ++p) {
            putTerminalChar(*p);
        }
        putTerminalChar('\r');
    }
}

void handleKeyboard() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
        return;
    }

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

    if (keys.fn && keys.word.size() == 1) {
        char c = keys.word[0];
        if (c == 'e' || c == 'E') {
            localEcho = !localEcho;
            drawStatus();
            return;
        }
        if (c == 'm' || c == 'M') {
            chooseIrLinkModeMenu();
            return;
        }
        if (c == 'v' || c == 'V') {
            showCharacterView();
            return;
        }
        if (c == 'h' || c == 'H') {
            hexView = !hexView;
            if (hexView) {
                irdaParserView = false;
            }
            drawStatus();
            return;
        }
        if (c == 'p' || c == 'P') {
            if (!irdaModeEnabled()) {
                setIrLinkMode(IrLinkMode::IrdaIrComm);
                return;
            }
            irdaParserView = !irdaParserView;
            if (irdaParserView) {
                hexView = false;
                resetIrdaParser();
                clearParserLog();
                clearBuffer();
            }
            drawStatus();
            redrawTerminal();
            return;
        }
        if (c == 't' || c == 'T') {
            toggleTelnetBridge();
            redrawTerminal();
            return;
        }
        if (c == 'w' || c == 'W') {
            setupWifiFromKeyboard();
            redrawTerminal();
            return;
        }
        if (c == 'r' || c == 'R') {
            resetTelnetTargetFromKeyboard();
            redrawTerminal();
            return;
        }
        if (c == 'y' || c == 'Y') {
            toggleSshBridge();
            redrawTerminal();
            return;
        }
        if (c == 'u' || c == 'U') {
            resetSshTargetFromKeyboard();
            redrawTerminal();
            return;
        }
        if (c == 'i' || c == 'I') {
            showNetworkInfo();
            redrawTerminal();
            return;
        }
        if (c == 'b' || c == 'B') {
            baudIndex = (baudIndex + 1) % (sizeof(kBaudRates) / sizeof(kBaudRates[0]));
            restartIrSerial();
            resetIrdaParser();
            drawStatus();
            return;
        }
        if (c == 's' || c == 'S') {
            showNotice("Dumping parser log to USB serial...");
            dumpParserLogToSerial();
            redrawTerminal();
            return;
        }
        if (c == 'c' || c == 'C') {
            if (psionScreenView) {
                clearPsionBuffer();
            } else {
                clearBuffer();
                clearParserLog();
                resetIrdaParser();
            }
            clearRxLog();
            redrawTerminal();
            return;
        }
    }

    for (char c : keys.word) {
        sendToIr(c);
    }

    if (keys.del) {
        if ((sshBridgeEnabled && sshBridgeConnected) || (telnetBridgeEnabled && telnetBridgeConnected)) {
            sendToIr(static_cast<char>(0x7f));
        } else {
            sendToIr('\b');
        }
    }

    if (keys.enter) {
        sendEnter();
    }
}

bool handleIrInput() {
    bool changed = false;
    size_t processed = 0;
    while (IrSerial.available() && processed < kIrInputChunkBytes) {
        uint8_t c = static_cast<uint8_t>(IrSerial.read());
        ++processed;
        appendRxLog(c);
        writeUsbByteNonBlocking(c);
        if (!irdaModeEnabled()) {
            writeInputByteToActiveBridge(c);
        }
        if (irdaModeEnabled()) {
            feedIrdaParser(c);
        } else {
            putDisplayByte(c);
        }
        changed = true;
    }
    if (psionPayloadDrawPending) {
        psionPayloadDrawPending = false;
        refreshTerminalIfNeeded(true);
    } else if (changed) {
        refreshTerminalIfNeeded();
    }
    return IrSerial.available() > 0;
}

void forwardUsbByteToIr(uint8_t c) {
    if (writeInputByteToActiveBridge(c)) {
        return;
    }
    if (irdaModeEnabled() && irCommConnected) {
        enqueueIrCommByte(c);
    } else {
        IrSerial.write(c);
    }
}

void printUsbCommandHelp() {
    Serial.println();
    Serial.println("IRTerminal USB commands:");
    Serial.println(":SSHKEY BEGIN   paste private key until :SSHKEY END");
    Serial.println(":SSHKEY END     finish private key import");
    Serial.println(":SSHKEY STATUS  show stored key status");
    Serial.println(":SSHKEY CLEAR   delete stored private key");
    Serial.println(":HELP           show this help");
}

void processUsbCommandLine(const char *line) {
    String command = line;
    command.trim();

    if (usbSshKeyCapture) {
        if (command == ":SSHKEY END") {
            saveSshPrivateKey(usbSshKeyBuffer);
            Serial.printf("SSH private key saved (%u bytes).\n", static_cast<unsigned>(configuredSshPrivateKey.length()));
            usbSshKeyBuffer = "";
            usbSshKeyCapture = false;
            return;
        }

        if (usbSshKeyBuffer.length() + command.length() + 1 > kSshPrivateKeyMax) {
            Serial.println("SSH private key too large; import canceled.");
            usbSshKeyBuffer = "";
            usbSshKeyCapture = false;
            return;
        }
        usbSshKeyBuffer += command;
        usbSshKeyBuffer += '\n';
        return;
    }

    if (command == ":SSHKEY BEGIN") {
        usbSshKeyBuffer = "";
        usbSshKeyBuffer.reserve(kSshPrivateKeyMax);
        usbSshKeyCapture = true;
        Serial.println("Paste SSH private key, then send :SSHKEY END");
        return;
    }
    if (command == ":SSHKEY CLEAR") {
        clearSshPrivateKey();
        Serial.println("SSH private key cleared.");
        return;
    }
    if (command == ":SSHKEY STATUS") {
        Serial.printf("SSH private key: %s", configuredSshPrivateKey.isEmpty() ? "not set\n" : "set\n");
        if (!configuredSshPrivateKey.isEmpty()) {
            Serial.printf("Stored bytes: %u\n", static_cast<unsigned>(configuredSshPrivateKey.length()));
        }
        return;
    }
    if (command == ":HELP") {
        printUsbCommandHelp();
        return;
    }

    Serial.print("Unknown USB command: ");
    Serial.println(command);
    Serial.println("Send :HELP for commands.");
}

void handleUsbBridge() {
    while (Serial.available()) {
        uint8_t c = static_cast<uint8_t>(Serial.read());

        if (usbCommandLineActive) {
            if (c == '\n' || c == '\r') {
                if (usbCommandLineLen > 0) {
                    usbCommandLine[usbCommandLineLen] = '\0';
                    processUsbCommandLine(usbCommandLine);
                }
                usbCommandLineLen = 0;
                usbCommandLineActive = false;
                continue;
            }
            if (usbCommandLineLen + 1 < sizeof(usbCommandLine)) {
                usbCommandLine[usbCommandLineLen++] = static_cast<char>(c);
            } else {
                Serial.println("USB command line too long.");
                usbCommandLineLen = 0;
                usbCommandLineActive = false;
            }
            continue;
        }

        if (usbSshKeyCapture || c == ':') {
            usbCommandLineActive = true;
            usbCommandLineLen = 0;
            usbCommandLine[usbCommandLineLen++] = static_cast<char>(c);
            continue;
        }

        forwardUsbByteToIr(c);
    }
}
} // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    USB.begin();
    Serial.begin(115200);
    loadNetworkConfig();
    restartIrSerial();
    Serial.println();
    Serial.println("[CCCP R2E] InstantComet reference-derived 115200 proof");
    Serial.printf("[CCCP R2E] UART1 normal 8N1 baud=%lu RX=G%d TX=G%d\n",
                  static_cast<unsigned long>(kBaudRates[baudIndex]), kIrRxPin, kIrTxPin);
    Serial.println("[CCCP R2E] wiring: G1(GPIO1)=RX<-IR TXD, G2(GPIO2)=TX->IR RXD");
    Serial.println("[CCCP R2E] IrLAP QoS: 115200 only; IrCOMM data rate: 115200");
    Serial.println("[CCCP R2E] IMPORTANT: ordinary HardwareSerial only");

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextFont(1);
    M5Cardputer.Display.fillScreen(TFT_BLACK);

    clearBuffer();
    clearPsionBuffer();
    chooseIrLinkModeMenu();
    drawStatus();
    showHelp();
    redrawTerminal();
}

void loop() {
    M5Cardputer.update();

    if (M5Cardputer.BtnA.wasPressed()) {
        ESP.restart();
    }

    handleKeyboard();
    bool irInputBacklog = handleIrInput();
    handleUsbBridge();
    handleTelnetBridge();
    handleSshBridge();
    refreshTerminalIfNeeded();

    delay(irInputBacklog ? 0 : 1);
}
