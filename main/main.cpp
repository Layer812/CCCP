/*
 * CCCP - Cardputer Communication Connector for PocketPostPet
 * Copyright (c) 2026 Layer812
 * SPDX-License-Identifier: MIT
 */

#include <M5Cardputer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <cstdio>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_err.h>

// Cardputer ADV HY2.0-4P / PORT.CUSTOM
//   GND / 5V / G2 / G1
// This project uses:
//   G1 = Cardputer TX -> MY018 RXD
//   G2 = Cardputer RX <- MY018 TXD
// MY018 is left at its factory/default 115200 bps setting.
// IMPORTANT: MY018 already performs IrDA SIR <-> TTL conversion, so do NOT
// enable ESP32 UART_MODE_IRDA here. This is an ordinary TTL UART.

static constexpr int IR_TX_PIN = 1;
static constexpr int IR_RX_PIN = 2;
static constexpr uint32_t IR_BAUD = 115200;

static constexpr uint32_t UI_REFRESH_MS = 250;
static constexpr size_t IR_LAST_LEN = 24;

HardwareSerial IrSerial(1);
Preferences prefs;

enum class ScreenMode {
  HOME,
  WIFI_LIST,
  WIFI_PASSWORD,
  MENU,
  CONFIRM_FORGET,
  CONFIRM_FACTORY
};

struct WifiEntry {
  String ssid;
  int32_t rssi = 0;
  bool open = false;
};

ScreenMode screenMode = ScreenMode::HOME;
std::vector<WifiEntry> wifiEntries;
int wifiSel = 0;
String pendingSsid;
String passwordInput;

uint64_t irRxBytes = 0;
uint64_t irTxBytes = 0;
uint8_t irLast[IR_LAST_LEN] = {};
size_t irLastCount = 0;
size_t irLastHead = 0;
uint32_t lastIrRxMs = 0;
uint32_t lastUiMs = 0;
uint32_t lastHeartbeatMs = 0;

String savedSsid;
bool haveSavedWifi = false;
bool wifiConnectIssued = false;
uint32_t wifiConnectStartMs = 0;

static void drawHeader(const char* title) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(BLACK);
  d.setTextColor(WHITE, BLACK);
  d.setTextSize(1);
  d.setCursor(4, 4);
  d.printf("%s\n", title);
  d.drawFastHLine(0, 17, d.width(), DARKGREY);
}

static String ipOrDash() {
  if (WiFi.status() != WL_CONNECTED) return "-";
  return WiFi.localIP().toString();
}

static void drawHome() {
  drawHeader("IrDA Wi-Fi Gateway R1S");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 23);

  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  d.printf("WiFi : %s\n", wifiOk ? "CONNECTED" : (wifiConnectIssued ? "CONNECTING" : "OFFLINE"));
  d.printf("SSID : %s\n", wifiOk ? WiFi.SSID().c_str() : (haveSavedWifi ? savedSsid.c_str() : "<not set>"));
  d.printf("IP   : %s\n", ipOrDash().c_str());
  d.printf("IrDA : TTL %lu 8N1\n", (unsigned long)IR_BAUD);
  d.printf("RX   : %llu bytes\n", (unsigned long long)irRxBytes);
  d.printf("TX   : %llu bytes\n", (unsigned long long)irTxBytes);

  if (lastIrRxMs == 0) {
    d.printf("Last : --\n");
  } else {
    d.printf("Last : %lu ms ago\n", (unsigned long)(millis() - lastIrRxMs));
  }

  d.printf("[M] menu  [W] WiFi setup\n");

  d.setCursor(4, 113);
  d.print("IR: ");
  size_t n = irLastCount < IR_LAST_LEN ? irLastCount : IR_LAST_LEN;
  size_t start = (irLastCount < IR_LAST_LEN) ? 0 : irLastHead;
  for (size_t i = 0; i < n && i < 12; ++i) {
    uint8_t b = irLast[(start + i) % IR_LAST_LEN];
    d.printf("%02X ", b);
  }
}

