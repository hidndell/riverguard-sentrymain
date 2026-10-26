#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ============================================================================
// >>> WI-FI & RENDER CLOUD ENDPOINTS (WITH X-API-KEY SECURITY) <<<
// ============================================================================
const char* ssid          = "realme 9 Pro 5G";
const char* password      = "12345678";

// Update the domain below if Render assigns a different subdomain on deployment:
const char* uploadURL     = "https://riverguard-sentrymain.onrender.com/api/upload";
const char* numbersURL    = "https://riverguard-sentrymain.onrender.com/api/get_numbers";

// Must match GATEWAY_API_KEY in Render Environment Variables (app.py default: rg-gateway-2026-key)
const char* gatewayApiKey = "rg-gateway-2026-key";

// --- HARDWARE PIN ASSIGNMENTS (MAIN GATEWAY NODE) ---
const int blueLED         = 2;  // Onboard Blue LED (Rx/Tx indicator & SOS)
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
volatile int globalMode          = 1; // 1 = SAFE, 2 = WARNING, 3 = CRITICAL
String dynamicPhoneNumbers       = "";
unsigned long lastWarnSmsTime    = 0;
unsigned long lastCritSmsTime    = 0;
bool firstWarnSmsSent            = false;
bool firstCritSmsSent            = false;
unsigned long lastStrobeToggle   = 0;
bool strobeActiveState           = false;

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

// --- HELPER: WAIT FOR SIM800L RESPONSE BETWEEN MULTI-RECIPIENT SMS ---
void waitSim800Ready(unsigned long timeoutMs) {
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeoutMs) {
    while (sim800.available()) {
      char c = (char)sim800.read();
      resp += c;
      if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) {
        delay(200);
        return;
      }
    }
    delay(20);
  }
}

// --- DISPATCH SMS ALERTS TO REGISTERED DATABASE NUMBERS ---
void broadcastSMS(const String &messageText) {
  String numbersCopy;
  portENTER_CRITICAL(&nodeMux);
  numbersCopy = dynamicPhoneNumbers;
  portEXIT_CRITICAL(&nodeMux);

  if (numbersCopy.length() < 7) return;

  int startIdx = 0;
  while (startIdx < (int)numbersCopy.length()) {
    int commaIdx = numbersCopy.indexOf(',', startIdx);
    if (commaIdx == -1) commaIdx = numbersCopy.length();

    String phone = numbersCopy.substring(startIdx, commaIdx);
    phone.trim();

    if (phone.length() >= 10) {
      while (sim800.available()) sim800.read(); // Flush stale UART bytes
      sim800.println("AT+CMGF=1");
      delay(150);
      sim800.print("AT+CMGS=\"");
      sim800.print(phone);
      sim800.println("\"");
      delay(250);
      sim800.print(messageText);
      sim800.write(26); // CTRL+Z
      waitSim800Ready(4500); // Wait for GSM network confirmation before next recipient
    }
    startIdx = commaIdx + 1;
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
      WiFiClientSecure client;
      client.setInsecure();

      // 1. SECURELY RETRIEVE REGISTERED PHONE NUMBERS FROM WEBSITE DATABASE
      {
        HTTPClient httpGet;
        httpGet.setTimeout(10000);
        if (httpGet.begin(client, numbersURL)) {
          httpGet.addHeader("X-API-Key", gatewayApiKey);
          int httpCode = httpGet.GET();
          if (httpCode == HTTP_CODE_OK) {
            String payload = httpGet.getString();
            payload.trim();
            // Always sync on HTTP 200 (including empty "" when Admin deletes all numbers)
            portENTER_CRITICAL(&nodeMux);
            dynamicPhoneNumbers = payload;
            portEXIT_CRITICAL(&nodeMux);
          } else {
            Serial.print("[Cloud] GET numbers HTTP code: ");
            Serial.println(httpCode);
          }
          httpGet.end();
        }
      }

      vTaskDelay(pdMS_TO_TICKS(200));

      // 2. SECURELY UPLOAD ALL DISCOVERED NODES IN ONE COMBINED HTTPS REQUEST
      String jsonPayload = "{\"mode\":" + String(globalMode) + ",\"nodes\":[";
      bool hasDiscoveredNodes = false;

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
          if (hasDiscoveredNodes) jsonPayload += ",";
          hasDiscoveredNodes = true;

          unsigned long now = millis();
          bool offline      = (now - seenSnap >= OFFLINE_TIMEOUT_MS);
          String statusStr  = offline ? "OFFLINE" : "ONLINE";

          jsonPayload += "{\"id\":" + String(i) +
                         ",\"distance\":" + String(distSnap) +
                         ",\"battery\":" + String(batSnap) +
                         ",\"rssi\":" + String((int)rssiSnap) +
                         ",\"status\":\"" + statusStr + "\"}";
        }
      }
      jsonPayload += "]}";

      if (hasDiscoveredNodes) {
        HTTPClient httpPost;
        httpPost.setTimeout(10000);
        if (httpPost.begin(client, uploadURL)) {
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

  sim800.begin(9600, SERIAL_8N1, simRxPin, simTxPin);

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
  delay(500);

  // 2. POST: SX1262 915MHz RADIO (LOCKED TO PROVEN 2KM SETTINGS: 915.0, 125.0, 9, 7, 0x12, 20)
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
// CORE 1 LOOP: LORA RECEPTION, OFFLINE MONITOR, ALARMS & INTERACTIVE OLED
// ============================================================================
void loop() {
  unsigned long now = millis();

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
          bat   = ( origin >= 1 && origin < MAX_NODES ) ? nodes[origin].latestBattery : 100;
        }

        // Accept if new msgID OR if Soldier Node rebooted (msgID == 1)
        if (origin >= 1 && origin < MAX_NODES &&
            (msgID != nodes[origin].lastMsgID || msgID <= 1)) {
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

          // Flash Gateway LED on packet receipt
          digitalWrite(blueLED, HIGH);
          delay(20);
          digitalWrite(blueLED, LOW);

          updateGlobalThreatLevel(now);
          renderGatewayOled(now);
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

  // --- 4. PHYSICAL 12V ALARMS & RATE-LIMITED SMS DISPATCH ---
  if (globalMode == 3) {
    digitalWrite(strobeRelay, LOW);
    digitalWrite(sirenRelay, LOW);

    if (!firstCritSmsSent || (now - lastCritSmsTime >= 300000UL)) {
      firstCritSmsSent = true;
      lastCritSmsTime  = now;
      broadcastSMS("RIVERGUARD CRITICAL ALERT: Water level has breached critical threshold (<180cm). Evacuate immediately!");
    }
  } else if (globalMode == 2) {
    digitalWrite(sirenRelay, HIGH);

    if (now - lastStrobeToggle >= 1000UL) {
      lastStrobeToggle  = now;
      strobeActiveState = !strobeActiveState;
      digitalWrite(strobeRelay, strobeActiveState ? LOW : HIGH);
    }

    if (!firstWarnSmsSent || (now - lastWarnSmsTime >= 1200000UL)) {
      firstWarnSmsSent = true;
      lastWarnSmsTime  = now;
      broadcastSMS("RIVERGUARD WARNING: River water level is rising (180cm-300cm). Please stay alert.");
    }
  } else {
    digitalWrite(strobeRelay, HIGH);
    digitalWrite(sirenRelay, HIGH);
  }
}
