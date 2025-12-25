# ChocDuino v0.9 — Duino-Coin ESP32 CYD Miner

ChocDuino is a Duino-Coin ESP32 miner with a custom **CYD (ESP32-2432S028)** TFT UI, an animated boot screen, and stability hardening for long-running operation.

This repository includes **all required Duino-Coin ESP miner source files** plus the ChocDuino CYD UI fork.  
No external Duino-Coin repository is required.

> **Credits / Upstream Project**  
> Duino-Coin Project  
> © The Duino-Coin Team & Community — MIT Licensed  
> https://duinocoin.com  
> https://github.com/revoxhere/duino-coin  

---

## Hardware Target

- **ESP32-2432S028** (“CYD”)
- 2.8" TFT display (ILI9341)
- Tested with **ESP32 Arduino Core 3.3.4**

---

## Configuration

Only **`Settings.h`** needs to be edited.

Typical configuration values:
- WiFi SSID and password
- Duino-Coin username
- Mining key (if enabled in your wallet)
- Optional rig identifier

---

## Dependencies

### Required Arduino Libraries

Install via **Arduino Library Manager**:

- **ArduinoJson**
- **TFT_eSPI**
- **ArduinoOTA**
- **Ticker**

Included with the ESP32 core:
- WiFi
- HTTPClient
- WiFiClientSecure
- ESPmDNS

---

## TFT_eSPI Setup (CYD — Known Working)

TFT_eSPI **must be configured inside the library**, not inside this project.

### Select the CYD Setup

Open the following file:

Arduino/libraries/TFT_eSPI/User_Setup_Select.h

arduino
Copy code

Ensure **only this line is enabled**:

```cpp
#include <User_Setups/Setup_CYD_ESP32_2432S028_ILI9341.h>
All other #include <User_Setups/...> lines must remain commented out.

CYD User Setup Reference
The active setup file (Setup_CYD_ESP32_2432S028_ILI9341.h) should match:

cpp
Copy code
#define USER_SETUP_INFO "CYD ESP32-2432S028 ILI9341"

#define ILI9341_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

#define TFT_MISO -1   // REQUIRED: fixes CYD SPI glitching

#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

#define SPI_FREQUENCY 16000000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
Build Environment
Arduino IDE 2.x

ESP32 by Espressif — version 3.3.4

Board: ESP32 Dev Module

Flash Size: 4MB

Partition Scheme: any scheme that fits

PSRAM: enable only if supported by your board

Build & Flash
Open the project in Arduino IDE.

Configure TFT_eSPI as described above.

Edit Settings.h.

Compile and upload to the ESP32-2432S028.

Notes
Screen rotation is handled in code.

Touch is not used.

CYD boards are sensitive to power quality.

License
Duino-Coin project code: MIT License

ChocDuino UI modifications: MIT-compatible

Please retain attribution when redistributing.