static void drawWifiList() {
  drawHeader("Wi-Fi Setup");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 22);

  if (wifiEntries.empty()) {
    d.println("No networks found.");
    d.println("[R] rescan   [TAB] back");
    return;
  }

  const int visible = 6;
  int first = wifiSel - visible / 2;
  if (first < 0) first = 0;
  if (first + visible > (int)wifiEntries.size()) first = std::max(0, (int)wifiEntries.size() - visible);

  for (int i = first; i < (int)wifiEntries.size() && i < first + visible; ++i) {
    const auto& e = wifiEntries[i];
    d.printf("%c %-18.18s %4ld %s\n",
             i == wifiSel ? '>' : ' ', e.ssid.c_str(), (long)e.rssi, e.open ? "O" : "*");
  }
  d.println(";/. = up/down   ENTER select");
  d.println("[R] rescan  [TAB] back");
}

static void drawPassword() {
  drawHeader("Wi-Fi Password");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 24);
  d.printf("SSID: %s\n\n", pendingSsid.c_str());
  d.print("Password: ");
  for (size_t i = 0; i < passwordInput.length(); ++i) d.print('*');
  d.println();
  d.println();
  d.println("ENTER = save & connect");
  d.println("DEL   = erase");
  d.println("TAB   = cancel");
}

static void drawMenu() {
  drawHeader("Menu");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 24);
  d.println("[W] Wi-Fi setup");
  d.println("[R] Reconnect Wi-Fi");
  d.println("[F] Forget saved Wi-Fi");
  d.println("[X] Factory reset");
  d.println("[TAB] Home");
  d.println();
  d.printf("IrDA: %lu bps, G1 TX / G2 RX\n", (unsigned long)IR_BAUD);
}

static void drawConfirmForget() {
  drawHeader("Forget Wi-Fi?");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 28);
  d.printf("Saved SSID:\n%s\n\n", haveSavedWifi ? savedSsid.c_str() : "<none>");
  d.println("[Y] Yes   [N]/[TAB] No");
}

static void drawConfirmFactory() {
  drawHeader("Factory Reset?");
  auto& d = M5Cardputer.Display;
  d.setCursor(4, 28);
  d.println("Clear all gateway settings");
  d.println("stored in NVS.");
  d.println();
  d.println("[Y] Yes   [N]/[TAB] No");
}

static void redraw() {
  switch (screenMode) {
    case ScreenMode::HOME: drawHome(); break;
    case ScreenMode::WIFI_LIST: drawWifiList(); break;
    case ScreenMode::WIFI_PASSWORD: drawPassword(); break;
    case ScreenMode::MENU: drawMenu(); break;
    case ScreenMode::CONFIRM_FORGET: drawConfirmForget(); break;
    case ScreenMode::CONFIRM_FACTORY: drawConfirmFactory(); break;
  }
}

static void loadWifiPrefs() {
  savedSsid = prefs.getString("ssid", "");
  haveSavedWifi = savedSsid.length() > 0;
}

static void saveWifiPrefs(const String& ssid, const String& pass) {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  savedSsid = ssid;
  haveSavedWifi = true;
}

static void connectWifi(const String& ssid, const String& pass) {
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnectIssued = true;
  wifiConnectStartMs = millis();
}

static void connectSavedWifi() {
  loadWifiPrefs();
  if (!haveSavedWifi) {
    wifiConnectIssued = false;
    return;
  }
  String pass = prefs.getString("pass", "");
  connectWifi(savedSsid, pass);
}

