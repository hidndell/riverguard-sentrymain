#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ============================================================================
// RIVERGUARD SOLDIER NODE — VERSION Fin03
// Changes from Fin02:
//   - Filters out 999 timeouts before taking the 5-ping median so occasional
//     missed echoes never corrupt valid water-level readings.
//   - Fixes the 999 EMA poisoning bug (never blends 999.0 into emaDistance),
//     restoring smooth, incremental EMA distance transitions without spikes.
// ============================================================================

// ============================================================================
// >>> SET UNIQUE NODE ID FOR EACH SOLDIER NODE (1, 2, 3, etc.) <<<
// ============================================================================
const int NODE_ID = 1;

// --- HARDWARE PIN ASSIGNMENTS ---
const int mosfetGatePin   = 32; // IRLZ44N Low-side switch for JSN-SR04T GND
const int trigPin         = 25; // JSN-SR04T Trigger (RX)
const int echoPin         = 13; // JSN-SR04T Echo (TX)
const int blueLED         = 2;  // Onboard ESP32 Blue LED (Send/Relay/SOS)
const int momentaryBtnPin = 15; // External Waterproof Momentary Button (D15 -> GND)
const int batteryAdcPin   = 34; // 100k/100k Voltage Divider Center Tap (ADC1)

// --- SX1262 915MHz WIRING (SOLDIER NODE: NSS=5, DIO1=33, RST=14, BUSY=Ignored) ---
const int LORA_NSS_PIN    = 5;
const int LORA_DIO1_PIN   = 33;
const int LORA_RST_PIN    = 14;
SX1262 radio = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, RADIOLIB_NC);

// --- OLED DISPLAY (Default I2C: SDA = 21, SCL = 22) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- WATCHDOG COUNTER IN RTC MEMORY ---
RTC_DATA_ATTR int bootFailureCount = 0;

// --- THREAD-SAFE QUEUE & SHARED TELEMETRY ---
struct TelemetryPacket {
  long distance;
  int batteryPct;
  float batteryVolts;
};
QueueHandle_t sensorQueue;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

volatile long currentDistanceCM   = 999;
volatile int currentBatteryPct    = 0;
volatile float currentBatteryVolt = 0.0f;
volatile int currentScanStep      = 0;

// Recent Relayed Packet Tracking
int lastRelayedOrigin             = 0;
long lastRelayedDist              = 0;
int lastRelayedBat                = 0;
unsigned long lastRelayedMillis   = 0;
float lastRelayedRSSI             = 0.0f;

// --- RADIO INTERRUPT & MESH VARIABLES ---
volatile bool receivedFlag        = false;
int myMsgCount                    = 0;
int lastSeenMsg[15]               = {0};

struct PendingRelay {
  bool active;
  String payload;
  unsigned long fireAtMillis;
};
PendingRelay relayBuffer = { false, "", 0 };

// --- NON-BLOCKING D2 LED STATE MACHINE ---
// Hold 1s (1000ms) for SENDING; Fast Blink Twice for RELAYING
enum LedIndicatorState {
  LED_IDLE,
  LED_SEND_HOLD,
  LED_RELAY_ON1,
  LED_RELAY_OFF1,
  LED_RELAY_ON2,
  LED_RELAY_OFF2
};
LedIndicatorState ledState = LED_IDLE;
unsigned long ledTimer     = 0;

void triggerSendLed() {
  digitalWrite(blueLED, HIGH);
  ledState = LED_SEND_HOLD;
  ledTimer = millis();
}

void triggerRelayLed() {
  digitalWrite(blueLED, HIGH);
  ledState = LED_RELAY_ON1;
  ledTimer = millis();
}

void updateLedStateMachine(unsigned long now) {
  switch (ledState) {
    case LED_IDLE:
      break;
    case LED_SEND_HOLD:
      if (now - ledTimer >= 1000UL) {
        digitalWrite(blueLED, LOW);
        ledState = LED_IDLE;
      }
      break;
    case LED_RELAY_ON1:
      if (now - ledTimer >= 100UL) {
        digitalWrite(blueLED, LOW);
        ledState = LED_RELAY_OFF1;
        ledTimer = now;
      }
      break;
    case LED_RELAY_OFF1:
      if (now - ledTimer >= 100UL) {
        digitalWrite(blueLED, HIGH);
        ledState = LED_RELAY_ON2;
        ledTimer = now;
      }
      break;
    case LED_RELAY_ON2:
      if (now - ledTimer >= 100UL) {
        digitalWrite(blueLED, LOW);
        ledState = LED_RELAY_OFF2;
        ledTimer = now;
      }
      break;
    case LED_RELAY_OFF2:
      if (now - ledTimer >= 100UL) {
        digitalWrite(blueLED, LOW);
        ledState = LED_IDLE;
      }
      break;
  }
}

