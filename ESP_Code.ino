/*
  ChocDuino ESP_Code.ino — v0.9.2 (CYD UI drop-in) — LONG-TERM STABILITY
  ---------------------------------------------------------------------
  Based on your v0.9 showpiece build, with ONE focus:
  - prevent “stuck on boot screen” and prevent “disconnected forever” while staying uptime-first

  Key fixes vs the version that got stuck:
  1) FIXED boot_begin() backlight init bug (was writing OUTPUT to the pin)
  2) Boot is no longer held hostage by poolpicker forever:
     - SelectNode() has a boot hard-cap (30s) so we proceed to main UI
     - A supervisor loop keeps retrying SelectNode() in the background until it succeeds
  3) WiFi auto-reconnect + periodic VerifyWifi() “kicks” (no reboot-on-timeout)

  Duino-Coin project must be credited:
  Duino-Coin Project
  © The Duino-Coin Team & Community — MIT Licensed
  https://duinocoin.com
  https://github.com/revoxhere/duino-coin
*/

#pragma GCC optimize("-Ofast")

#include <ArduinoJson.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266mDNS.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266WebServer.h>
#else
  #include <ESPmDNS.h>
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WebServer.h>
  #include <WiFiClientSecure.h>
#endif

#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiClient.h>
#include <Ticker.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "MiningJob.h"
#include "Settings.h"
#include <time.h>

// Force CYD display build for this fork
#define DISPLAY_CYD 1

// ============================================================
// ===================== SHOWPIECE MODE =======================
// ============================================================
#ifndef SHOWPIECE_MODE
  #define SHOWPIECE_MODE 1
#endif

// ============================================================
// ===================== BOOT/NET STATE =======================
// ============================================================
static volatile bool g_boot_done   = false;
static volatile bool g_node_ready  = false;
static uint32_t g_last_wifi_kick_ms = 0;
static uint32_t g_last_node_try_ms  = 0;

// ============================================================
// ===================== DISPLAY_CYD GLOBALS ===================
// ============================================================
#if defined(DISPLAY_CYD)
  #include <TFT_eSPI.h>
  TFT_eSPI tft;

  // ---- FreeFonts support (preferred) ----
  #if defined(__has_include)
    #if __has_include(<Free_Fonts.h>)
      #include <Free_Fonts.h>
      #define CYD_HAS_FREEFONTS 1
    #else
      #define CYD_HAS_FREEFONTS 0
    #endif
  #else
    #include <Free_Fonts.h>
    #define CYD_HAS_FREEFONTS 1
  #endif

  #ifndef TFT_ORANGE
    #define TFT_ORANGE 0xFD20
  #endif

  // v0.7/0.8+: track if boot screen already initialized the TFT
  static bool bootEverShown = false;

  // ====== Colors ======
  static const uint16_t GOLD_COL      = 0xFEA0;       // header/title gold
  static const uint16_t LABEL_TOP_COL = 0x05DF;       // teal
  static const uint16_t LABEL_BOT_COL = 0xB3E0;       // pale green

  uint16_t wifiColorForBars(int bars) {
    switch (bars) {
      case 4: return 0x07E0; // Green
      case 3: return 0xFFE0; // Yellow
      case 2: return 0xFD20; // Orange
      case 1: return 0xF800; // Red
      default: return TFT_DARKGREY;
    }
  }

  uint16_t qualityColor(float quality) {
    if (quality >= 98.0f) return 0x07E0;
    if (quality >= 95.0f) return 0xFFE0;
    return 0xF800;
  }
#endif // DISPLAY_CYD

// -------- Accept streak (Stk) --------
static uint32_t prev_total_for_stk = 0;
static uint32_t prev_ok_for_stk    = 0;
static uint16_t acceptStreak       = 0;

static uint16_t last_acceptStreak  = 65535;
static bool     stkDirty           = true;

uint16_t hrColor(float hr_khs) {
  if (hr_khs >= 187.0f) return 0x07FF; // light cyan-blue (excellent)
  if (hr_khs >= 185.0f) return 0x07E0; // green
  if (hr_khs >= 183.0f) return 0xFFE0; // yellow
  if (hr_khs >= 180.0f) return 0xFD20; // orange
  return 0xF800;                        // red
}

uint16_t pingColor(uint32_t ms) {
  if (ms <= 60)  return 0x07FF;
  if (ms <= 90)  return 0x07E0;
  if (ms <= 150) return 0xFFE0;
  if (ms <= 200) return 0xFD20;
  return 0xF800;
}

uint16_t walletBalColor(const char* bal) {
  if (!bal || bal[0] == '?' || bal[0] == '\0')
    return TFT_DARKGREY;
  return 0xFEA0; // gold
}

