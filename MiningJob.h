#pragma GCC optimize("-Ofast")

#ifndef MINING_JOB_H
#define MINING_JOB_H

#include <Arduino.h>
#include <string.h>
#include <Ticker.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#include <WiFiClient.h>

#include "DSHA1.h"
#include "Counter.h"
#include "Settings.h"

// https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TypeConversion.cpp
const char base36Chars[36] PROGMEM = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'
};

const uint8_t base36CharValues[75] PROGMEM{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0, 0, 0, 0, 0, 0,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35
};

#define SPC_TOKEN ' '
#define END_TOKEN '\n'
#define SEP_TOKEN ','
#define IOT_TOKEN '@'

struct MiningConfig {
    String host = "";
    int port = 0;
    String DUCO_USER = "";
    String RIG_IDENTIFIER = "";
    String MINER_KEY = "";
    String MINER_VER = SOFTWARE_VERSION;

    #if defined(ESP8266)
        String START_DIFF = "ESP8266H";
    #elif defined(CONFIG_FREERTOS_UNICORE)
        String START_DIFF = "ESP32S";
    #else
        String START_DIFF = "ESP32";
    #endif

    MiningConfig(String DUCO_USER, String RIG_IDENTIFIER, String MINER_KEY)
            : DUCO_USER(DUCO_USER), RIG_IDENTIFIER(RIG_IDENTIFIER), MINER_KEY(MINER_KEY) {}
};

class MiningJob {
public:
    MiningConfig *config;
    int core = 0;

    MiningJob(int core, MiningConfig *config) {
        this->core = core;
        this->config = config;
        this->client_buffer = "";
        dsha1 = new DSHA1();
        dsha1->warmup();
        generateRigIdentifier();
    }

    void blink(uint8_t count, uint8_t pin = LED_BUILTIN) {
        #if defined(LED_BLINKING)
            uint8_t state = HIGH;
            for (int x = 0; x < (count << 1); ++x) {
                digitalWrite(pin, state ^= HIGH);
                delay(50);
            }
        #else
            digitalWrite(LED_BUILTIN, HIGH);
        #endif
    }

    bool max_micros_elapsed(unsigned long current, unsigned long max_elapsed) {
        static unsigned long _start = 0;
        if ((current - _start) > max_elapsed) {
            _start = current;
            return true;
        }
        return false;
    }

    void handleSystemEvents(void) {
        delay(10);
        yield();
        ArduinoOTA.handle();
    }

    void mine() {
        // Heal WiFi first (prevents "stuck disconnected for 30 mins")
        if (!ensureWiFi(15000UL)) {
            client.stop();
            return;
        }

        if (!connectToNode()) return;
        if (!askForJob())     return;

        // Safety: if we haven’t had a successful submit in 5 minutes, reconnect.
        uint32_t now = millis();
        if (last_submit_ms && (now - last_submit_ms) > 300000UL) {
            #if defined(SERIAL_PRINTING)
              Serial.printf("Core [%d] - NET: no submit in 5m, reconnect\n", core);
            #endif
            client.stop();
            return;
        }

        dsha1->reset().write((const unsigned char *)getLastBlockHash().c_str(), getLastBlockHash().length());

        int start_time = micros();
        max_micros_elapsed(start_time, 0);

        #if defined(LED_BLINKING)
            #if defined(BLUSHYBOX)
              for (int i = 0; i < 72; i++) { analogWrite(LED_BUILTIN, i); delay(1); }
            #else
              digitalWrite(LED_BUILTIN, LOW);
            #endif
        #endif

        for (Counter<10> counter; counter < job_difficulty; ++counter) {
            DSHA1 ctx = *dsha1;
            ctx.write((const unsigned char *)counter.c_str(), counter.strlen()).finalize(hashArray);

            #ifndef CONFIG_FREERTOS_UNICORE
                #if defined(ESP32)
                    #define SYSTEM_TIMEOUT 100000
                #else
                    #define SYSTEM_TIMEOUT 500000
                #endif
                if (max_micros_elapsed(micros(), SYSTEM_TIMEOUT)) {
                    handleSystemEvents();

                    // Abort quickly if network dropped mid-hash loop
                    if (WiFi.status() != WL_CONNECTED || !client.connected()) {
                        #if defined(SERIAL_PRINTING)
                          Serial.printf("Core [%d] - NET: dropped during hash loop, reconnect\n", core);
                        #endif
                        client.stop();
                        return;
                    }
                }
            #endif

            if (memcmp(getExpectedHash(), hashArray, 20) == 0) {
                unsigned long elapsed_time = micros() - start_time;
                float elapsed_time_s = elapsed_time * .000001f;
                share_count++;

                #if defined(LED_BLINKING)
                    #if defined(BLUSHYBOX)
                      for (int i = 72; i > 0; i--) { analogWrite(LED_BUILTIN, i); delay(1); }
                    #else
                      digitalWrite(LED_BUILTIN, HIGH);
                    #endif
                #endif

                if (String(core) == "0") {
                    hashrate = counter / elapsed_time_s;
                    submit(counter, hashrate, elapsed_time_s);
                } else {
                    hashrate_core_two = counter / elapsed_time_s;
                    submit(counter, hashrate_core_two, elapsed_time_s);
                }

                #if defined(BLUSHYBOX)
                    gauge_set(hashrate + hashrate_core_two);
                #endif
                break;
            }
        }
    }

private:
    String client_buffer;
    uint8_t hashArray[20];
    String last_block_hash;
    String expected_hash_str;
    uint8_t expected_hash[20];
    unsigned int job_difficulty = 1;  // DO NOT SHADOW global ::difficulty