static void scanWifi() {
  screenMode = ScreenMode::WIFI_LIST;
  drawHeader("Wi-Fi Setup");
  M5Cardputer.Display.setCursor(4, 28);
  M5Cardputer.Display.println("Scanning 2.4 GHz networks...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  int n = WiFi.scanNetworks(false, true);
  wifiEntries.clear();

  for (int i = 0; i < n; ++i) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;

    // Collapse duplicate SSIDs, keeping the strongest entry.
    bool merged = false;
    for (auto& e : wifiEntries) {
      if (e.ssid == s) {
        if (WiFi.RSSI(i) > e.rssi) {
          e.rssi = WiFi.RSSI(i);
          e.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        merged = true;
        break;
      }
    }
    if (!merged) {
      WifiEntry e;
      e.ssid = s;
      e.rssi = WiFi.RSSI(i);
      e.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      wifiEntries.push_back(e);
    }
  }
  WiFi.scanDelete();
  wifiSel = 0;
  redraw();
}

static void forgetWifi() {
  prefs.remove("ssid");
  prefs.remove("pass");
  savedSsid = "";
  haveSavedWifi = false;
  wifiConnectIssued = false;
  WiFi.disconnect(true, false);
}

static void factoryReset() {
  WiFi.disconnect(true, false);
  prefs.clear();
  delay(100);
  ESP.restart();
}

static void selectWifi() {
  if (wifiEntries.empty()) return;
  const auto& e = wifiEntries[wifiSel];
  pendingSsid = e.ssid;
  passwordInput = "";

  if (e.open) {
    saveWifiPrefs(pendingSsid, "");
    connectWifi(pendingSsid, "");
    screenMode = ScreenMode::HOME;
  } else {
    screenMode = ScreenMode::WIFI_PASSWORD;
  }
  redraw();
}

static void handlePrintable(char c) {
  if (screenMode == ScreenMode::WIFI_PASSWORD) {
    if ((uint8_t)c >= 32 && (uint8_t)c <= 126 && passwordInput.length() < 63) {
      passwordInput += c;
      redraw();
    }
    return;
  }

  c = (char)tolower((unsigned char)c);

  if (screenMode == ScreenMode::HOME) {
    if (c == 'm') { screenMode = ScreenMode::MENU; redraw(); }
    else if (c == 'w') scanWifi();
    return;
  }

  if (screenMode == ScreenMode::WIFI_LIST) {
    if (c == 'r') scanWifi();
    return;
  }

  if (screenMode == ScreenMode::MENU) {
    if (c == 'w') scanWifi();
    else if (c == 'r') { connectSavedWifi(); screenMode = ScreenMode::HOME; redraw(); }
    else if (c == 'f') { screenMode = ScreenMode::CONFIRM_FORGET; redraw(); }
    else if (c == 'x') { screenMode = ScreenMode::CONFIRM_FACTORY; redraw(); }
    return;
  }

  if (screenMode == ScreenMode::CONFIRM_FORGET) {
    if (c == 'y') { forgetWifi(); screenMode = ScreenMode::HOME; redraw(); }
    else if (c == 'n') { screenMode = ScreenMode::MENU; redraw(); }
    return;
  }

  if (screenMode == ScreenMode::CONFIRM_FACTORY) {
    if (c == 'y') factoryReset();
    else if (c == 'n') { screenMode = ScreenMode::MENU; redraw(); }
    return;
  }
}

static void handleKeyboard() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;

  // M5Cardputer 1.1.1 KeysState (the vgmM5 baseline) exposes:
  // tab/fn/shift/ctrl/opt/alt/del/enter/space + word/hid_keys.
  // It does NOT expose up/down/backspace/esc fields.  vgmM5 itself uses
  // ';' and '.' as cursor navigation, so CCCP follows that known-good API.
  auto& ks = M5Cardputer.Keyboard.keysState();

  // TAB is the non-printable Back/Cancel key in CCCP.  This keeps every
  // printable character, including 'b', available while typing Wi-Fi passwords.
  if (ks.tab) {
    if (screenMode == ScreenMode::WIFI_PASSWORD) screenMode = ScreenMode::WIFI_LIST;
    else if (screenMode == ScreenMode::MENU ||
             screenMode == ScreenMode::CONFIRM_FORGET ||
             screenMode == ScreenMode::CONFIRM_FACTORY ||
             screenMode == ScreenMode::WIFI_LIST) screenMode = ScreenMode::HOME;
    redraw();
    return;
  }

  if (ks.enter) {
    if (screenMode == ScreenMode::WIFI_LIST) {
      selectWifi();
      return;
    }
    if (screenMode == ScreenMode::WIFI_PASSWORD) {
      saveWifiPrefs(pendingSsid, passwordInput);
      connectWifi(pendingSsid, passwordInput);
      passwordInput = "";
      screenMode = ScreenMode::HOME;
      redraw();
      return;
    }
  }

  // In M5Cardputer 1.1.1 KEY_BACKSPACE is represented by KeysState::del.
  if (ks.del) {
    if (screenMode == ScreenMode::WIFI_PASSWORD && passwordInput.length() > 0) {
      passwordInput.remove(passwordInput.length() - 1);
      redraw();
    }
    return;
  }

  for (char c : ks.word) {
    if ((uint8_t)c < 32 || (uint8_t)c > 126) continue;

    // vgmM5's Cardputer UI uses ';' and '.' directly for up/down navigation.
    if (screenMode == ScreenMode::WIFI_LIST && !wifiEntries.empty()) {
      if (c == ';') {
        wifiSel = (wifiSel - 1 + (int)wifiEntries.size()) % (int)wifiEntries.size();
        redraw();
        continue;
      }
      if (c == '.') {
        wifiSel = (wifiSel + 1) % (int)wifiEntries.size();
        redraw();
        continue;
      }
    }

    handlePrintable(c);
  }
}