// --- MOMENTARY BUTTON ISR (10-SECOND WAKE) ---
volatile bool buttonWakeRequested = false;
volatile unsigned long lastBtnIsr = 0;
bool oledIsAwake                  = false;
unsigned long oledSleepTime       = 0;
unsigned long lastOledRefresh     = 0;

#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setRadioFlag(void) {
  receivedFlag = true;
}

void IRAM_ATTR handleButtonInterrupt(void) {
  unsigned long now = millis();
  if (now - lastBtnIsr > 250UL) {
    buttonWakeRequested = true;
    lastBtnIsr = now;
  }
}

// --- WATCHDOG SOS MORSE CODE FALLBACK ---
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

// --- PRE-ORAL PROVEN ULTRASONIC FIRING SEQUENCE ---
long getSingleReading() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(10);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(100);
  digitalWrite(trigPin, LOW);

  unsigned long timeoutStart = micros();
  bool timeout = false;
  while (digitalRead(echoPin) == LOW) {
    if (micros() - timeoutStart > 45000UL) {
      timeout = true;
      break;
    }
  }
  unsigned long echoStart = micros();
  while (digitalRead(echoPin) == HIGH && !timeout) {
    if (micros() - echoStart > 45000UL) {
      timeout = true;
      break;
    }
  }
  unsigned long echoEnd = micros();
  if (timeout || echoEnd <= echoStart) return 999;

  long cm = (echoEnd - echoStart) * 0.034 / 2;
  return (cm < 20 || cm > 450) ? 999 : cm;
}

// --- HELPER: RENDER SOLDIER OLED TELEMETRY ---
void renderSoldierOled() {
  long distSnap;
  int batPctSnap;
  float batVoltSnap;
  int scanStepSnap;

  portENTER_CRITICAL(&stateMux);
  distSnap     = currentDistanceCM;
  batPctSnap   = currentBatteryPct;
  batVoltSnap  = currentBatteryVolt;
  scanStepSnap = currentScanStep;
  portEXIT_CRITICAL(&stateMux);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Line 1: Node ID + Battery Health (% and Voltage)
  display.setCursor(0, 0);
  display.print("N"); display.print(NODE_ID);
  display.print(" BAT:"); display.print(batPctSnap); display.print("% (");
  display.print(batVoltSnap, 2); display.print("V)");

  // Line 2: Latest Local Sensor Reading (or active scan progress)
  display.setCursor(0, 11);
  display.print("SENS: ");
  if (distSnap == 999 && scanStepSnap > 0) {
    display.print("Scan "); display.print(scanStepSnap); display.print("/5...");
  } else if (distSnap == 999) {
    display.print("Out of Rng");
  } else {
    display.print(distSnap); display.print("cm ");
    if (distSnap <= 179) display.print("[CRIT]");
    else if (distSnap <= 300) display.print("[WARN]");
    else display.print("[SAFE]");
  }

  // Line 3: Recent Relayed Transmission Details
  display.setCursor(0, 22);
  if (lastRelayedOrigin == 0) {
    display.print("RELAY: None yet");
  } else {
    unsigned long ageSec = (millis() - lastRelayedMillis) / 1000UL;
    display.print("RLY N"); display.print(lastRelayedOrigin);
    display.print(":"); display.print(lastRelayedDist);
    display.print("cm ("); display.print(ageSec); display.print("s ago)");
  }

  display.display();
}