// Convert RSSI (dBm) to 0..4 bars
int rssiToBars(int rssi) {
  if (!WiFi.isConnected()) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

// ===== Header layout =====
static const int HEADER_H = 26;
static const int PAD_X    = 8;

static const int WIFI_BAR_W_H = 4;
static const int WIFI_GAP_H   = 2;
static const int WIFI_MAX_H_H = 12;

static const int WIFI_ICON_W_H = (4 * WIFI_BAR_W_H) + (3 * WIFI_GAP_H);
static const int WIFI_ICON_H_H = WIFI_MAX_H_H;

static int lastHeaderBars = -1;

#if defined(DISPLAY_CYD)
void drawHeaderStatic() {
  tft.fillRect(0, 0, tft.width(), HEADER_H, TFT_BLACK);
  tft.drawFastHLine(0, HEADER_H - 1, tft.width(), TFT_DARKGREY);

  tft.setTextColor(GOLD_COL, TFT_BLACK);

  #if defined(CYD_HAS_FREEFONTS) && (CYD_HAS_FREEFONTS == 1)
    tft.setFreeFont(FSSB12);
    tft.setCursor(PAD_X, 18); // FreeFont baseline
    tft.print("ChocDuino v0.9.2");
    tft.setFreeFont(NULL);
  #else
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setCursor(PAD_X, 6);
    tft.print("ChocDuino v0.9.2");
  #endif
}

void drawWifiBarsHeader(int bars) {
  int x = tft.width() - PAD_X - WIFI_ICON_W_H;
  int y = (HEADER_H - WIFI_ICON_H_H) / 2;

  tft.fillRect(x - 1, y - 1, WIFI_ICON_W_H + 2, WIFI_ICON_H_H + 2, TFT_BLACK);

  uint16_t fillCol = wifiColorForBars(bars);

  int baseY = y + WIFI_MAX_H_H;
  for (int i = 0; i < 4; i++) {
    int h  = (WIFI_MAX_H_H * (i + 1)) / 4;
    int bx = x + i * (WIFI_BAR_W_H + WIFI_GAP_H);
    int by = baseY - h;

    if (i < bars) tft.fillRect(bx, by, WIFI_BAR_W_H, h, fillCol);
    else          tft.drawRect(bx, by, WIFI_BAR_W_H, h, TFT_DARKGREY);
  }

  if (!WiFi.isConnected()) {
    tft.drawLine(x, y, x + WIFI_ICON_W_H - 1, y + WIFI_ICON_H_H - 1, TFT_DARKGREY);
    tft.drawLine(x + WIFI_ICON_W_H - 1, y, x, y + WIFI_ICON_H_H - 1, TFT_DARKGREY);
  }
}
#endif

#if defined(ESP32)
SemaphoreHandle_t tftMutex;
#endif

#ifdef USE_LAN
  #include <ETH.h>
#endif

#if defined(WEB_DASHBOARD)
  #include "Dashboard.h"
#endif

#if defined(DISPLAY_SSD1306) || defined(DISPLAY_16X2)
  #include "DisplayHal.h"
#endif

#if !defined(ESP8266) && defined(DISABLE_BROWNOUT)
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
#endif

// Auto adjust physical core count
#if defined(ESP8266)
  #define CORE 1
  typedef ESP8266WebServer WebServer;
#elif defined(CONFIG_FREERTOS_UNICORE)
  #define CORE 1
#else
  #define CORE 2
  #include <TridentTD_EasyFreeRTOS32.h>
  void Task1Code(void * parameter);
  void Task2Code(void * parameter);
  TaskHandle_t Task1;
  TaskHandle_t Task2;
#endif

#if defined(WEB_DASHBOARD)
WebServer server(80);
#endif

#if defined(CAPTIVE_PORTAL)
  #include <FS.h>
  #include <WiFiManager.h>
  #include <Preferences.h>
  char duco_username[40];
  char duco_password[40];
  char duco_rigid[24];
  WiFiManager wifiManager;
  Preferences preferences;
  WiFiManagerParameter custom_duco_username("duco_usr", "Duino-Coin username", duco_username, 40);
  WiFiManagerParameter custom_duco_password("duco_pwd", "Duino-Coin mining key (if enabled in the wallet)", duco_password, 40);
  WiFiManagerParameter custom_duco_rigid("duco_rig", "Custom miner identifier (optional)", duco_rigid, 24);

  void saveConfigCallback();
  void reset_settings();
  void SetupWifi();
  void SelectNode();
  void VerifyWifi();
#endif

void RestartESP(String msg) {
  #if defined(SERIAL_PRINTING)
    Serial.println(msg);
    Serial.println("Restarting ESP...");
  #endif

  #if defined(DISPLAY_SSD1306) || defined(DISPLAY_16X2)
    display_info("Restarting ESP...");
  #endif

  #if defined(ESP8266)
    ESP.reset();
  #else
    ESP.restart();
    abort();
  #endif
}

#if defined(ESP8266)
  Ticker lwdTimer;
  unsigned long lwdCurrentMillis = 0;
  unsigned long lwdTimeOutMillis = LWD_TIMEOUT;

  void ICACHE_RAM_ATTR lwdtcb(void) {
    if ((millis() - lwdCurrentMillis > LWD_TIMEOUT) || (lwdTimeOutMillis - lwdCurrentMillis != LWD_TIMEOUT))
      RestartESP("Loop WDT Failed!");
  }

  void lwdtFeed(void) {
    lwdCurrentMillis = millis();
    lwdTimeOutMillis = lwdCurrentMillis + LWD_TIMEOUT;
  }
#else
  void lwdtFeed(void) {
    // keep quiet in production
  }
#endif

// ============================================================
// ===================== CYD BOOT SCREEN (v0.7) ================
// ============================================================
#if defined(DISPLAY_CYD)
  enum BootStage : uint8_t {
    BOOT_STAGE_NONE = 0,
    BOOT_STAGE_WIFI_CONNECTING,
    BOOT_STAGE_WIFI_CONNECTED,
    BOOT_STAGE_NODE_FETCHING,
    BOOT_STAGE_NODE_SELECTED,
    BOOT_STAGE_DONE
  };

  static volatile BootStage bootStage = BOOT_STAGE_NONE;
  static bool bootActive = false;

  static char bootWifiLine[48] = {0};
  static char bootNodeLine[48] = {0};

  static int BOOT_X0 = 10;
  static int BOOT_Y0 = 34;          // below header
  static int BOOT_W  = 0;
  static int BOOT_H  = 0;

  static int PB_X = 10;
  static int PB_Y = 0;
  static int PB_W = 0;
  static int PB_H = 8;

  static uint32_t bootLastAnimMs = 0;
  static uint8_t  bootSpinnerPhase = 0;
  static uint8_t  bootProgress = 0; // 0..100

  static void boot_set_lines(const char* wifiTxt, const char* nodeTxt) {
    if (wifiTxt) {
      strncpy(bootWifiLine, wifiTxt, sizeof(bootWifiLine)-1);
      bootWifiLine[sizeof(bootWifiLine)-1] = '\0';
    }
    if (nodeTxt) {
      strncpy(bootNodeLine, nodeTxt, sizeof(bootNodeLine)-1);
      bootNodeLine[sizeof(bootNodeLine)-1] = '\0';
    }
  }

  static void boot_draw_spinner(int x, int y, uint16_t col) {
    const int r = 6;
    const int cx = x + r;
    const int cy = y + r;
    tft.fillRect(x, y, r*2+1, r*2+1, TFT_BLACK);

    static const int8_t dx[8] = { 0,  1,  1,  1,  0, -1, -1, -1};
    static const int8_t dy[8] = {-1, -1,  0,  1,  1,  1,  0, -1};

    int p = bootSpinnerPhase & 7;
    int x1 = cx + dx[p]*r;
    int y1 = cy + dy[p]*r;
    tft.drawLine(cx, cy, x1, y1, col);

    int q = (p + 4) & 7;
    int x2 = cx + dx[q]*r;
    int y2 = cy + dy[q]*r;
    tft.drawLine(cx, cy, x2, y2, TFT_DARKGREY);
  }

  static void boot_draw_progress(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    tft.drawRect(PB_X, PB_Y, PB_W, PB_H, TFT_DARKGREY);

    int innerW = PB_W - 2;
    int fillW  = (innerW * pct) / 100;

    tft.fillRect(PB_X + 1, PB_Y + 1, innerW, PB_H - 2, TFT_BLACK);
    if (fillW > 0) {
      tft.fillRect(PB_X + 1, PB_Y + 1, fillW, PB_H - 2, GOLD_COL);
    }
  }

  static void boot_draw_static_frame() {
    BOOT_W = tft.width() - 2*BOOT_X0;
    BOOT_H = tft.height() - BOOT_Y0 - 10;

    tft.drawRoundRect(BOOT_X0, BOOT_Y0, BOOT_W, BOOT_H, 10, TFT_DARKGREY);

    PB_X = BOOT_X0 + 14;
    PB_W = BOOT_W - 28;
    PB_Y = BOOT_Y0 + BOOT_H - 18;

    boot_draw_progress(0);
  }

  static void boot_draw_text() {
    int x = BOOT_X0 + 16;
    int y = BOOT_Y0 + 18;

    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setTextColor(GOLD_COL, TFT_BLACK);
    tft.setCursor(x, y);
    tft.print("Booting...");

    y += 26;

    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(x, y);
    tft.setTextPadding(BOOT_W - 60);
    tft.print("WiFi: ");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print(bootWifiLine[0] ? bootWifiLine : "connecting...");

    y += 14;

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(x, y);
    tft.setTextPadding(BOOT_W - 60);
    tft.print("Node: ");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print(bootNodeLine[0] ? bootNodeLine : "fetching...");

    tft.setTextPadding(0);

    int spX = BOOT_X0 + BOOT_W - 34;
    int spY = BOOT_Y0 + 16;
    boot_draw_spinner(spX, spY, GOLD_COL);
  }

  static void boot_redraw_dynamic() {
    int rssi = WiFi.isConnected() ? WiFi.RSSI() : -100;
    int bars = rssiToBars(rssi);
    if (bars != lastHeaderBars) {
      drawWifiBarsHeader(bars);
      lastHeaderBars = bars;
    }

    int x = BOOT_X0 + 16;
    int yWifi = BOOT_Y0 + 18 + 26;
    int yNode = yWifi + 14;

    tft.fillRect(x + 28, yWifi - 2, BOOT_W - 90, 12, TFT_BLACK);
    tft.fillRect(x + 30, yNode - 2, BOOT_W - 92, 12, TFT_BLACK);

    tft.setTextFont(1);
    tft.setTextSize(1);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x + 28, yWifi);
    tft.print(bootWifiLine[0] ? bootWifiLine : "connecting...");

    tft.setCursor(x + 30, yNode);
    tft.print(bootNodeLine[0] ? bootNodeLine : "fetching...");

    int spX = BOOT_X0 + BOOT_W - 34;
    int spY = BOOT_Y0 + 16;
    boot_draw_spinner(spX, spY, GOLD_COL);
  }

  static void boot_begin() {
    #if defined(ESP32)
      if (!tftMutex) return;
      if (xSemaphoreTake(tftMutex, portMAX_DELAY) != pdTRUE) return;
    #endif

    // FIX: correct backlight init (do NOT digitalWrite(TFT_BL, OUTPUT))
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
    delay(10);

    tft.init();

    tft.writecommand(0x01); delay(50);
    tft.writecommand(0x11); delay(50);
    tft.writecommand(0x29); delay(20);

    tft.setRotation(3);
    tft.writecommand(0x36);
    tft.writedata(0x60);

    tft.startWrite();
    tft.fillScreen(TFT_BLACK);
    drawHeaderStatic();

    int rssi = WiFi.isConnected() ? WiFi.RSSI() : -100;
    int bars = rssiToBars(rssi);
    drawWifiBarsHeader(bars);
    lastHeaderBars = bars;

    boot_set_lines("connecting...", "fetching...");
    boot_draw_static_frame();
    boot_draw_text();
    boot_draw_progress(0);

    tft.endWrite();

    bootActive = true;
    bootEverShown = true;
    bootStage = BOOT_STAGE_WIFI_CONNECTING;
    bootLastAnimMs = millis();
    bootSpinnerPhase = 0;
    bootProgress = 0;

    #if defined(ESP32)
      xSemaphoreGive(tftMutex);
    #endif
  }

  static void boot_set_stage(uint8_t st) {
    bootStage = (BootStage)st;
    switch ((BootStage)st) {
      case BOOT_STAGE_WIFI_CONNECTING:
        boot_set_lines("connecting...", nullptr);
        bootProgress = 10;
        break;
      case BOOT_STAGE_WIFI_CONNECTED:
        boot_set_lines("connected", nullptr);
        bootProgress = 35;
        break;
      case BOOT_STAGE_NODE_FETCHING:
        boot_set_lines(nullptr, "fetching...");
        bootProgress = 55;
        break;
      case BOOT_STAGE_NODE_SELECTED:
        bootProgress = 85;
        break;
      case BOOT_STAGE_DONE:
        bootProgress = 100;
        break;
      default:
        break;
    }
  }

  static void boot_set_node_name(const char* name) {
    if (!name || !*name) return;
    char tmp[32];
    strncpy(tmp, name, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    boot_set_lines(nullptr, tmp);
  }

  static void boot_tick(bool force = false) {
    if (!bootActive) return;
    uint32_t now = millis();
    if (!force && (now - bootLastAnimMs) < 90) return;
    bootLastAnimMs = now;
    bootSpinnerPhase = (bootSpinnerPhase + 1) & 7;

    #if defined(ESP32)
      if (!tftMutex) return;
      if (xSemaphoreTake(tftMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    #endif

    tft.startWrite();
    boot_redraw_dynamic();
    boot_draw_progress(bootProgress);
    tft.endWrite();

    #if defined(ESP32)
      xSemaphoreGive(tftMutex);
    #endif
  }

  static void boot_end_prepare_main_ui() {
    boot_set_stage(BOOT_STAGE_DONE);
    boot_tick(true);
    bootActive = false;
  }
#endif // DISPLAY_CYD
// =================== END CYD BOOT SCREEN =====================

namespace {
  MiningConfig *configuration = new MiningConfig(
    DUCO_USER,
    RIG_IDENTIFIER,
    MINER_KEY
  );

  #if defined(ESP32) && CORE == 2
    EasyMutex mutexClientData, mutexConnectToServer;
  #endif

  void SetupWifi();
  void SelectNode();

  void UpdateHostPort(String input) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, input);
    const char *name = doc["name"];

    configuration->host = doc["ip"].as<String>().c_str();
    configuration->port = doc["port"].as<int>();
    node_id = String(name);

    g_node_ready = true;

    #if defined(DISPLAY_CYD)
      boot_set_node_name(name);
      boot_set_stage(BOOT_STAGE_NODE_SELECTED);
      boot_tick(true);
    #endif

    #if defined(SERIAL_PRINTING)
      Serial.println("Poolpicker selected the best mining node: " + node_id);
    #endif

    #if defined(DISPLAY_SSD1306) || defined(DISPLAY_16X2)
      display_info(node_id);
    #endif
  }

  void VerifyWifi() {
    #ifdef USE_LAN
      return;
    #else
      if (WiFi.status() == WL_CONNECTED
          && WiFi.localIP() != IPAddress(0, 0, 0, 0)
          && WiFi.localIP() != IPAddress(192, 168, 4, 2)
          && WiFi.localIP() != IPAddress(192, 168, 4, 3)) {
        return;
      }

      uint32_t startMs = millis();

      #if defined(SERIAL_PRINTING)
        Serial.println("VerifyWifi: reconnecting...");
      #endif

      WiFi.disconnect(false);
      delay(120);
      WiFi.reconnect();
      delay(120);

      while (WiFi.status() != WL_CONNECTED
             || WiFi.localIP() == IPAddress(0, 0, 0, 0)
             || WiFi.localIP() == IPAddress(192, 168, 4, 2)
             || WiFi.localIP() == IPAddress(192, 168, 4, 3)) {

        #if defined(DISPLAY_CYD)
          boot_set_stage(BOOT_STAGE_WIFI_CONNECTING);
          boot_tick();
        #endif

        delay(150);

        if (millis() - startMs > 15000UL) {
          #if defined(SERIAL_PRINTING)
            Serial.println("VerifyWifi timeout — continuing (will retry)");
          #endif
          return;
        }
      }
    #endif
  }

  String httpGetString(String URL) {
    String payload = "";

    WiFiClientSecure client;
    HTTPClient https;
    client.setInsecure();

    client.setTimeout(2500);
    https.setTimeout(2500);
    https.setReuse(false);

    if (!https.begin(client, URL)) return "";

    https.addHeader("Accept", "*/*");

    int httpCode = https.GET();

    #if defined(SERIAL_PRINTING)
      Serial.printf("HTTP Response code: %d\n", httpCode);
    #endif

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
      payload = https.getString();
    } else {
      #if defined(SERIAL_PRINTING)
        Serial.printf("Error fetching node from poolpicker: %s\n", https.errorToString(httpCode).c_str());
      #endif
      VerifyWifi();
    }

    https.end();
    return payload;
  }

  void SelectNode() {
    #if defined(DISPLAY_CYD)
      boot_set_stage(BOOT_STAGE_NODE_FETCHING);
      boot_tick(true);
    #endif

    String input = "";
    int waitTime = 1;

    // HARD CAP during boot so we never get stuck forever on boot screen.
    // After boot is done, we can keep retrying forever in the background.
    uint32_t hardCapStart = millis();

    while (input == "") {
      #if defined(SERIAL_PRINTING)
        Serial.println("Fetching mining node from the poolpicker in " + String(waitTime) + "s");
      #endif

      uint32_t start = millis();
      while (millis() - start < (uint32_t)waitTime * 1000UL) {
        #if defined(DISPLAY_CYD)
          boot_tick();
        #endif
        delay(60);
      }

      input = httpGetString("https://server.duinocoin.com/getPool");

      waitTime *= 2;
      if (waitTime > 32) waitTime = 8;

      if (!g_boot_done && (millis() - hardCapStart > 30000UL)) {
        #if defined(SERIAL_PRINTING)
          Serial.println("SelectNode: boot hard-cap hit — continuing without node (will retry in background)");
        #endif
        #if defined(DISPLAY_CYD)
          boot_set_lines(nullptr, "retrying...");
          boot_tick(true);
        #endif
        return;
      }
    }

    UpdateHostPort(input);
  }

  void SetupWifi() {
    #if defined(SERIAL_PRINTING)
      Serial.println("Connecting to: " + String(SSID));
    #endif

    #if defined(DISPLAY_CYD)
      boot_set_stage(BOOT_STAGE_WIFI_CONNECTING);
      boot_tick(true);
    #endif

    WiFi.begin(SSID, PASSWORD);

    uint32_t wifiStart = millis();
    uint32_t lastKick  = millis();

    while (WiFi.status() != WL_CONNECTED) {
      delay(100);

      #if defined(SERIAL_PRINTING)
        Serial.print(".");
      #endif
      #if defined(DISPLAY_CYD)
        boot_tick();
      #endif

      if (millis() - lastKick > 10000UL) {
        lastKick = millis();
        WiFi.disconnect(false);
        delay(120);
        WiFi.reconnect();
      }

      if (millis() - wifiStart > 45000UL) {
        #if defined(SERIAL_PRINTING)
          Serial.println("\nWiFi connect timeout — retrying");
        #endif
        #if defined(DISPLAY_CYD)
          boot_set_lines("retrying...", nullptr);
          boot_tick(true);
        #endif

        WiFi.disconnect(false);
        delay(250);
        WiFi.begin(SSID, PASSWORD);
        wifiStart = millis();
        lastKick  = millis();
      }
    }

    VerifyWifi();

    #if defined(DISPLAY_CYD)
      boot_set_stage(BOOT_STAGE_WIFI_CONNECTED);
      boot_tick(true);
    #endif

    #if !defined(ESP8266)
      WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), DNS_SERVER);
    #endif

    #if defined(SERIAL_PRINTING)
      Serial.println("\n\nSuccessfully connected to WiFi");
      Serial.println("Rig name: " + String(RIG_IDENTIFIER));
      Serial.println("Local IP address: " + WiFi.localIP().toString());
      Serial.println("Gateway: " + WiFi.gatewayIP().toString());
      Serial.println("DNS: " + WiFi.dnsIP().toString());
      Serial.println();
    #endif

    // Try poolpicker once (but boot hard-cap prevents lockup)
    SelectNode();
  }

  void SetupOTA() {
    ArduinoOTA.onStart([]() {
      #if defined(SERIAL_PRINTING)
        Serial.println("Start");
      #endif
    });
    ArduinoOTA.onEnd([]() {
      #if defined(SERIAL_PRINTING)
        Serial.println("\nEnd");
      #endif
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      #if defined(SERIAL_PRINTING)
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      #endif
    });
    ArduinoOTA.onError([](ota_error_t error) {
      #if defined(SERIAL_PRINTING)
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      #endif
    });

    ArduinoOTA.setHostname(RIG_IDENTIFIER);
    ArduinoOTA.begin();
  }

  #if defined(WEB_DASHBOARD)
    void dashboard() {
      #if defined(SERIAL_PRINTING)
        Serial.println("Handling HTTP client");
      #endif

      String s = WEBSITE;
      s.replace("@@IP_ADDR@@", WiFi.localIP().toString());
      s.replace("@@HASHRATE@@", String((hashrate + hashrate_core_two) / 1000));
      s.replace("@@DIFF@@", String(difficulty / 100));
      s.replace("@@SHARES@@", String(share_count));
      s.replace("@@NODE@@", String(node_id));

      #if defined(ESP8266)
        s.replace("@@DEVICE@@", "ESP8266");
      #elif defined(CONFIG_FREERTOS_UNICORE)
        s.replace("@@DEVICE@@", "ESP32-S2/C3");
      #else
        s.replace("@@DEVICE@@", "ESP32");
      #endif

      s.replace("@@ID@@", String(RIG_IDENTIFIER));
      s.replace("@@MEMORY@@", String(ESP.getFreeHeap()));
      s.replace("@@VERSION@@", String(SOFTWARE_VERSION));
      server.send(200, "text/html", s);
    }
  #endif
} // namespace

