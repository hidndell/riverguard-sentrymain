#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

// ============================================================================
// RIVERGUARD MAIN GATEWAY NODE — VERSION Fin05
// Changes from Fin04:
//   - Sends a one-way Registration Confirmation SMS via SIM800L whenever a
//     new mobile number registers on the website (/api/get_numbers?with_ids=1).
//   - Uses a Cloud Database ID Watermark (lastWelcomeId in ESP32 NVS Flash):
//       * On first sync after flashing, silently baselines pre-existing TiDB
//         registrations so numbers registered before Fin05 are NOT spammed.
//       * Any new registration (id > lastWelcomeId) automatically receives the
//         confirmation SMS (even if registered while Main Node was powered off,
//         or if deleted by Admin and re-registered later).
//   - Retains Fin04 non-blocking 3x Blue LED (Pin D2) RX blink indicator,
//     full SIM800L AT/CMGS handshake, offline NVS cache + fallback number,
//     and multi-node cloud telemetry upload.
// ============================================================================

// ============================================================================
// >>> WI-FI, OFFLINE FALLBACK SIM & RENDER CLOUD ENDPOINTS <<<
// ============================================================================
const char* ssid                = "realme 9 Pro 5G";
const char* password            = "12345678";

// Built-in Offline Fallback Emergency Number (works even with ZERO internet/Wi-Fi).
// Replace "+639123456789" with your real responder SIM number (placeholder is skipped until changed):
const char* fallbackPhoneNumber = "+639123456789";

// Update the domain below if Render assigns a different subdomain on deployment:
const char* uploadURL           = "https://riverguard-sentrymain.onrender.com/api/upload";
const char* numbersURL          = "https://riverguard-sentrymain.onrender.com/api/get_numbers";

// Must match GATEWAY_API_KEY in Render Environment Variables (app.py default: rg-gateway-2026-key)
const char* gatewayApiKey       = "rg-gateway-2026-key";

// Confirmation SMS message sent once to each newly registered mobile number:
const char* welcomeSmsMessage   = "RIVERGUARD: Your mobile number is now registered to receive automated flood SMS alerts.";

// --- HARDWARE PIN ASSIGNMENTS (MAIN GATEWAY NODE) ---
const int blueLED         = 2;  // Onboard Blue LED (Sensor RX Blink Indicator & SOS)
const int loraDio1Pin     = 4;  // SX1262 DIO1 Interrupt Pin on Main Node
const int momentaryBtnPin = 15; // Momentary UI Button (D15 -> GND)
const int strobeRelay     = 26; // 12V Warning Strobe Relay (Active LOW)
const int sirenRelay      = 27; // 12V Critical Siren Relay (Active LOW)
const int simRxPin        = 16; // ESP32 RX2 -> SIM800L TX
const int simTxPin        = 17; // ESP32 TX2 -> SIM800L RX

// --- SX1262 915MHz WIRING (MAIN NODE: NSS=5, DIO1=4, RST=14, BUSY=Ignored) ---
SX1262 radio = new Module(5, loraDio1Pin, 14, RADIOLIB_NC);
volatile bool receivedFlag = false;

#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setRadioFlag(void) {
  receivedFlag = true;
}

// --- OLED DISPLAY & GSM SERIAL ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
HardwareSerial sim800(2);

// --- WATCHDOG COUNTER ---
RTC_DATA_ATTR int bootFailureCount = 0;

// --- DYNAMIC NODE TRACKING & 5-READING HISTORY STRUCTURES ---
const int MAX_NODES = 10; // Supports Node IDs 1 through 9
const unsigned long OFFLINE_TIMEOUT_MS = 300000UL; // 5 minutes (300,000 ms)

struct ReadingRecord {
  long distance;
  int batteryPct;
  float rssi;
  float snr;
  unsigned long timestamp;
};

struct NodeState {
  bool heard;                     // True once at least 1 packet is received
  int lastMsgID;                  // Duplicate packet filter
  long latestDistance;            // Most recent distance (cm)
  int latestBattery;              // Most recent battery (%)
  float latestRSSI;               // Most recent RSSI (dBm)
  float latestSNR;                // Most recent SNR (dB)
  unsigned long lastSeenMillis;   // millis() when last packet arrived
  int historyCount;               // Number of stored readings (up to 5)
  ReadingRecord history[5];       // Rolling history: index 0 = newest, 4 = oldest
};

NodeState nodes[MAX_NODES];
portMUX_TYPE nodeMux = portMUX_INITIALIZER_UNLOCKED;

// --- GLOBAL THREAT, RELAY & SMS TIMERS ---
Preferences prefs;
SemaphoreHandle_t nvsMutex           = NULL;
volatile int globalMode              = 1; // 1 = SAFE, 2 = WARNING, 3 = CRITICAL
String dynamicPhoneNumbers           = ""; // Website-registered numbers (cached in RAM + NVS Flash)
volatile bool newNumberRegistered    = false;
unsigned long lastWarnSmsTime        = 0;
unsigned long lastCritSmsTime        = 0;
unsigned long lastEmptySmsCheck      = 0;
int smsRetryCount                    = 0;
bool firstWarnSmsSent                = false;
bool firstCritSmsSent                = false;
bool inSafeState                     = true;
unsigned long safeSinceMillis        = 0;
unsigned long lastStrobeToggle       = 0;
bool strobeActiveState               = false;

// --- NEW REGISTRATION CONFIRMATION SMS WATERMARK & QUEUE ---
struct WelcomeItem {
  long recordId;
  char phone[20];
  int attempts;
};

const int MAX_WELCOME_QUEUE          = 16;
WelcomeItem welcomeQueue[MAX_WELCOME_QUEUE];
int welcomeQueueCount                = 0;
long lastWelcomeId                   = -1; // -1 = uninitialized (baselines existing DB IDs on first sync)
long highestQueuedId                 = -1;
unsigned long lastWelcomeTryMillis   = 0;