// ============================================================================
// CORE 0 TASK: PRE-ORAL MOSFET POWER GATING, VALID-MEDIAN & CLEAN INCREMENTAL EMA
// ============================================================================
void TaskSensorMode(void *pvParameters) {
  float emaDistance          = 0.0f;
  const float alpha          = 0.25f;
  bool hasValidEma           = false;
  int consecutiveOutOfRange  = 0;

  for (;;) {
    // 1. Sample 18650 Battery Voltage while MOSFET is OFF
    long adcSum = 0;
    for (int i = 0; i < 20; i++) {
      adcSum += analogRead(batteryAdcPin);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    float rawAvg     = adcSum / 20.0f;
    float pinVoltage = (rawAvg / 4095.0f) * 3.3f * 1.05f;
    float battVolts  = pinVoltage * 2.0f;
    int battPct      = (int)(((battVolts - 3.30f) / (4.20f - 3.30f)) * 100.0f);
    battPct          = constrain(battPct, 0, 100);

    // 2. WAKE SENSOR: Restore pins and turn MOSFET ON
    pinMode(trigPin, OUTPUT);
    digitalWrite(trigPin, LOW);
    pinMode(echoPin, INPUT);
    digitalWrite(mosfetGatePin, HIGH);

    vTaskDelay(pdMS_TO_TICKS(2000));

    // 3. SAMPLING: Take 5 readings spaced 1000ms apart so JSN blinks 5 times
    long validReadings[5];
    int validCount = 0;

    for (int i = 0; i < 5; i++) {
      portENTER_CRITICAL(&stateMux);
      currentScanStep = i + 1;
      portEXIT_CRITICAL(&stateMux);

      long sample = getSingleReading();
      if (sample >= 20 && sample <= 450) {
        validReadings[validCount++] = sample;
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 4. COMPLETE POWER-DOWN: Cut MOSFET and float Trig Pin to stop Phantom Power leak
    digitalWrite(mosfetGatePin, LOW);
    pinMode(trigPin, INPUT);
    pinMode(echoPin, INPUT);

    portENTER_CRITICAL(&stateMux);
    currentScanStep = 0;
    portEXIT_CRITICAL(&stateMux);

    // 5. Valid-Sample Median + Clean Incremental EMA (Never Poisoned by 999)
    long finalDistance = 999;

    if (validCount > 0) {
      consecutiveOutOfRange = 0;

      // Bubble sort only valid ultrasonic echoes (20..450 cm)
      for (int i = 0; i < validCount - 1; i++) {
        for (int j = i + 1; j < validCount; j++) {
          if (validReadings[i] > validReadings[j]) {
            long t           = validReadings[i];
            validReadings[i] = validReadings[j];
            validReadings[j] = t;
          }
        }
      }
      long rawMedian = validReadings[validCount / 2];

      if (!hasValidEma) {
        emaDistance = (float)rawMedian;
        hasValidEma = true;
      } else {
        // Deadband of ±1 cm eliminates stationary flicker; larger shifts step incrementally via EMA
        if (labs(rawMedian - lroundf(emaDistance)) > 1) {
          emaDistance = (rawMedian * alpha) + (emaDistance * (1.0f - alpha));
        }
      }
      finalDistance = lroundf(emaDistance);
    } else {
      // All 5 pings missed: hold last valid EMA for 1 cycle, then report 999 without corrupting emaDistance
      consecutiveOutOfRange++;
      if (hasValidEma && consecutiveOutOfRange < 2) {
        finalDistance = lroundf(emaDistance);
      } else {
        hasValidEma   = false;
        finalDistance = 999;
      }
    }

    // 6. Update Shared State for Instant Button Inspection
    portENTER_CRITICAL(&stateMux);
    currentDistanceCM  = finalDistance;
    currentBatteryPct  = battPct;
    currentBatteryVolt = battVolts;
    portEXIT_CRITICAL(&stateMux);

    // 7. Send Packet to Core 1 for LoRa Transmission
    TelemetryPacket packet = { finalDistance, battPct, battVolts };
    xQueueSend(sensorQueue, &packet, pdMS_TO_TICKS(100));

    // 8. Sleep Interval with Sensor Completely Powered OFF
    if (finalDistance >= 300 || finalDistance == 999) {
      vTaskDelay(pdMS_TO_TICKS(120000)); // Mode 1 (Safe): 120s
    } else if (finalDistance >= 180 && finalDistance < 300) {
      vTaskDelay(pdMS_TO_TICKS(30000));  // Mode 2 (Warning): 30s
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));   // Mode 3 (Critical): 5s
    }
  }
}

// ============================================================================
// SETUP (Core 1 Initialization & POST)
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(blueLED, OUTPUT);
  digitalWrite(blueLED, LOW);

  pinMode(mosfetGatePin, OUTPUT);
  digitalWrite(mosfetGatePin, LOW);
  pinMode(trigPin, INPUT);
  pinMode(echoPin, INPUT);
  pinMode(batteryAdcPin, INPUT);
  pinMode(LORA_DIO1_PIN, INPUT);

  for (int i = 0; i < 15; i++) {
    lastSeenMsg[i] = -1;
  }

  pinMode(momentaryBtnPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(momentaryBtnPin), handleButtonInterrupt, FALLING);

  randomSeed(analogRead(batteryAdcPin));

  // 1. POST: OLED Display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    playSOS();
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("POST SENTRY N"); display.println(NODE_ID);
  display.println("OLED... OK");
  display.display();
  delay(400);

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
      display.ssd1306_command(SSD1306_DISPLAYOFF);
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
  display.print("SENTRY NODE "); display.println(NODE_ID);
  display.println("915MHz SF9/125k Ready");
  display.println("Taking 1st Reading...");
  display.display();

  oledIsAwake   = true;
  oledSleepTime = millis() + 10000UL;

  sensorQueue = xQueueCreate(5, sizeof(TelemetryPacket));
  xTaskCreatePinnedToCore(
    TaskSensorMode,
    "SensorTask",
    4096,
    NULL,
    1,
    NULL,
    0
  );
}