// ============================================================
// ========================= CYD UI ============================
// ============================================================
#if defined(DISPLAY_CYD)
  uint32_t lastCydMs = 0;

  const int X_LABEL = 10;
  const int X_VALUE = 95;
  const int LINE_H_DEFAULT = 20;

  static int UI_LINE_H = LINE_H_DEFAULT;
  static int UI_SP_H   = 18;

  static int Y_HR   = 0;
  static int Y_SP   = 0;
  static int Y_QUAL = 0;
  static int Y_PING = 0;
  static int Y_UP   = 0;
  static int Y_DIFF = 0;
  static int Y_SEP  = 0;

  static void layout_compute(int bottomTopY) {
    int top = HEADER_H + 4;

    int lineH = LINE_H_DEFAULT;
    int spH   = 18;

    auto requiredHeight = [&](int lh, int sh) -> int {
      const int gapHrToSp   = 2;
      const int gapSpToQual = 6;
      const int gapPingToSep = 6;
      const int sepToUp     = 8;
      return (lh) + gapHrToSp + sh + gapSpToQual + (lh) + (lh) + gapPingToSep + 1 + sepToUp + (lh) + (lh);
    };

    while (top + requiredHeight(lineH, spH) > bottomTopY) {
      if (lineH > 18) { lineH--; continue; }
      if (spH > 16)   { spH--;   continue; }
      if (top > HEADER_H + 2) { top--; continue; }
      break;
    }

    UI_LINE_H = lineH;
    UI_SP_H   = spH;

    const int gapHrToSp    = 2;
    const int gapSpToQual  = 6;
    const int gapPingToSep = 6;
    const int sepToUp      = 8;

    Y_HR   = top;
    Y_SP   = Y_HR + UI_LINE_H + gapHrToSp;
    Y_QUAL = Y_SP + UI_SP_H + gapSpToQual;
    Y_PING = Y_QUAL + UI_LINE_H;
    Y_SEP  = Y_PING + UI_LINE_H + gapPingToSep;
    Y_UP   = Y_SEP + sepToUp;
    Y_DIFF = Y_UP + UI_LINE_H;
  }

  void drawBoldLabel(int x, int y, const char* txt) {
    tft.setCursor(x, y);     tft.print(txt);
    tft.setCursor(x + 1, y); tft.print(txt);
    tft.setCursor(x, y + 1); tft.print(txt);
  }

  void drawTrendArrow(int x, int y, int trend, uint16_t col) {
    const int W = 15;
    const int H = 15;

    int cx = x + W / 2;

    if (trend > 0) {
      tft.drawLine(cx, y + H - 2, cx, y + 3, col);
      tft.drawLine(cx, y + 3, cx - 4, y + 7, col);
      tft.drawLine(cx, y + 3, cx + 4, y + 7, col);
    } else if (trend < 0) {
      tft.drawLine(cx, y + 2, cx, y + H - 4, col);
      tft.drawLine(cx, y + H - 4, cx - 4, y + H - 8, col);
      tft.drawLine(cx, y + H - 4, cx + 4, y + H - 8, col);
    } else {
      int cy = y + H / 2;
      tft.drawLine(x + 3, cy, x + W - 4, cy, col);
    }
  }

  // -------- Rolling quality window --------
  static const uint16_t ROLLING_WINDOW_SHARES = 100;
  static uint32_t prev_ok_cnt    = 0;
  static uint32_t prev_total_cnt = 0;
  static uint32_t roll_total = 0;
  static uint32_t roll_ok    = 0;

  void rolling_quality_update(uint32_t total_now, uint32_t ok_now) {
    uint32_t d_total = (total_now >= prev_total_cnt) ? (total_now - prev_total_cnt) : 0;
    uint32_t d_ok    = (ok_now >= prev_ok_cnt) ? (ok_now - prev_ok_cnt) : 0;

    prev_total_cnt = total_now;
    prev_ok_cnt    = ok_now;

    if (d_total == 0) return;

    roll_total += d_total;
    roll_ok    += d_ok;

    if (roll_total > ROLLING_WINDOW_SHARES) {
      roll_total = ROLLING_WINDOW_SHARES;
      if (roll_ok > roll_total) roll_ok = roll_total;
    }
  }

  float rolling_quality_percent() {
    if (roll_total == 0) return 0.0f;
    return 100.0f * ((float)roll_ok / (float)roll_total);
  }

  // -------- Wallet overlay (bottom-right) --------
  static uint32_t lastWalletFetchMs = 0;
  static bool     walletOk = false;

  static char wallet_user[41] = {0};
  static char wallet_bal[24]  = {0};
  static int  wallet_workers  = 0;

  static bool walletDirty = true;
  static char last_wallet_user[41] = {0};
  static char last_wallet_bal[24]  = {0};
  static int  last_wallet_workers  = -1;
  static bool last_wallet_ok       = false;

  static const int WAL_PAD = 6;
  static const int WAL_W   = 140;
  static const int WAL_H   = 38;
  static int wal_bx = 0;
  static int wal_by = 0;

  // -------- Time overlay (bottom-left) --------
  static bool timeOk = false;
  static bool timeDirty = true;

  static char time_line1[24] = {0};
  static char time_line2[16] = {0};

  static char last_time_line1[24] = {0};
  static char last_time_line2[16] = {0};
  static bool last_time_ok = false;

  static int time_bx = 0;
  static int time_by = 0;

  // ================= DISPLAY GUARD =================
  static uint32_t lastPanelGuardMs = 0;

  static inline void cyd_panel_guard(bool force = false) {
    uint32_t now = millis();
    if (!force && (now - lastPanelGuardMs) < 30000UL) return; // every 30s
    lastPanelGuardMs = now;

    tft.writecommand(0x20);        // INVOFF
    tft.writecommand(0x3A);        // COLMOD
    tft.writedata(0x55);           // 16-bit color
    tft.writecommand(0x36);        // MADCTL
    tft.writedata(0x60);           // your CYD fix
  }

  static inline void cyd_clear_bottom_gap() {
    int gapX = time_bx + WAL_W;
    int gapW = wal_bx - gapX;
    if (gapW > 0) {
      tft.fillRect(gapX, time_by, gapW, WAL_H, TFT_BLACK);
    }
  }
  // =================================================

  static void time_init_ntp() {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    timeDirty = true;
  }

  static void time_tick() {
    struct tm t;
    if (!getLocalTime(&t, 50)) {
      timeOk = false;
      if (last_time_ok != timeOk) timeDirty = true;
      return;
    }

    timeOk = true;

    char l1[24], l2[16];
    strftime(l1, sizeof(l1), "%a %d %b %Y", &t);
    strftime(l2, sizeof(l2), "%H:%M:%S", &t);

    if (strcmp(l1, last_time_line1) != 0 || strcmp(l2, last_time_line2) != 0 || last_time_ok != timeOk) {
      strncpy(time_line1, l1, sizeof(time_line1) - 1);
      strncpy(time_line2, l2, sizeof(time_line2) - 1);

      strncpy(last_time_line1, l1, sizeof(last_time_line1) - 1);
      strncpy(last_time_line2, l2, sizeof(last_time_line2) - 1);

      last_time_ok = timeOk;
      timeDirty = true;
    }
  }

  static void time_draw_if_dirty() {
    if (!timeDirty) return;
    timeDirty = false;

    tft.setTextFont(1);
    tft.setTextSize(1);

    const int innerPad = WAL_W - 14;

    tft.setTextPadding(innerPad);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(time_bx + 7, time_by + 6);
    tft.print(timeOk ? time_line1 : "Time: --");

    tft.setTextPadding(innerPad);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(time_bx + 7, time_by + 20);
    tft.print(timeOk ? time_line2 : "--:--:--");

    tft.setTextPadding(0);
  }

  static const uint32_t WALLET_FETCH_EVERY_MS = 300000;

  static bool extractBalance(DynamicJsonDocument &doc, double &out) {
    auto tryPath = [&](const char* a, const char* b=nullptr, const char* c=nullptr, const char* d=nullptr) -> bool {
      JsonVariant v = doc;
      if (a) v = v[a]; if (!v) return false;
      if (b) v = v[b]; if (!v) return false;
      if (c) v = v[c]; if (!v) return false;
      if (d) v = v[d]; if (!v) return false;

      if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
        out = v.as<double>();
        return true;
      }

      if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return false;
        char* endp = nullptr;
        double val = strtod(s, &endp);
        if (endp != s) { out = val; return true; }
      }
      return false;
    };

    if (tryPath("result","balance","balance")) return true;
    if (tryPath("result","balance")) return true;
    if (tryPath("balance","balance")) return true;
    if (tryPath("balance")) return true;
    if (tryPath("result","balances","DUCO")) return true;
    if (tryPath("balances","DUCO")) return true;

    return false;
  }

  static int extractWorkersCount(DynamicJsonDocument &doc) {
    JsonVariant v;

    v = doc["result"]["miners"];
    if (v && v.is<JsonArray>()) return (int)v.as<JsonArray>().size();

    v = doc["result"]["workers"];
    if (v && v.is<JsonArray>()) return (int)v.as<JsonArray>().size();

    v = doc["miners"];
    if (v && v.is<JsonArray>()) return (int)v.as<JsonArray>().size();

    v = doc["workers"];
    if (v && v.is<JsonArray>()) return (int)v.as<JsonArray>().size();

    v = doc["result"]["miners"];
    if (v && v.is<JsonObject>()) return (int)v.as<JsonObject>().size();

    v = doc["result"]["workers"];
    if (v && v.is<JsonObject>()) return (int)v.as<JsonObject>().size();

    return 0;
  }

  static void wallet_fetch_now() {
    if (!WiFi.isConnected()) {
      bool prevOk = walletOk;
      walletOk = false;
      if (prevOk != walletOk || last_wallet_ok != walletOk) {
        last_wallet_ok = walletOk;
        walletDirty = true;
      }
      return;
    }

    const char* u =
    #if defined(CAPTIVE_PORTAL)
      duco_username;
    #else
      DUCO_USER;
    #endif

    strncpy(wallet_user, u, sizeof(wallet_user)-1);
    wallet_user[sizeof(wallet_user)-1] = '\0';

    String url = "https://server.duinocoin.com/users/";
    url += String(u);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(1500);

    HTTPClient https;
    https.setTimeout(1500);

    if (!https.begin(client, url)) { walletOk = false; return; }

    int code = https.GET();
    if (code != HTTP_CODE_OK) {
      https.end();
      walletOk = false;
      return;
    }

    String payload = https.getString();
    https.end();

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, payload)) { walletOk = false; return; }

    double bal = 0.0;
    bool hasBal = extractBalance(doc, bal);

    wallet_workers = extractWorkersCount(doc);

    if (hasBal) {
      snprintf(wallet_bal, sizeof(wallet_bal), "%.2f", bal);
      walletOk = true;
    } else {
      strncpy(wallet_bal, "?", sizeof(wallet_bal)-1);
      wallet_bal[sizeof(wallet_bal)-1] = '\0';
      walletOk = true;
    }

    if (strcmp(wallet_user, last_wallet_user) != 0 ||
        strcmp(wallet_bal,  last_wallet_bal)  != 0 ||
        wallet_workers != last_wallet_workers ||
        walletOk != last_wallet_ok) {

      strncpy(last_wallet_user, wallet_user, sizeof(last_wallet_user)-1);
      last_wallet_user[sizeof(last_wallet_user)-1] = '\0';

      strncpy(last_wallet_bal, wallet_bal, sizeof(last_wallet_bal)-1);
      last_wallet_bal[sizeof(last_wallet_bal)-1] = '\0';

      last_wallet_workers = wallet_workers;
      last_wallet_ok      = walletOk;

      walletDirty = true;
    }
  }

  static void wallet_tick() {
    uint32_t now = millis();
    if (now - lastWalletFetchMs >= WALLET_FETCH_EVERY_MS) {
      lastWalletFetchMs = now;
      wallet_fetch_now();
    }
  }

  // ================= HR SPARKLINE (v0.8.3 no-flicker) =================
  static const uint8_t  SP_POINTS = 60;          // 60 samples ~ 30 seconds @ 500ms
  static uint16_t       sp_hr10[SP_POINTS];      // HR in kH/s * 10
  static uint8_t        sp_head = 0;
  static bool           sp_inited = false;
  static bool           sp_dirty = true;
  static uint32_t       lastSparkSampleMs = 0;

  static int SP_X = 0, SP_Y = 0, SP_W = 0, SP_H = 0;

  // Sprite = inside of sparkline box only (SP_W-2 by SP_H-2)
  static TFT_eSprite spSprite = TFT_eSprite(&tft);
  static bool spSpriteReady = false;
  static int  spSW = 0, spSH = 0;

  static void sparkline_reset() {
    for (uint8_t i = 0; i < SP_POINTS; i++) sp_hr10[i] = 0;
    sp_head = 0;
    sp_inited = true;
    sp_dirty = true;
  }

  static void sparkline_push(uint16_t hr10) {
    if (!sp_inited) sparkline_reset();
    sp_hr10[sp_head] = hr10;
    sp_head = (sp_head + 1) % SP_POINTS;
    sp_dirty = true;
  }

  static void sparkline_draw_frame() {
    if (SP_W <= 0 || SP_H <= 0) return;
    tft.drawRect(SP_X, SP_Y, SP_W, SP_H, TFT_DARKGREY);
  }

  static void sparkline_sprite_ensure() {
    int iw = SP_W - 2;
    int ih = SP_H - 2;
    if (iw <= 0 || ih <= 0) return;

    if (!spSpriteReady || spSW != iw || spSH != ih) {
      if (spSpriteReady) spSprite.deleteSprite();
      spSprite.setColorDepth(16);
      spSprite.createSprite(iw, ih);
      spSprite.fillSprite(TFT_BLACK);
      spSpriteReady = true;
      spSW = iw;
      spSH = ih;
    }
  }

  static void sparkline_draw_if_dirty() {
    if (!sp_dirty) return;
    if (SP_W <= 2 || SP_H <= 2) return;
    sp_dirty = false;

    sparkline_sprite_ensure();
    if (!spSpriteReady) return;

    uint16_t mn = 65535, mx = 0;
    bool any = false;
    for (uint8_t i = 0; i < SP_POINTS; i++) {
      uint16_t v = sp_hr10[i];
      if (v == 0) continue;
      any = true;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }

    const int iw = SP_W - 2;
    const int ih = SP_H - 2;

    spSprite.fillSprite(TFT_BLACK);

    if (!any || mn == 65535) {
      spSprite.pushSprite(SP_X + 1, SP_Y + 1);
      return;
    }
    if (mx <= mn) mx = mn + 1;

    int prevX = -1, prevY = -1;

    for (uint8_t p = 0; p < SP_POINTS; p++) {
      uint8_t idx = (sp_head + p) % SP_POINTS; // oldest first
      uint16_t v = sp_hr10[idx];
      if (v == 0) continue;

      int x = (int)((uint32_t)p * (uint32_t)(iw - 1) / (SP_POINTS - 1));

      uint32_t num = (uint32_t)(v - mn) * (uint32_t)(ih - 1);
      uint32_t den = (uint32_t)(mx - mn);
      int y = (ih - 1) - (int)(num / den);

      if (x < 0) x = 0; else if (x > iw - 1) x = iw - 1;
      if (y < 0) y = 0; else if (y > ih - 1) y = ih - 1;

      if (prevX >= 0) spSprite.drawLine(prevX, prevY, x, y, GOLD_COL);
      prevX = x;
      prevY = y;
    }

    spSprite.pushSprite(SP_X + 1, SP_Y + 1);
  }
  // ================= END HR SPARKLINE =================

  void cyd_init() {
    #if defined(ESP32)
      if (tftMutex) xSemaphoreTake(tftMutex, portMAX_DELAY);
    #endif

    if (!bootEverShown) {
      pinMode(TFT_BL, OUTPUT);
      digitalWrite(TFT_BL, !TFT_BACKLIGHT_ON);
      delay(200);

      tft.init();

      tft.writecommand(0x01);
      delay(150);
      tft.writecommand(0x11);
      delay(150);
      tft.writecommand(0x29);
      delay(50);

      tft.startWrite();
      tft.fillScreen(TFT_BLACK); delay(20);
      tft.fillScreen(TFT_BLACK); delay(20);
      tft.fillScreen(TFT_BLACK); delay(20);

      tft.setRotation(3);
      tft.writecommand(0x36);
      tft.writedata(0x60);

      tft.endWrite();

      digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
    }

    // Transition: clear below header only
    tft.startWrite();
    tft.fillRect(0, HEADER_H, tft.width(), tft.height() - HEADER_H, TFT_BLACK);

    drawHeaderStatic();

    int rssi = WiFi.isConnected() ? WiFi.RSSI() : -100;
    int bars = rssiToBars(rssi);
    drawWifiBarsHeader(bars);
    lastHeaderBars = bars;

    wallet_fetch_now();
    walletDirty = true;

    tft.setTextFont(1);
    tft.setTextSize(2);

    // Bottom boxes (EXACT placement kept)
    wal_bx = tft.width()  - WAL_W - WAL_PAD;
    wal_by = tft.height() - WAL_H - WAL_PAD;
    tft.fillRect(wal_bx + 1, wal_by + 1, WAL_W - 2, WAL_H - 2, TFT_BLACK);
    tft.drawRect(wal_bx, wal_by, WAL_W, WAL_H, TFT_DARKGREY);

    time_bx = WAL_PAD;
    time_by = tft.height() - WAL_H - WAL_PAD;
    tft.fillRect(time_bx + 1, time_by + 1, WAL_W - 2, WAL_H - 2, TFT_BLACK);
    tft.drawRect(time_bx, time_by, WAL_W, WAL_H, TFT_DARKGREY);

    // Own the gap between bottom boxes
    cyd_clear_bottom_gap();

    // Compute safe layout ABOVE bottom boxes (stats stack + sparkline)
    const int bottomTop = time_by - 8;
    layout_compute(bottomTop);

    // Draw static labels once
    stkDirty = true;
    last_acceptStreak = 65535;
    prev_total_for_stk = share_count;
    prev_ok_for_stk    = accepted_share_count;
    acceptStreak       = 0;

    tft.setTextColor(LABEL_TOP_COL, TFT_BLACK);
    drawBoldLabel(X_LABEL, Y_HR,   "HR:");
    drawBoldLabel(X_LABEL, Y_QUAL, "Qual:");
    drawBoldLabel(X_LABEL, Y_PING, "Ping:");

    tft.setTextColor(LABEL_BOT_COL, TFT_BLACK);
    drawBoldLabel(X_LABEL, Y_UP,   "Up:");
    drawBoldLabel(X_LABEL, Y_DIFF, "Diff:");

    // Separator line (static)
    tft.drawFastHLine(X_LABEL, Y_SEP, tft.width() - (X_LABEL * 2), TFT_DARKGREY);

    // Sparkline: directly under HR, spanning from X_LABEL
    SP_X = X_LABEL;
    SP_W = tft.width() - SP_X - 8;
    SP_H = UI_SP_H;
    SP_Y = Y_SP;

    sparkline_reset();
    sparkline_draw_frame();

    sparkline_sprite_ensure();
    sp_dirty = true;
    sparkline_draw_if_dirty();

    // Time init
    timeDirty = true;
    time_init_ntp();
    time_tick();
    timeDirty = true;

    tft.endWrite();

    // IMPORTANT: panel guard must be OUTSIDE startWrite/endWrite
    cyd_panel_guard(true);

    #if defined(ESP32)
      if (tftMutex) xSemaphoreGive(tftMutex);
    #endif
  }

  void cyd_draw_values() {
    static float lastQ = -1.0f;

    float hr_khs   = (hashrate + hashrate_core_two) / 1000.0f;
    uint32_t ok    = accepted_share_count;
    uint32_t total = share_count;

    uint16_t newStreak = acceptStreak;

    if (total >= prev_total_for_stk && ok >= prev_ok_for_stk) {
      uint32_t d_total = total - prev_total_for_stk;
      uint32_t d_ok    = ok    - prev_ok_for_stk;

      if (d_total > 0) {
        if (d_ok < d_total) newStreak = 0;
        newStreak = (uint16_t)min<uint32_t>(65535, (uint32_t)newStreak + d_ok);
      }
    } else {
      newStreak = 0;
    }

    prev_total_for_stk = total;
    prev_ok_for_stk    = ok;

    if (newStreak != acceptStreak) acceptStreak = newStreak;
    if (acceptStreak != last_acceptStreak) {
      last_acceptStreak = acceptStreak;
      stkDirty = true;
    }

    rolling_quality_update(total, ok);
    float quality = rolling_quality_percent();

    uint32_t diff  = difficulty / 100;
    int rssi       = WiFi.isConnected() ? WiFi.RSSI() : -100;
    uint32_t up    = millis() / 1000;

    #if defined(ESP32)
      if (!tftMutex) return;
      if (xSemaphoreTake(tftMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    #endif

    // IMPORTANT: panel guard must be OUTSIDE startWrite/endWrite
    cyd_panel_guard(false);

    tft.startWrite();

    int bars = rssiToBars(rssi);
    if (bars != lastHeaderBars) {
      drawWifiBarsHeader(bars);
      lastHeaderBars = bars;
    }

    const int valuePad = (tft.width() - X_VALUE) - 6;
    const int qualPad  = 78;

    tft.setTextFont(1);
    tft.setTextSize(2);

    auto line = [&](int yy) { tft.setCursor(X_VALUE, yy); };

    // HR
    line(Y_HR);
    tft.setTextPadding(valuePad);
    tft.setTextColor(hrColor(hr_khs), TFT_BLACK);
    tft.printf("%7.2f kH/s", hr_khs);

    // Qual
    line(Y_QUAL);
    tft.setTextPadding(qualPad);
    tft.setTextColor(qualityColor(quality), TFT_BLACK);
    tft.printf("%5.1f%%", quality);

    const int arrowX = X_VALUE + 74;
    const int arrowY = Y_QUAL - 2;
    tft.fillRect(arrowX, arrowY, 15, 15, TFT_BLACK);

    int newTrend = 0;
    if (lastQ >= 0.0f) {
      float dQ = quality - lastQ;
      if (dQ > 0.3f)       newTrend = 1;
      else if (dQ < -0.3f) newTrend = -1;
    }
    lastQ = quality;

    uint16_t arrowCol =
      (newTrend > 0) ? 0x07E0 :
      (newTrend < 0) ? 0xF800 :
                       TFT_DARKGREY;

    drawTrendArrow(arrowX, arrowY, newTrend, arrowCol);

    const int stkX = arrowX + 18;
    const int stkY = Y_QUAL + 4;

    if (stkDirty) {
      stkDirty = false;
      tft.fillRect(stkX, Y_QUAL, 60, 16, TFT_BLACK);

      tft.setTextSize(1);
      tft.setTextPadding(0);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(stkX, stkY);
      tft.printf("Stk:%u", acceptStreak);

      tft.setTextSize(2);
    }

    // Ping
    line(Y_PING);
    tft.setTextPadding(valuePad);
    tft.setTextColor(pingColor((uint32_t)ping), TFT_BLACK);
    tft.printf("%4u ms", (uint32_t)ping);

    // Separator line
    tft.drawFastHLine(X_LABEL, Y_SEP, tft.width() - (X_LABEL * 2), TFT_DARKGREY);

    // Up
    line(Y_UP);
    tft.setTextPadding(valuePad);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

    uint32_t s = up;
    uint32_t d = s / 86400UL; s %= 86400UL;
    uint32_t h = s / 3600UL;  s %= 3600UL;
    uint32_t m = s / 60UL;    s %= 60UL;

    char ubuf[32];
    snprintf(ubuf, sizeof(ubuf), "%luD %02luH %02luM %02luS", d, h, m, s);
    tft.print(ubuf);

    // Diff
    line(Y_DIFF);
    tft.setTextPadding(valuePad);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.printf("%lu", diff);

    tft.setTextPadding(0);

    time_draw_if_dirty();

    if (walletDirty) {
      walletDirty = false;

      tft.fillRect(wal_bx + 1, wal_by + 1, WAL_W - 2, WAL_H - 2, TFT_BLACK);

      tft.setTextFont(1);
      tft.setTextSize(1);

      const int innerPad = WAL_W - 14;

      tft.setTextPadding(innerPad);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(wal_bx + 7, wal_by + 4);
      tft.print(wallet_user[0] ? wallet_user : "user");

      tft.setTextPadding(0);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(wal_bx + 7, wal_by + 18);
      tft.print("Bal:");

      tft.setTextColor(walletBalColor(wallet_bal), TFT_BLACK);
      tft.print(wallet_bal[0] ? wallet_bal : "?");

      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(wal_bx + 80, wal_by + 18);
      tft.print("W:");
      tft.print(wallet_workers);

      uint16_t dot = walletOk ? 0x07E0 : 0xF800;
      tft.fillCircle(wal_bx + WAL_W - 8, wal_by + 8, 3, dot);

      tft.setTextPadding(0);
    }

    // Keep the gap between bottom boxes black
    cyd_clear_bottom_gap();

    // Re-outline bottom boxes (thin)
    tft.drawRect(time_bx, time_by, WAL_W, WAL_H, TFT_DARKGREY);
    tft.drawRect(wal_bx,  wal_by,  WAL_W, WAL_H, TFT_DARKGREY);

    tft.endWrite();

    #if defined(ESP32)
      xSemaphoreGive(tftMutex);
    #endif
  }

  void cyd_tick_1hz() {
    if (millis() - lastCydMs >= 1000) {
      lastCydMs = millis();
      time_tick();
      wallet_tick();
      cyd_draw_values();
    }
  }

  // fast UI tick for sparkline sampling + redraw (500ms sampling)
  void cyd_tick_fast_250ms() {
    uint32_t now = millis();
    if (now - lastSparkSampleMs < 500) return;
    lastSparkSampleMs = now;

    float hr_khs = (hashrate + hashrate_core_two) / 1000.0f;
    if (hr_khs < 0) hr_khs = 0;

    uint16_t hr10 = (uint16_t)(hr_khs * 10.0f + 0.5f);
    sparkline_push(hr10);

    #if defined(ESP32)
      if (!tftMutex) return;
      if (xSemaphoreTake(tftMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    #endif

    sparkline_draw_if_dirty();

    #if defined(ESP32)
      xSemaphoreGive(tftMutex);
    #endif
  }
#endif // DISPLAY_CYD

// ================= Mining tasks =================
MiningJob *job[CORE];

#if CORE == 2
  EasyFreeRTOS32 task1, task2;
#endif

void task1_func(void *) {
  #if defined(ESP32) && CORE == 2
    VOID SETUP() { }
    VOID LOOP() { job[0]->mine(); }
  #endif
}

void task2_func(void *) {
  #if defined(ESP32) && CORE == 2
    VOID SETUP() { job[1] = new MiningJob(1, configuration); }
    VOID LOOP() { job[1]->mine(); }
  #endif
}

void system_events_func(void* parameter);

void setup() {
  #if defined(ESP32)
    tftMutex = xSemaphoreCreateMutex();
  #endif

  #if !defined(ESP8266) && defined(DISABLE_BROWNOUT)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  #endif

  #if defined(SERIAL_PRINTING)
    Serial.begin(SERIAL_BAUDRATE);
    Serial.println("\n\nDuino-Coin " + String(configuration->MINER_VER));
    #if SHOWPIECE_MODE
      Serial.println("ChocDuino: SHOWPIECE MODE (no restart-on-timeout)");
    #endif
  #endif

  pinMode(LED_BUILTIN, OUTPUT);

  // show boot screen immediately (before WiFi/node)
  #if defined(DISPLAY_CYD)
    boot_begin();
  #endif

  #if defined(DISPLAY_SSD1306) || defined(DISPLAY_16X2)
    screen_setup();
    display_boot();
    delay(2500);
  #endif

  assert(CORE == 1 || CORE == 2);
  WALLET_ID = String(random(0, 2811));
  job[0] = new MiningJob(0, configuration);

  WiFi.mode(WIFI_STA);

  // Stability defaults
  #if !defined(ESP8266)
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
  #endif

  #if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  #else
    WiFi.setSleep(false);
  #endif

  #if defined(CAPTIVE_PORTAL)
    preferences.begin("duino_config", false);
    strcpy(duco_username, preferences.getString("duco_username", "username").c_str());
    strcpy(duco_password, preferences.getString("duco_password", "None").c_str());
    strcpy(duco_rigid, preferences.getString("duco_rigid", "None").c_str());
    preferences.end();

    configuration->DUCO_USER = duco_username;
    configuration->RIG_IDENTIFIER = duco_rigid;
    configuration->MINER_KEY = duco_password;
    RIG_IDENTIFIER = duco_rigid;

    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.addParameter(&custom_duco_username);
    wifiManager.addParameter(&custom_duco_password);
    wifiManager.addParameter(&custom_duco_rigid);

    wifiManager.autoConnect("Duino-Coin");
    delay(1000);
    VerifyWifi();
    SelectNode();
  #else
    SetupWifi();
  #endif

  // Boot is allowed to end even if poolpicker didn't respond yet
  g_boot_done = true;

  #if defined(DISPLAY_CYD)
    boot_end_prepare_main_ui();
  #endif

  SetupOTA();

  #if defined(DISPLAY_CYD)
    cyd_init();
  #endif

  #if defined(WEB_DASHBOARD)
    if (!MDNS.begin(RIG_IDENTIFIER)) {
      #if defined(SERIAL_PRINTING)
        Serial.println("mDNS unavailable");
      #endif
    }
    MDNS.addService("http", "tcp", 80);
    server.on("/", dashboard);
    #if defined(CAPTIVE_PORTAL)
      server.on("/reset", reset_settings);
    #endif
    server.begin();
  #endif

  #if defined(ESP8266)
    lwdtFeed();
    lwdTimer.attach_ms(LWD_TIMEOUT, lwdtcb);
  #else
    setCpuFrequencyMhz(240);
  #endif

  job[0]->blink(BLINK_SETUP_COMPLETE);

  #if defined(ESP32) && CORE == 2
    mutexClientData = xSemaphoreCreateMutex();
    mutexConnectToServer = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(system_events_func, "system_events_func", 10000, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(task1_func, "task1_func", 10000, NULL, 1, &Task1, 0);
    xTaskCreatePinnedToCore(task2_func, "task2_func", 10000, NULL, 1, &Task2, 1);
  #endif
}

static inline void stability_supervisor_tick() {
  uint32_t now = millis();

  // WiFi heal every ~3s
  if (now - g_last_wifi_kick_ms > 3000UL) {
    g_last_wifi_kick_ms = now;
    VerifyWifi();
  }

  // If node isn't ready yet, retry poolpicker every ~15s (once boot is done)
  if (g_boot_done && !g_node_ready && WiFi.isConnected()) {
    if (now - g_last_node_try_ms > 15000UL) {
      g_last_node_try_ms = now;
      SelectNode();
    }
  }
}

void system_events_func(void* parameter) {
  while (true) {
    delay(10);

    #if defined(DISPLAY_CYD)
      cyd_tick_1hz();
      cyd_tick_fast_250ms();
    #endif

    #if defined(WEB_DASHBOARD)
      server.handleClient();
    #endif

    ArduinoOTA.handle();

    stability_supervisor_tick();
  }
}

void single_core_loop() {
  job[0]->mine();
  lwdtFeed();
  VerifyWifi();
  ArduinoOTA.handle();

  #if defined(WEB_DASHBOARD)
    server.handleClient();
  #endif

  stability_supervisor_tick();
}

void loop() {
  #if defined(ESP8266) || defined(CONFIG_FREERTOS_UNICORE)
    single_core_loop();
  #endif
  delay(10);
}