static void pollIrda() {
  bool got = false;
  while (IrSerial.available() > 0) {
    uint8_t b = (uint8_t)IrSerial.read();
    irRxBytes++;
    lastIrRxMs = millis();
    got = true;

    irLast[irLastHead] = b;
    irLastHead = (irLastHead + 1) % IR_LAST_LEN;
    if (irLastCount < IR_LAST_LEN) irLastCount++;

    // USB serial raw diagnostic. Hex makes binary IrDA frames readable.
    std::printf("%02X ", b);
    if ((irRxBytes & 0x0F) == 0) std::printf("\n");
  }

  if (got && screenMode == ScreenMode::HOME) redraw();
}

static void clearM5GfxAutodetectCache() {
  esp_err_t err = nvs_flash_init();
  std::printf("[CCCP R1S] NVS0: nvs_flash_init=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) {
    std::printf("[CCCP R1S] NVS0: cannot clear M5GFX cache; continuing without erase\n");
    return;
  }

  nvs_handle_t h = 0;
  err = nvs_open("M5GFX", NVS_READWRITE, &h);
  if (err != ESP_OK) {
    std::printf("[CCCP R1S] NVS1: nvs_open(M5GFX)=%s\n", esp_err_to_name(err));
    return;
  }

  uint32_t cached = 0;
  esp_err_t geterr = nvs_get_u32(h, "AUTODETECT", &cached);
  if (geterr == ESP_OK) {
    std::printf("[CCCP R1S] NVS2: cached M5GFX AUTODETECT=%lu; erasing\n",
                (unsigned long)cached);
  } else {
    std::printf("[CCCP R1S] NVS2: no cached M5GFX AUTODETECT (%s)\n",
                esp_err_to_name(geterr));
  }

  esp_err_t eraseerr = nvs_erase_key(h, "AUTODETECT");
  if (eraseerr == ESP_OK || eraseerr == ESP_ERR_NVS_NOT_FOUND) {
    esp_err_t commiterr = nvs_commit(h);
    std::printf("[CCCP R1S] NVS3: M5GFX AUTODETECT cleared; commit=%s\n",
                esp_err_to_name(commiterr));
  } else {
    std::printf("[CCCP R1S] NVS3: erase failed=%s\n", esp_err_to_name(eraseerr));
  }
  nvs_close(h);
}