// ============================================================================
// CORE 1 LOOP: MESH LISTENING, RELAYING, LED INDICATORS & BUTTON UI
// ============================================================================
void loop() {
  unsigned long now = millis();

  // --- 1. UPDATE NON-BLOCKING D2 LED INDICATOR ---
  updateLedStateMachine(now);

  // --- 2. HANDLE MOMENTARY BUTTON WAKE & 10-SECOND OLED TIMEOUT ---
  if (buttonWakeRequested) {
    buttonWakeRequested = false;
    oledIsAwake         = true;
    oledSleepTime       = now + 10000UL;
    display.ssd1306_command(SSD1306_DISPLAYON);
    renderSoldierOled();
    lastOledRefresh     = now;
  }

  if (oledIsAwake) {
    if (now >= oledSleepTime) {
      oledIsAwake = false;
      display.clearDisplay();
      display.display();
      display.ssd1306_command(SSD1306_DISPLAYOFF);
    } else if (now - lastOledRefresh >= 250UL) {
      lastOledRefresh = now;
      renderSoldierOled();
    }
  }

  // --- 3. INTERCEPT INCOMING LORA PACKETS FOR MESH RELAYING ---
  if (receivedFlag || digitalRead(LORA_DIO1_PIN) == HIGH) {
    receivedFlag = false;
    String incoming;
    int state = radio.readData(incoming);

    if (state == RADIOLIB_ERR_NONE && incoming.length() > 0) {
      int p1 = incoming.indexOf('|');
      int p2 = incoming.indexOf('|', p1 + 1);
      int p3 = incoming.indexOf('|', p2 + 1);

      if (p1 > 0 && p2 > 0) {
        int origin = incoming.substring(0, p1).toInt();
        long dist  = incoming.substring(p1 + 1, p2).toInt();
        int bat    = 100;
        int msgID  = 0;

        if (p3 > 0) {
          bat   = incoming.substring(p2 + 1, p3).toInt();
          msgID = incoming.substring(p3 + 1).toInt();
        } else {
          msgID = incoming.substring(p2 + 1).toInt();
        }

        if (origin > 0 && origin < 15 && origin != NODE_ID &&
            (msgID != lastSeenMsg[origin] || msgID <= 1)) {
          lastSeenMsg[origin] = msgID;

          lastRelayedOrigin = origin;
          lastRelayedDist   = dist;
          lastRelayedBat    = bat;
          lastRelayedMillis = now;
          lastRelayedRSSI   = radio.getRSSI();

          relayBuffer.active       = true;
          relayBuffer.payload      = incoming;
          relayBuffer.fireAtMillis = now + random(30, 120);
        }
      }
    }
    radio.startReceive();
  }

  // --- 4. EXECUTE SCHEDULED MESH RELAY (NON-BLOCKING BACKOFF) ---
  if (relayBuffer.active && (now >= relayBuffer.fireAtMillis)) {
    relayBuffer.active = false;
    triggerRelayLed();

    radio.clearPacketReceivedAction();
    radio.transmit(relayBuffer.payload);
    radio.setPacketReceivedAction(setRadioFlag);
    radio.startReceive();

    if (oledIsAwake) {
      renderSoldierOled();
    }
  }

  // --- 5. DISPATCH LOCAL SENSOR TELEMETRY FROM CORE 0 ---
  if (!relayBuffer.active) {
    TelemetryPacket localData;
    if (xQueueReceive(sensorQueue, &localData, 0) == pdPASS) {
      myMsgCount++;
      String payload = String(NODE_ID) + "|" +
                       String(localData.distance) + "|" +
                       String(localData.batteryPct) + "|" +
                       String(myMsgCount);

      triggerSendLed();

      radio.clearPacketReceivedAction();
      radio.transmit(payload);
      radio.setPacketReceivedAction(setRadioFlag);
      radio.startReceive();

      if (oledIsAwake) {
        renderSoldierOled();
      }
    }
  }
}