    DSHA1 *dsha1;
    WiFiClient client;
    String chipID = "";

    uint32_t last_submit_ms = 0;

    #if defined(ESP8266)
        #if defined(BLUSHYBOX)
          String MINER_BANNER = "Official BlushyBox Miner (ESP8266)";
        #else
          String MINER_BANNER = "Official ESP8266 Miner";
        #endif
    #elif defined(CONFIG_FREERTOS_UNICORE)
        String MINER_BANNER = "Official ESP32-S2 Miner";
    #else
        #if defined(BLUSHYBOX)
          String MINER_BANNER = "Official BlushyBox Miner (ESP32)";
        #else
          String MINER_BANNER = "Official ESP32 Miner";
        #endif
    #endif

    bool ensureWiFi(uint32_t timeoutMs) {
        if (WiFi.status() == WL_CONNECTED) return true;

        #if defined(SERIAL_PRINTING)
          Serial.printf("Core [%d] - WiFi down, reconnecting...\n", core);
        #endif

        WiFi.disconnect(false);
        delay(80);
        WiFi.reconnect();

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (max_micros_elapsed(micros(), 100000)) handleSystemEvents();
            if (millis() - start > timeoutMs) {
                #if defined(SERIAL_PRINTING)
                  Serial.printf("Core [%d] - WiFi reconnect timeout\n", core);
                #endif
                return false;
            }
        }
        return true;
    }

    // Safe hex conversion. Returns false instead of asserting/rebooting.
    bool hexStringToUint8ArraySafe(const String &hexString, uint8_t *uint8Array, const uint32_t arrayLength) {
        if (!uint8Array) return false;

        const uint32_t need = arrayLength * 2;
        if ((uint32_t)hexString.length() < need) {
            #if defined(SERIAL_PRINTING)
              Serial.printf("Core [%d] - JOB: bad expected_hash len=%u need=%u\n",
                            core, (unsigned)hexString.length(), (unsigned)need);
            #endif
            return false;
        }

        const char *hexChars = hexString.c_str();
        for (uint32_t i = 0; i < arrayLength; ++i) {
            const char c1 = hexChars[i * 2];
            const char c2 = hexChars[i * 2 + 1];

            if (c1 < '0' || c1 > 'z' || c2 < '0' || c2 > 'z') return false;

            const uint8_t hi = pgm_read_byte(base36CharValues + (uint8_t)(c1 - '0'));
            const uint8_t lo = pgm_read_byte(base36CharValues + (uint8_t)(c2 - '0'));

            if (hi > 15 || lo > 15) return false; // reject non-hex

            uint8Array[i] = (hi << 4) | lo;
        }
        return true;
    }

    void generateRigIdentifier() {
        String AutoRigName = "";

        #if defined(ESP8266)
            chipID = String(ESP.getChipId(), HEX);

            if (strcmp(config->RIG_IDENTIFIER.c_str(), "Auto") != 0)
                return;

            AutoRigName = "ESP8266-" + chipID;
            AutoRigName.toUpperCase();
            config->RIG_IDENTIFIER = AutoRigName.c_str();
        #else
            uint64_t chip_id = ESP.getEfuseMac();
            uint16_t chip = (uint16_t)(chip_id >> 32);
            char fullChip[23];
            snprintf(fullChip, 23, "%04X%08X", chip, (uint32_t)chip_id);
            chipID = String(fullChip);

            if (strcmp(config->RIG_IDENTIFIER.c_str(), "Auto") != 0)
                return;

            AutoRigName = "ESP32-" + String(fullChip);
            AutoRigName.toUpperCase();
            config->RIG_IDENTIFIER = AutoRigName.c_str();
        #endif

        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - Rig identifier: " + config->RIG_IDENTIFIER);
        #endif
    }

    bool connectToNode() {
        if (client.connected()) return true;

        unsigned long startMs = millis();

        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - Connecting to a Duino-Coin node...");
        #endif

        while (!client.connect(config->host.c_str(), config->port)) {
            if (max_micros_elapsed(micros(), 100000)) handleSystemEvents();

            if (millis() - startMs > 30000UL) {
                #if defined(SERIAL_PRINTING)
                  Serial.println("Core [" + String(core) + "] - Connect timeout (no restart)");
                #endif
                client.stop();
                return false;
            }
        }

        if (!waitForClientData(8000)) {
            client.stop();
            return false;
        }

        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - Connected. Node reported version: " + client_buffer);
        #endif

        blink(BLINK_CLIENT_CONNECT);
        return true;
    }

    bool waitForClientData(uint32_t timeoutMs) {
        client_buffer = "";
        unsigned long startMs = millis();

        while (client.connected()) {
            if (client.available()) {
                client_buffer = client.readStringUntil(END_TOKEN);
                client_buffer.replace("\r", "");
                return true;
            }

            if (max_micros_elapsed(micros(), 100000)) handleSystemEvents();

            if (millis() - startMs > timeoutMs) {
                #if defined(SERIAL_PRINTING)
                  Serial.printf("Core [%d] - waitForClientData timeout (%lu ms)\n", core, (unsigned long)timeoutMs);
                #endif
                return false;
            }
        }

        return false;
    }

    void submit(unsigned long counter, float hashrate, float elapsed_time_s) {
        if (!client.connected()) return;

        client.print(String(counter) +
                     SEP_TOKEN + String(hashrate) +
                     SEP_TOKEN + MINER_BANNER +
                     SPC_TOKEN + config->MINER_VER +
                     SEP_TOKEN + config->RIG_IDENTIFIER +
                     SEP_TOKEN + "DUCOID" + String(chipID) +
                     SEP_TOKEN + String(WALLET_ID) +
                     END_TOKEN);

        unsigned long ping_start = millis();
        if (!waitForClientData(8000)) {
            client.stop();
            return;
        }
        ping = millis() - ping_start;

        last_submit_ms = millis();

        if (client_buffer == "GOOD") accepted_share_count++;

        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - " +
                          client_buffer +
                          " share #" + String(share_count) +
                          " (" + String(counter) + ")" +
                          " hashrate: " + String(hashrate / 1000, 2) + " kH/s (" +
                          String(elapsed_time_s) + "s) " +
                          "Ping: " + String(ping) + "ms " +
                          "(" + node_id + ")\n");
        #endif
    }

    bool parseJobLine() {
        char *job_str_copy = strdup(client_buffer.c_str());
        if (!job_str_copy) return false;

        String tokens[3] = {"", "", ""};
        char *token = strtok(job_str_copy, ",");
        for (int i = 0; token != NULL && i < 3; i++) {
            tokens[i] = token;
            token = strtok(NULL, ",");
        }
        free(job_str_copy);

        if (tokens[0].length() < 8 || tokens[1].length() < 40 || tokens[2].length() < 1) {
            #if defined(SERIAL_PRINTING)
              Serial.printf("Core [%d] - JOB malformed: len0=%u len1=%u len2=%u\n",
                            core, (unsigned)tokens[0].length(), (unsigned)tokens[1].length(), (unsigned)tokens[2].length());
            #endif
            return false;
        }

        last_block_hash   = tokens[0];
        expected_hash_str = tokens[1];

        if (!hexStringToUint8ArraySafe(expected_hash_str, expected_hash, 20)) {
            return false;
        }

        int diff = tokens[2].toInt();
        if (diff <= 0) return false;

        job_difficulty = (unsigned int)diff * 100 + 1;
        ::difficulty   = job_difficulty; // update GLOBAL difficulty so UI shows Diff

        return true;
    }

    bool askForJob() {
        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - Asking for a new job for user: " + String(config->DUCO_USER));
        #endif

        client.print("JOB," +
                     String(config->DUCO_USER) +
                     SEP_TOKEN + config->START_DIFF +
                     SEP_TOKEN + String(config->MINER_KEY) +
                     END_TOKEN);

        if (!waitForClientData(12000)) {
            client.stop();
            return false;
        }

        #if defined(SERIAL_PRINTING)
          Serial.println("Core [" + String(core) + "] - Received job (" + String(client_buffer.length()) + " bytes)");
        #endif

        if (!parseJobLine()) {
            #if defined(SERIAL_PRINTING)
              Serial.println("Core [" + String(core) + "] - JOB: discard malformed job, reconnecting...");
            #endif
            client.stop();
            return false;
        }

        return true;
    }

    const String &getLastBlockHash() const { return last_block_hash; }
    const String &getExpectedHashStr() const { return expected_hash_str; }
    const uint8_t *getExpectedHash() const { return expected_hash; }
    unsigned int getDifficulty() const { return job_difficulty; }
};

#endif