// --- THREAD-SAFE NVS FLASH HELPERS ---
void saveSmsListToNvs(const String &listStr) {
  if (nvsMutex != NULL) xSemaphoreTake(nvsMutex, portMAX_DELAY);
  prefs.begin("riverguard", false);
  prefs.putString("sms_list", listStr);
  prefs.end();
  if (nvsMutex != NULL) xSemaphoreGive(nvsMutex);
}

void saveWelcomeWatermarkToNvs(long watermarkId) {
  if (nvsMutex != NULL) xSemaphoreTake(nvsMutex, portMAX_DELAY);
  prefs.begin("riverguard", false);
  prefs.putLong("welcome_id", watermarkId);
  prefs.end();
  if (nvsMutex != NULL) xSemaphoreGive(nvsMutex);
}

// --- NON-BLOCKING BLUE LED (PIN D2) RX BLINK STATE MACHINE ---
// Blinks 3 times clearly whenever data arrives from any Soldier/Sensor Node
enum RxLedState {
  RX_LED_IDLE,
  RX_LED_ON1,
  RX_LED_OFF1,
  RX_LED_ON2,
  RX_LED_OFF2,
  RX_LED_ON3
};
RxLedState rxLedState      = RX_LED_IDLE;
unsigned long rxLedTimer   = 0;

void triggerRxBlueLed() {
  digitalWrite(blueLED, HIGH);
  rxLedState = RX_LED_ON1;
  rxLedTimer = millis();
}

void updateRxBlueLed(unsigned long now) {
  switch (rxLedState) {
    case RX_LED_IDLE:
      break;
    case RX_LED_ON1:
      if (now - rxLedTimer >= 120UL) {
        digitalWrite(blueLED, LOW);
        rxLedState = RX_LED_OFF1;
        rxLedTimer = now;
      }
      break;
    case RX_LED_OFF1:
      if (now - rxLedTimer >= 100UL) {
        digitalWrite(blueLED, HIGH);
        rxLedState = RX_LED_ON2;
        rxLedTimer = now;
      }
      break;
    case RX_LED_ON2:
      if (now - rxLedTimer >= 120UL) {
        digitalWrite(blueLED, LOW);
        rxLedState = RX_LED_OFF2;
        rxLedTimer = now;
      }
      break;
    case RX_LED_OFF2:
      if (now - rxLedTimer >= 100UL) {
        digitalWrite(blueLED, HIGH);
        rxLedState = RX_LED_ON3;
        rxLedTimer = now;
      }
      break;
    case RX_LED_ON3:
      if (now - rxLedTimer >= 120UL) {
        digitalWrite(blueLED, LOW);
        rxLedState = RX_LED_IDLE;
      }
      break;
  }
}

// --- UI & MOMENTARY BUTTON STATE MACHINE ---
enum UiDisplayMode {
  UI_CAROUSEL,
  UI_DETAILED
};

UiDisplayMode currentUiMode      = UI_CAROUSEL;
int carouselCursor               = 0;
int detailNodeID                 = 1;
int detailHistoryIdx             = 0;
unsigned long lastCarouselStep   = 0;
unsigned long lastUiRefresh      = 0;
unsigned long lastBtnActivity    = 0;
const unsigned long DETAIL_TIMEOUT_MS = 15000UL; // 15 seconds inactivity return

bool btnLastReading              = HIGH;
bool btnStableState              = HIGH;
unsigned long btnLastDebounce    = 0;
unsigned long btnPressStartTime  = 0;
bool btnLongPressFired           = false;

// --- WATCHDOG SOS MORSE CODE ---
void playSOS() {
  pinMode(blueLED, OUTPUT);
  while (true) {
    for (int i = 0; i < 3; i++) { digitalWrite(blueLED, HIGH); delay(200); digitalWrite(blueLED, LOW); delay(200); }
    delay(200);
    for (int i = 0; i < 3; i++) { digitalWrite(blueLED, HIGH); delay(600); digitalWrite(blueLED, LOW); delay(200); }
    delay(200);
    for (int i = 0; i < 3; i++) { digitalWrite(blueLED, HIGH); delay(200); digitalWrite(blueLED, LOW); delay(200); }
    delay(2000);
  }
}

// --- HELPER: FORMAT DURATION AS "Xm Ys" OR "Xh Ym" ---
String formatDuration(unsigned long durationMs) {
  unsigned long totalSec = durationMs / 1000UL;
  unsigned long mins     = totalSec / 60UL;
  unsigned long secs     = totalSec % 60UL;
  if (mins >= 60UL) {
    unsigned long hrs  = mins / 60UL;
    unsigned long rMin = mins % 60UL;
    return String(hrs) + "h " + String(rMin) + "m";
  }
  return String(mins) + "m " + String(secs) + "s";
}

// --- HELPER: CHECK IF NODE IS OFFLINE (>= 5 MINUTES SILENT) ---
bool isNodeOffline(int nodeId, unsigned long now) {
  if (!nodes[nodeId].heard) return true;
  return (now - nodes[nodeId].lastSeenMillis >= OFFLINE_TIMEOUT_MS);
}

// --- HELPER: BUILD LIST OF DISCOVERED NODES ---
int getDiscoveredNodes(int *outList) {
  int count = 0;
  for (int i = 1; i < MAX_NODES; i++) {
    if (nodes[i].heard) {
      outList[count++] = i;
    }
  }
  return count;
}

// --- HELPER: FIND NEXT ONLINE (OR DISCOVERED) NODE FOR LONG-PRESS ---
int getNextDetailNode(int currentId, unsigned long now) {
  for (int offset = 1; offset < MAX_NODES; offset++) {
    int candidate = ((currentId - 1 + offset) % (MAX_NODES - 1)) + 1;
    if (nodes[candidate].heard && !isNodeOffline(candidate, now)) {
      return candidate;
    }
  }
  for (int offset = 1; offset < MAX_NODES; offset++) {
    int candidate = ((currentId - 1 + offset) % (MAX_NODES - 1)) + 1;
    if (nodes[candidate].heard) {
      return candidate;
    }
  }
  return 0;
}