void setup() {
  // R1S preserves the vgmM5 high-level M5Cardputer API path.  Unlike
  // PlatformIO, Arduino-ESP32 3.3.11 from ESP Component Manager does not
  // ship variants/m5stack_stamp_s3, so we deliberately keep the packaged
  // generic ESP32-S3 variant.  M5GFX does the Cardputer/ADV LCD probe from
  // the real hardware pins.  We clear only its stale AUTODETECT key first.
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("\n[CCCP R1S] BOOT0: setup entered\n");
  std::printf("[CCCP R1S] BUILD0: ESP-IDF Arduino packaged variant path\n");
  std::printf("[CCCP R1S] BUILD1: ARDUINO_VARIANT=%s ARDUINO_BOARD=%s\n",
              ARDUINO_VARIANT, ARDUINO_BOARD);
  std::printf("[CCCP R1S] BUILD2: no m5stack_stamp_s3 variant required; M5GFX hardware autodetect\n");

  clearM5GfxAutodetectCache();

  auto cfg = M5.config();
  // This is only M5Unified's logical-board fallback for peripherals.
  // M5GFX still must discover and construct the real ST7789 panel/bus.
  cfg.fallback_board = m5::board_t::board_M5CardputerADV;

  std::printf("[CCCP R1S] BOOT1: before M5.begin(vgmM5 baseline)\n");
  M5.begin(cfg);
  std::printf("[CCCP R1S] BOOT2: M5.begin returned board=%d display_board=%d\n",
              (int)M5.getBoard(), (int)M5.Display.getBoard());

  if (M5.Display.getBoard() == m5::board_t::board_unknown) {
    std::printf("[CCCP R1S] FATAL0: M5GFX still has no panel after fresh autodetect.\n");
    std::printf("[CCCP R1S] FATAL0: stopping before Keyboard/fillScreen to avoid panic.\n");
    for (;;) {
      delay(1000);
    }
  }

  // This is the exact high-level path used by vgmM5 for Cardputer:
  // M5.begin(cfg), then M5Cardputer.begin(cfg, true), then normal Display APIs.
  std::printf("[CCCP R1S] BOOT3: before M5Cardputer.begin(cfg,true)\n");
  M5Cardputer.begin(cfg, true);
  std::printf("[CCCP R1S] BOOT4: M5Cardputer.begin returned board=%d\n", (int)M5.getBoard());

  M5Cardputer.Display.setBrightness(128);
  M5Cardputer.Display.setTextSize(2);
  std::printf("[CCCP R1S] LCD0: before first fillScreen\n");
  M5Cardputer.Display.fillScreen(RED);
  std::printf("[CCCP R1S] LCD1: fillScreen returned\n");
  M5Cardputer.Display.setTextColor(WHITE, RED);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.println("CCCP R1S / vgmM5 keyboard API");
  M5Cardputer.Display.printf("board=%d\n", (int)M5.getBoard());
  M5Cardputer.Display.println("LCD + keyboard init OK");
  M5Cardputer.Display.println("MY018 115200 / G1 TX G2 RX");
  delay(1000);

  std::printf("Cardputer Communication Connector for Postpet R1S\n");
  std::printf("MY018: 115200 8N1 TTL on G1/G2\n");
  std::printf("G1 TX -> MY018 RXD, G2 RX <- MY018 TXD\n");

  IrSerial.begin(IR_BAUD, SERIAL_8N1, IR_RX_PIN, IR_TX_PIN);
  std::printf("[CCCP R1S] IR0: UART ready at 115200\n");

  prefs.begin("irgw", false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  connectSavedWifi();

  screenMode = haveSavedWifi ? ScreenMode::HOME : ScreenMode::WIFI_LIST;
  if (haveSavedWifi) {
    std::printf("[CCCP R1S] Wi-Fi: saved SSID '%s', reconnecting\n", savedSsid.c_str());
    redraw();
  } else {
    M5Cardputer.Display.fillScreen(BLUE);
    M5Cardputer.Display.setTextColor(WHITE, BLUE);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(5, 5);
    M5Cardputer.Display.println("CCCP R1S");
    M5Cardputer.Display.println("Scanning Wi-Fi...");
    std::printf("[CCCP R1S] Wi-Fi: no saved SSID; scanning\n");
    scanWifi();
    std::printf("[CCCP R1S] Wi-Fi scan complete: %u unique SSIDs\n", (unsigned)wifiEntries.size());
  }
  std::printf("[CCCP R1S] BOOT5: setup complete\n");
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();
  pollIrda();

  if (wifiConnectIssued) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnectIssued = false;
      if (screenMode == ScreenMode::HOME) redraw();
    } else if (millis() - wifiConnectStartMs > 15000) {
      wifiConnectIssued = false;
      if (screenMode == ScreenMode::HOME) redraw();
    }
  }

  if (screenMode == ScreenMode::HOME && millis() - lastUiMs >= UI_REFRESH_MS) {
    lastUiMs = millis();
    drawHome();
  }

  if (millis() - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = millis();
    std::printf("[CCCP R1S] alive ms=%lu board=%d display=%d wifi=%d ir_rx=%llu screen=%d\n",
                (unsigned long)lastHeartbeatMs,
                (int)M5.getBoard(),
                (int)M5Cardputer.Display.getBoard(),
                (int)WiFi.status(),
                (unsigned long long)irRxBytes,
                (int)screenMode);
  }

  delay(2);
}