// --- EVALUATE GLOBAL THREAT LEVEL (ONLY CONSIDERS ONLINE NODES) ---
void updateGlobalThreatLevel(unsigned long now) {
  int highestThreat = 1;
  for (int i = 1; i < MAX_NODES; i++) {
    if (nodes[i].heard && !isNodeOffline(i, now)) {
      long d = nodes[i].latestDistance;
      if (d > 0 && d <= 179) {
        highestThreat = 3; // Critical
        break;
      } else if (d >= 180 && d <= 300) {
        if (highestThreat < 2) highestThreat = 2; // Warning
      }
    }
  }
  globalMode = highestThreat;
}

// --- HELPER: NORMALIZE PHILIPPINE SIM NUMBER & SKIP DUMMY PLACEHOLDER ---
String normalizePhoneNumber(String raw) {
  raw.trim();
  if (raw == "+639123456789" || raw == "09123456789" || raw.length() < 11) {
    return "";
  }
  if (raw.startsWith("09") && raw.length() == 11) {
    return "+63" + raw.substring(1);
  }
  return raw;
}

// --- HELPER: DETECT IF A NEW MOBILE NUMBER WAS ADDED ON THE WEBSITE ---
bool hasNewlyAddedNumber(const String &oldList, const String &newList) {
  if (newList.length() < 11) return false;
  int startIdx = 0;
  while (startIdx < (int)newList.length()) {
    int commaIdx = newList.indexOf(',', startIdx);
    if (commaIdx == -1) commaIdx = newList.length();

    String p = normalizePhoneNumber(newList.substring(startIdx, commaIdx));
    if (p.length() >= 11 && oldList.indexOf(p) == -1) {
      return true;
    }
    startIdx = commaIdx + 1;
  }
  return false;
}

// --- HELPER: WAIT FOR SPECIFIC SUBSTRING FROM SIM800L UART ---
bool waitSim800Response(const char* expected, unsigned long timeoutMs) {
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeoutMs) {
    while (sim800.available()) {
      char c = (char)sim800.read();
      resp += c;
      if (resp.indexOf(expected) != -1) {
        return true;
      }
      if (resp.indexOf("ERROR") != -1) {
        return false;
      }
    }
    delay(10);
  }
  return false;
}

// --- HELPER: SEND SINGLE SMS VIA SIM800L WITH FULL HANDSHAKE ---
bool sendSingleSMS(const String &rawPhone, const String &messageText) {
  String phone = normalizePhoneNumber(rawPhone);
  if (phone.length() < 11) return false;

  Serial.print("[SMS] Sending message to: ");
  Serial.println(phone);

  // 1. Verify AT command mode (recover with ESC if stuck in an unfinished prompt)
  while (sim800.available()) sim800.read();
  sim800.println("AT");
  if (!waitSim800Response("OK", 600)) {
    sim800.write(27); // ESC cancels any stuck prompt
    delay(200);
    while (sim800.available()) sim800.read();
    sim800.println("AT");
    waitSim800Response("OK", 800);
  }

  // 2. Ensure SMS Text Mode and completely flush its "OK" before AT+CMGS
  while (sim800.available()) sim800.read();
  sim800.println("AT+CMGF=1");
  waitSim800Response("OK", 1000);
  delay(50);
  while (sim800.available()) sim800.read();

  // 3. Open SMS recipient prompt and wait for '>'
  sim800.print("AT+CMGS=\"");
  sim800.print(phone);
  sim800.println("\"");
  if (!waitSim800Response(">", 3000)) {
    Serial.println("[SMS] '>' prompt timeout, attempting send...");
  }
  delay(50);
  while (sim800.available()) sim800.read();

  // 4. Write message body, flush UART echo, then send CTRL+Z (ASCII 26)
  sim800.print(messageText);
  delay(150);
  while (sim800.available()) sim800.read();
  sim800.write(26);

  // 5. Wait up to 10 seconds for GSM cellular network confirmation (+CMGS: ... OK)
  unsigned long start = millis();
  String resp = "";
  bool sentOk = false;
  while (millis() - start < 10000UL) {
    while (sim800.available()) {
      char c = (char)sim800.read();
      resp += c;
      if ((resp.indexOf("+CMGS:") != -1 && resp.indexOf("OK") != -1) ||
          resp.endsWith("OK\r\n")) {
        sentOk = true;
        break;
      }
      if (resp.indexOf("ERROR") != -1) {
        break;
      }
    }
    if (sentOk || resp.indexOf("ERROR") != -1) break;
    delay(20);
  }

  Serial.print("[SMS] Result for ");
  Serial.print(phone);
  Serial.print(": ");
  Serial.println(sentOk ? "DELIVERED TO GSM NETWORK" : resp);

  delay(1000); // Allow SIM800L baseband to settle before next recipient
  return sentOk;
}

// --- DISPATCH SMS ALERTS (REGISTERED DATABASE NUMBERS + OFFLINE FALLBACK) ---
bool broadcastSMS(const String &messageText) {
  String numbersCopy;
  portENTER_CRITICAL(&nodeMux);
  numbersCopy = dynamicPhoneNumbers;
  portEXIT_CRITICAL(&nodeMux);

  int recipientCount = 0;
  int deliveredCount = 0;

  // 1. Text all numbers synced from the website database
  if (numbersCopy.length() >= 11) {
    int startIdx = 0;
    while (startIdx < (int)numbersCopy.length()) {
      int commaIdx = numbersCopy.indexOf(',', startIdx);
      if (commaIdx == -1) commaIdx = numbersCopy.length();

      String phone = normalizePhoneNumber(numbersCopy.substring(startIdx, commaIdx));
      if (phone.length() >= 11) {
        recipientCount++;
        if (sendSingleSMS(phone, messageText)) {
          deliveredCount++;
        }
      }
      startIdx = commaIdx + 1;
    }
  }

  // 2. Also text the built-in offline fallback number (if set to a real SIM & not duplicate)
  String fallbackNorm = normalizePhoneNumber(String(fallbackPhoneNumber));
  if (fallbackNorm.length() >= 11 && numbersCopy.indexOf(fallbackNorm) == -1) {
    recipientCount++;
    if (sendSingleSMS(fallbackNorm, messageText)) {
      deliveredCount++;
    }
  }

  if (recipientCount == 0) {
    Serial.println("[SMS] No registered or real fallback SIM numbers available yet.");
    return false;
  }
  return (deliveredCount > 0);
}

// --- DISPATCH QUEUED REGISTRATION CONFIRMATION SMS ON CORE 1 ---
void processWelcomeSmsQueue(unsigned long now) {
  WelcomeItem item;
  bool hasItem = false;

  portENTER_CRITICAL(&nodeMux);
  if (welcomeQueueCount > 0) {
    item    = welcomeQueue[0];
    hasItem = true;
  }
  portEXIT_CRITICAL(&nodeMux);

  if (!hasItem) return;

  // Space out retries by 10 seconds if GSM network was still registering
  if (item.attempts > 0 && (now - lastWelcomeTryMillis < 10000UL)) {
    return;
  }

  Serial.print("[Welcome] Sending registration confirmation SMS to ID ");
  Serial.print(item.recordId);
  Serial.print(" (");
  Serial.print(item.phone);
  Serial.println(")...");

  bool sent = sendSingleSMS(String(item.phone), String(welcomeSmsMessage));

  if (sent || (item.attempts + 1) >= 3) {
    long updatedWatermark = -1;

    portENTER_CRITICAL(&nodeMux);
    for (int i = 0; i < welcomeQueueCount - 1; i++) {
      welcomeQueue[i] = welcomeQueue[i + 1];
    }
    if (welcomeQueueCount > 0) welcomeQueueCount--;
    if (item.recordId > lastWelcomeId) {
      lastWelcomeId = item.recordId;
    }
    updatedWatermark = lastWelcomeId;
    portEXIT_CRITICAL(&nodeMux);

    lastWelcomeTryMillis = 0;
    if (updatedWatermark >= 0) {
      saveWelcomeWatermarkToNvs(updatedWatermark);
    }

    Serial.print("[Welcome] Confirmation finalized for ID ");
    Serial.print(item.recordId);
    Serial.print(" | Saved watermark ID: ");
    Serial.println(updatedWatermark);
  } else {
    portENTER_CRITICAL(&nodeMux);
    if (welcomeQueueCount > 0 && welcomeQueue[0].recordId == item.recordId) {
      welcomeQueue[0].attempts++;
    }
    portEXIT_CRITICAL(&nodeMux);
    lastWelcomeTryMillis = millis();
  }
}

// ============================================================================
// CORE 0 TASK: SECURE BIDIRECTIONAL WEBSITE DATABASE SYNC (HTTPS + X-API-KEY)
// ============================================================================
void TaskWiFiCloud(void *pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
      unsigned long waitStart = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - waitStart < 8000UL)) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      // 1. SECURELY RETRIEVE REGISTERED NUMBERS + DB IDs (/api/get_numbers?with_ids=1)
      {
        WiFiClientSecure clientGet;
        clientGet.setInsecure();
        HTTPClient httpGet;
        httpGet.setTimeout(10000);
        httpGet.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        String syncUrl = String(numbersURL);
        syncUrl += (syncUrl.indexOf('?') == -1) ? "?with_ids=1" : "&with_ids=1";

        if (httpGet.begin(clientGet, syncUrl)) {
          httpGet.addHeader("X-API-Key", gatewayApiKey);
          int httpCode = httpGet.GET();
          if (httpCode == HTTP_CODE_OK) {
            String payload = httpGet.getString();
            payload.trim();

            bool hasIdFormat   = (payload.indexOf(':') != -1);
            String cleanNumbers = "";
            long maxDbId       = 0;

            // Pass 1: Extract clean comma-separated SIM numbers & find highest DB ID
            if (payload.length() > 0) {
              int startIdx = 0;
              while (startIdx < (int)payload.length()) {
                int commaIdx = payload.indexOf(',', startIdx);
                if (commaIdx == -1) commaIdx = payload.length();

                String token = payload.substring(startIdx, commaIdx);
                token.trim();
                if (token.length() > 0) {
                  int colonIdx    = token.indexOf(':');
                  long recId      = -1;
                  String rawPhone = token;

                  if (colonIdx > 0) {
                    recId    = token.substring(0, colonIdx).toInt();
                    rawPhone = token.substring(colonIdx + 1);
                  }

                  String normPhone = normalizePhoneNumber(rawPhone);
                  if (normPhone.length() >= 11) {
                    if (cleanNumbers.length() > 0) cleanNumbers += ",";
                    cleanNumbers += normPhone;
                  }
                  if (recId > maxDbId) {
                    maxDbId = recId;
                  }
                }
                startIdx = commaIdx + 1;
              }
            }

            // Update cached emergency SMS recipient list
            String oldNumbers;
            bool changed = false;
            long watermarkSnap;

            portENTER_CRITICAL(&nodeMux);
            oldNumbers           = dynamicPhoneNumbers;
            changed              = (dynamicPhoneNumbers != cleanNumbers);
            dynamicPhoneNumbers  = cleanNumbers;
            watermarkSnap        = lastWelcomeId;
            portEXIT_CRITICAL(&nodeMux);

            // Handle Confirmation SMS Watermark & Queueing
            if (payload.length() == 0) {
              // Empty registry on first sync -> baseline watermark to 0
              if (watermarkSnap < 0) {
                portENTER_CRITICAL(&nodeMux);
                lastWelcomeId   = 0;
                highestQueuedId = 0;
                portEXIT_CRITICAL(&nodeMux);
                saveWelcomeWatermarkToNvs(0);
                Serial.println("[Welcome] Baselined empty registry (lastWelcomeId = 0).");
              }
            } else if (hasIdFormat) {
              if (watermarkSnap < 0) {
                // First-boot baseline: skip numbers registered BEFORE Fin05 was initialized
                portENTER_CRITICAL(&nodeMux);
                lastWelcomeId   = maxDbId;
                highestQueuedId = maxDbId;
                portEXIT_CRITICAL(&nodeMux);
                saveWelcomeWatermarkToNvs(maxDbId);

                Serial.print("[Welcome] Baselined pre-existing registrations up to ID ");
                Serial.print(maxDbId);
                Serial.println(" (no SMS sent to old numbers).");
              } else {
                // Pass 2: Enqueue only genuinely new registrations (recId > highestQueuedId)
                int startIdx = 0;
                while (startIdx < (int)payload.length()) {
                  int commaIdx = payload.indexOf(',', startIdx);
                  if (commaIdx == -1) commaIdx = payload.length();

                  String token = payload.substring(startIdx, commaIdx);
                  token.trim();
                  int colonIdx = token.indexOf(':');
                  if (colonIdx > 0) {
                    long recId       = token.substring(0, colonIdx).toInt();
                    String normPhone = normalizePhoneNumber(token.substring(colonIdx + 1));

                    if (recId > 0 && normPhone.length() >= 11) {
                      bool queuedNow = false;
                      portENTER_CRITICAL(&nodeMux);
                      if (recId > highestQueuedId && welcomeQueueCount < MAX_WELCOME_QUEUE) {
                        WelcomeItem &slot = welcomeQueue[welcomeQueueCount++];
                        slot.recordId     = recId;
                        normPhone.toCharArray(slot.phone, sizeof(slot.phone));
                        slot.attempts     = 0;
                        highestQueuedId   = recId;
                        newNumberRegistered = true;
                        queuedNow         = true;
                      }
                      portEXIT_CRITICAL(&nodeMux);

                      if (queuedNow) {
                        Serial.print("[Welcome] Queued confirmation SMS for new ID ");
                        Serial.print(recId);
                        Serial.print(": ");
                        Serial.println(normPhone);
                      }
                    }
                  }
                  startIdx = commaIdx + 1;
                }
              }
            } else if (changed && watermarkSnap >= 0 && hasNewlyAddedNumber(oldNumbers, cleanNumbers)) {
              newNumberRegistered = true;
            }

            if (changed) {
              saveSmsListToNvs(cleanNumbers);
              Serial.print("[Cloud] Synced registered SMS numbers: ");
              Serial.println(cleanNumbers.length() > 0 ? cleanNumbers : "(none)");
            }
          } else {
            Serial.print("[Cloud] GET numbers HTTP code: ");
            Serial.println(httpCode);
          }
          httpGet.end();
        }
      }

      vTaskDelay(pdMS_TO_TICKS(200));

      // 2. SECURELY UPLOAD ALL DISCOVERED SOLDIER NODES IN ONE COMBINED HTTPS REQUEST
      String nodesJsonArray = "[";
      bool hasDiscoveredNodes = false;
      long primaryActiveDist  = 999;
      long legacyNodeDist[4]  = { 999, 999, 999, 999 };

      for (int i = 1; i < MAX_NODES; i++) {
        bool heardSnap;
        long distSnap;
        int batSnap;
        float rssiSnap;
        unsigned long seenSnap;

        portENTER_CRITICAL(&nodeMux);
        heardSnap = nodes[i].heard;
        distSnap  = nodes[i].latestDistance;
        batSnap   = nodes[i].latestBattery;
        rssiSnap  = nodes[i].latestRSSI;
        seenSnap  = nodes[i].lastSeenMillis;
        portEXIT_CRITICAL(&nodeMux);

        if (heardSnap) {
          if (hasDiscoveredNodes) nodesJsonArray += ",";
          hasDiscoveredNodes = true;

          unsigned long now = millis();
          bool offline      = (now - seenSnap >= OFFLINE_TIMEOUT_MS);
          String statusStr  = offline ? "OFFLINE" : "ONLINE";

          if (!offline && distSnap > 0 && distSnap < primaryActiveDist) {
            primaryActiveDist = distSnap;
          }
          if (i >= 1 && i <= 3) {
            legacyNodeDist[i] = distSnap;
          }

          nodesJsonArray += "{\"id\":" + String(i) +
                            ",\"distance\":" + String(distSnap) +
                            ",\"battery\":" + String(batSnap) +
                            ",\"rssi\":" + String((int)rssiSnap) +
                            ",\"status\":\"" + statusStr + "\"}";
        }
      }
      nodesJsonArray += "]";

      if (hasDiscoveredNodes) {
        String jsonPayload = "{\"mode\":" + String(globalMode) +
                             ",\"distance\":" + String(primaryActiveDist) +
                             ",\"node1\":" + String(legacyNodeDist[1] < 999 ? legacyNodeDist[1] : primaryActiveDist) +
                             ",\"node2\":" + String(legacyNodeDist[2]) +
                             ",\"node3\":" + String(legacyNodeDist[3]) +
                             ",\"nodes\":" + nodesJsonArray + "}";

        WiFiClientSecure clientPost;
        clientPost.setInsecure();
        HTTPClient httpPost;
        httpPost.setTimeout(10000);
        httpPost.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (httpPost.begin(clientPost, uploadURL)) {
          httpPost.addHeader("Content-Type", "application/json");
          httpPost.addHeader("X-API-Key", gatewayApiKey);
          int postCode = httpPost.POST(jsonPayload);
          if (postCode != HTTP_CODE_OK) {
            Serial.print("[Cloud] POST upload HTTP code: ");
            Serial.println(postCode);
          }
          httpPost.end();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ============================================================================
// OLED UI RENDERING (CAROUSEL MODE vs. DETAILED 5-HISTORY VIEW)
// ============================================================================
void renderGatewayOled(unsigned long now) {
  int discoveredList[MAX_NODES];
  int discoveredCount = getDiscoveredNodes(discoveredList);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // CASE 1: No transmissions received from any soldier node yet
  if (discoveredCount == 0) {
    display.setCursor(0, 2);
    display.println("RIVERGUARD GATEWAY");
    display.setCursor(0, 14);
    display.println("No nodes");
    display.setCursor(0, 23);
    display.println("transmission yet");
    display.display();
    return;
  }

  // CASE 2: DEFAULT CAROUSEL MODE
  if (currentUiMode == UI_CAROUSEL) {
    if (carouselCursor >= discoveredCount) {
      carouselCursor = 0;
    }
    int nodeId   = discoveredList[carouselCursor];
    bool offline = isNodeOffline(nodeId, now);

    if (!offline) {
      // Concise Online Carousel View: Node #, Battery %, Recent Reading, and RSSI
      display.setCursor(0, 0);
      display.print("NODE "); display.print(nodeId);
      display.print(" | BAT:"); display.print(nodes[nodeId].latestBattery); display.println("%");

      display.setCursor(0, 10);
      display.setTextSize(2);
      display.print(nodes[nodeId].latestDistance); display.print("cm");
      display.setTextSize(1);

      display.setCursor(85, 10);
      if (nodes[nodeId].latestDistance <= 179) display.print("CRIT");
      else if (nodes[nodeId].latestDistance <= 300) display.print("WARN");
      else display.print("SAFE");

      display.setCursor(85, 22);
      display.print((int)nodes[nodeId].latestRSSI); display.print("dBm");
    } else {
      // Offline Carousel View: Label Offline + Duration since last Rx and since Offline
      unsigned long sinceLastRxMs      = now - nodes[nodeId].lastSeenMillis;
      unsigned long sinceDeclaredOffMs = sinceLastRxMs - OFFLINE_TIMEOUT_MS;

      display.setCursor(0, 0);
      display.print("NODE "); display.print(nodeId); display.println(": OFFLINE");

      display.setCursor(0, 11);
      display.print("LastRx: "); display.println(formatDuration(sinceLastRxMs));

      display.setCursor(0, 22);
      display.print("OffDur: "); display.println(formatDuration(sinceDeclaredOffMs));
    }

    display.display();
    return;
  }

  // CASE 3: DETAILED NODE VIEW (5 HISTORICAL READINGS)
  if (currentUiMode == UI_DETAILED) {
    if (!nodes[detailNodeID].heard) {
      detailNodeID = discoveredList[0];
    }

    int availHist = nodes[detailNodeID].historyCount;
    if (detailHistoryIdx >= availHist) {
      detailHistoryIdx = 0;
    }

    ReadingRecord rec = nodes[detailNodeID].history[detailHistoryIdx];
    bool offline      = isNodeOffline(detailNodeID, now);

    // Line 1: Node ID, Status, and History Index (1/5 .. 5/5)
    display.setCursor(0, 0);
    display.print("N"); display.print(detailNodeID);
    display.print(offline ? "[OFF]" : "[ON]");
    display.print(" Hist ");
    display.print(detailHistoryIdx + 1); display.print("/"); display.print(availHist);

    // Line 2: Historical Reading Distance & Battery %
    display.setCursor(0, 11);
    display.print("Val:"); display.print(rec.distance);
    display.print("cm | Bat:"); display.print(rec.batteryPct); display.print("%");

    // Line 3: Signal Quality + Age of that specific reading
    display.setCursor(0, 22);
    display.print((int)rec.rssi); display.print("dBm | ");
    display.print(formatDuration(now - rec.timestamp)); display.print(" ago");

    display.display();
  }
}

// ============================================================================
// NON-BLOCKING MOMENTARY BUTTON HANDLER (MAIN NODE)
// ============================================================================
void handleMainNodeButton(unsigned long now) {
  bool rawReading = digitalRead(momentaryBtnPin);

  if (rawReading != btnLastReading) {
    btnLastDebounce = now;
  }
  btnLastReading = rawReading;

  if ((now - btnLastDebounce) > 30UL) {
    if (rawReading != btnStableState) {
      btnStableState = rawReading;

      if (btnStableState == LOW) {
        btnPressStartTime = now;
        btnLongPressFired = false;
        lastBtnActivity   = now;
      } else {
        lastBtnActivity = now;
        if (!btnLongPressFired) {
          // SHORT CLICK: Loop within the 5 history readings of current node
          if (currentUiMode == UI_DETAILED && nodes[detailNodeID].historyCount > 0) {
            detailHistoryIdx = (detailHistoryIdx + 1) % nodes[detailNodeID].historyCount;
            renderGatewayOled(now);
          }
        }
      }
    }

    // LONG PRESS (>= 800ms): Enter Detailed View or switch to next Online/Discovered Node
    if (btnStableState == LOW && !btnLongPressFired && (now - btnPressStartTime >= 800UL)) {
      btnLongPressFired = true;
      lastBtnActivity   = now;

      int discoveredList[MAX_NODES];
      int discoveredCount = getDiscoveredNodes(discoveredList);

      if (discoveredCount > 0) {
        if (currentUiMode == UI_CAROUSEL) {
          currentUiMode    = UI_DETAILED;
          int firstOnline  = getNextDetailNode(0, now);
          detailNodeID     = (firstOnline > 0) ? firstOnline : discoveredList[0];
          detailHistoryIdx = 0;
        } else {
          int nextNode     = getNextDetailNode(detailNodeID, now);
          if (nextNode > 0) {
            detailNodeID = nextNode;
          }
          detailHistoryIdx = 0;
        }
        renderGatewayOled(now);
      }
    }
  }

  // 15-Second Inactivity Timeout -> Return to Carousel
  if (currentUiMode == UI_DETAILED && (now - lastBtnActivity >= DETAIL_TIMEOUT_MS)) {
    currentUiMode    = UI_CAROUSEL;
    lastCarouselStep = now;
    renderGatewayOled(now);
  }
}

// ============================================================================
// SETUP (Core 1 Initialization & POST)
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(3000); // 3-second hardware stabilization delay for LoRa & SIM800L

  nvsMutex = xSemaphoreCreateMutex();
  sim800.begin(9600, SERIAL_8N1, simRxPin, simTxPin);

  // Load last synced website numbers + confirmation SMS watermark from ESP32 Flash
  prefs.begin("riverguard", true);
  dynamicPhoneNumbers = prefs.getString("sms_list", "");
  lastWelcomeId       = prefs.getLong("welcome_id", -1);
  highestQueuedId     = lastWelcomeId;
  prefs.end();

  pinMode(blueLED, OUTPUT);
  pinMode(loraDio1Pin, INPUT);
  pinMode(strobeRelay, OUTPUT);
  pinMode(sirenRelay, OUTPUT);
  pinMode(momentaryBtnPin, INPUT_PULLUP);

  digitalWrite(strobeRelay, HIGH);
  digitalWrite(sirenRelay, HIGH);
  digitalWrite(blueLED, LOW);

  for (int i = 0; i < MAX_NODES; i++) {
    nodes[i].heard          = false;
    nodes[i].lastMsgID      = -1;
    nodes[i].latestDistance = 999;
    nodes[i].latestBattery  = 100;
    nodes[i].latestRSSI     = 0.0f;
    nodes[i].latestSNR      = 0.0f;
    nodes[i].lastSeenMillis = 0;
    nodes[i].historyCount   = 0;
  }

  // 1. POST: OLED Display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    playSOS();
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("POST: MAIN GATEWAY");
  display.println("OLED... OK");
  display.display();
  delay(400);

  // 2. POST: SIM800L GSM MODULE (Auto-baud calibration + SMS Text Mode)
  display.println("SIM800L Init...");
  display.display();
  bool simReady = false;
  for (int attempt = 0; attempt < 4; attempt++) {
    while (sim800.available()) sim800.read();
    sim800.println("AT");
    if (waitSim800Response("OK", 500)) {
      simReady = true;
      break;
    }
    delay(250);
  }
  sim800.println("AT+CMGF=1");
  waitSim800Response("OK", 500);
  sim800.println("AT+CSCS=\"GSM\"");
  waitSim800Response("OK", 500);
  while (sim800.available()) sim800.read();
  Serial.println(simReady ? "[GSM] SIM800L Ready (Text Mode)" : "[GSM] SIM800L warming up (will retry on SMS)");

  // 3. POST: SX1262 915MHz RADIO (LOCKED TO PROVEN 2KM SETTINGS: 915.0, 125.0, 9, 7, 0x12, 20)
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("POST: MAIN GATEWAY");
  display.println(simReady ? "SIM800L... OK" : "SIM800L... WAIT");
  display.println("Radio Init...");
  display.display();
  int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 20);
  if (state != RADIOLIB_ERR_NONE) {
    bootFailureCount++;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("RADIO FAIL: "); display.println(state);
    display.print("STRIKE: "); display.print(bootFailureCount); display.println("/3");
    display.display();
    if (bootFailureCount >= 3) {
      playSOS();
    }
    delay(4000);
    ESP.restart();
  }

  bootFailureCount = 0;
  radio.setPacketReceivedAction(setRadioFlag);
  radio.startReceive();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("MAIN GATEWAY ONLINE");
  display.println("915MHz SF9/125k Ready");
  display.display();
  delay(1000);

  // Spawn Core 0 Background Wi-Fi & Database Task
  xTaskCreatePinnedToCore(
    TaskWiFiCloud,
    "WiFiCloudTask",
    10240,
    NULL,
    1,
    NULL,
    0
  );
}

// ============================================================================
// CORE 1 LOOP: LORA RECEPTION, BLUE LED RX BLINK, WELCOME SMS & ALARMS
// ============================================================================
void loop() {
  unsigned long now = millis();

  // --- 0. UPDATE NON-BLOCKING BLUE LED RX BLINK INDICATOR ---
  updateRxBlueLed(now);

  // --- 1. CATCH LORA PACKETS (Interrupt Flag OR Hardware Pin 4 Polling) ---
  if (receivedFlag || digitalRead(loraDio1Pin) == HIGH) {
    receivedFlag = false;
    String incoming;
    int state = radio.readData(incoming);

    if (state == RADIOLIB_ERR_NONE && incoming.length() > 0) {
      // Universal Parser: Supports BOTH 4-part (ID|Dist|Bat|MsgID) AND 3-part (ID|Dist|MsgID)
      int p1 = incoming.indexOf('|');
      int p2 = incoming.indexOf('|', p1 + 1);
      int p3 = incoming.indexOf('|', p2 + 1);

      if (p1 > 0 && p2 > 0) {
        int origin = incoming.substring(0, p1).toInt();
        long dist  = incoming.substring(p1 + 1, p2).toInt();
        int bat    = 100;
        int msgID  = 0;

        if (p3 > 0) {
          // 4-part payload: OriginID|Distance|BatteryPct|MsgID
          bat   = incoming.substring(p2 + 1, p3).toInt();
          msgID = incoming.substring(p3 + 1).toInt();
        } else {
          // 3-part pre-oral payload: OriginID|Distance|MsgID
          msgID = incoming.substring(p2 + 1).toInt();
          bat   = (origin >= 1 && origin < MAX_NODES) ? nodes[origin].latestBattery : 100;
        }

        if (origin >= 1 && origin < MAX_NODES) {
          // Blink Main Node Blue LED (3 visible blinks) on every sensor node reception
          triggerRxBlueLed();

          // Accept new msgID OR Soldier Node reboot (msgID <= 1)
          if (msgID != nodes[origin].lastMsgID || msgID <= 1) {
            float rssi = radio.getRSSI();
            float snr  = radio.getSNR();

            portENTER_CRITICAL(&nodeMux);
            nodes[origin].heard          = true;
            nodes[origin].lastMsgID      = msgID;
            nodes[origin].latestDistance = dist;
            nodes[origin].latestBattery  = bat;
            nodes[origin].latestRSSI     = rssi;
            nodes[origin].latestSNR      = snr;
            nodes[origin].lastSeenMillis = now;

            // Shift 5-reading history buffer (index 0 = newest, index 4 = oldest)
            for (int k = 4; k > 0; k--) {
              nodes[origin].history[k] = nodes[origin].history[k - 1];
            }
            nodes[origin].history[0] = { dist, bat, rssi, snr, now };
            if (nodes[origin].historyCount < 5) {
              nodes[origin].historyCount++;
            }
            portEXIT_CRITICAL(&nodeMux);

            Serial.print("RX Node "); Serial.print(origin);
            Serial.print(" | Dist: "); Serial.print(dist);
            Serial.print("cm | Bat: "); Serial.print(bat);
            Serial.print("% | RSSI: "); Serial.println(rssi);

            updateGlobalThreatLevel(now);
            renderGatewayOled(now);
          }
        }
      }
    }
    radio.startReceive();
  }

  // --- 2. HANDLE MOMENTARY BUTTON (LONG PRESS / SINGLE CLICK / 15s TIMEOUT) ---
  handleMainNodeButton(now);

  // --- 3. ADVANCE CAROUSEL & REFRESH SCREEN ---
  if (currentUiMode == UI_CAROUSEL) {
    if (now - lastCarouselStep >= 2500UL) {
      lastCarouselStep = now;
      carouselCursor++;
      updateGlobalThreatLevel(now);
      renderGatewayOled(now);
    } else if (now - lastUiRefresh >= 1000UL) {
      lastUiRefresh = now;
      updateGlobalThreatLevel(now);
      renderGatewayOled(now);
    }
  } else {
    if (now - lastUiRefresh >= 1000UL) {
      lastUiRefresh = now;
      renderGatewayOled(now);
    }
  }

  // --- 4. SEND CONFIRMATION SMS TO NEWLY REGISTERED NUMBERS (ALL MODES) ---
  processWelcomeSmsQueue(now);

  // --- 5. PHYSICAL 12V ALARMS & RATE-LIMITED EMERGENCY SMS DISPATCH ---
  if (globalMode == 3) {
    inSafeState = false;
    digitalWrite(strobeRelay, LOW);
    digitalWrite(sirenRelay, LOW);

    bool critDue = (!firstCritSmsSent || newNumberRegistered || (now - lastCritSmsTime >= 300000UL));
    if (critDue && (lastEmptySmsCheck == 0 || (now - lastEmptySmsCheck >= 10000UL) || newNumberRegistered)) {
      if (newNumberRegistered) smsRetryCount = 0;
      newNumberRegistered = false;
      if (broadcastSMS("RIVERGUARD CRITICAL ALERT: Water level has breached critical threshold (<180cm). Evacuate immediately!")) {
        firstCritSmsSent  = true;
        lastCritSmsTime   = millis();
        lastEmptySmsCheck = 0;
        smsRetryCount     = 0;
      } else {
        smsRetryCount++;
        if (smsRetryCount >= 6) {
          firstCritSmsSent  = true;
          lastCritSmsTime   = millis();
          lastEmptySmsCheck = 0;
          smsRetryCount     = 0;
        } else {
          lastEmptySmsCheck = millis(); // Retry in 10s once numbers sync or GSM registers
        }
      }
    }
  } else if (globalMode == 2) {
    inSafeState = false;
    digitalWrite(sirenRelay, HIGH);

    if (now - lastStrobeToggle >= 1000UL) {
      lastStrobeToggle  = now;
      strobeActiveState = !strobeActiveState;
      digitalWrite(strobeRelay, strobeActiveState ? LOW : HIGH);
    }

    bool warnDue = (!firstWarnSmsSent || newNumberRegistered || (now - lastWarnSmsTime >= 1200000UL));
    if (warnDue && (lastEmptySmsCheck == 0 || (now - lastEmptySmsCheck >= 10000UL) || newNumberRegistered)) {
      if (newNumberRegistered) smsRetryCount = 0;
      newNumberRegistered = false;
      if (broadcastSMS("RIVERGUARD WARNING: River water level is rising (180cm-300cm). Please stay alert.")) {
        firstWarnSmsSent  = true;
        lastWarnSmsTime   = millis();
        lastEmptySmsCheck = 0;
        smsRetryCount     = 0;
      } else {
        smsRetryCount++;
        if (smsRetryCount >= 6) {
          firstWarnSmsSent  = true;
          lastWarnSmsTime   = millis();
          lastEmptySmsCheck = 0;
          smsRetryCount     = 0;
        } else {
          lastEmptySmsCheck = millis(); // Retry in 10s once numbers sync or GSM registers
        }
      }
    }
  } else {
    digitalWrite(strobeRelay, HIGH);
    digitalWrite(sirenRelay, HIGH);
    newNumberRegistered = false;
    lastEmptySmsCheck   = 0;
    smsRetryCount       = 0;

    // Reset SMS alert latches after 10 seconds in SAFE so subsequent flood rises trigger immediately
    if (!inSafeState) {
      inSafeState     = true;
      safeSinceMillis = now;
    } else if (now - safeSinceMillis >= 10000UL) {
      firstWarnSmsSent = false;
      firstCritSmsSent = false;
    }
  }
}
