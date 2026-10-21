#include <SPI.h>
#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

Adafruit_SSD1306 display(128, 32, &Wire, -1);

// --- SX1262 915MHz WIRING ---
SX1262 radio = new Module(5, 4, 14, 33);
volatile bool receivedFlag = false;
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) { receivedFlag = true; }

const int strobeRelay = 26;
const int sirenRelay = 27;
const int blueLED = 2; 

// --- WI-FI & DATABASE SETTINGS ---
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";
String databaseURL = "https://your-server.pythonanywhere.com/api/upload"; 
String fetchNumbersURL = "https://your-server.pythonanywhere.com/api/get_numbers";

HardwareSerial sim800(2); 
String emergencyNumber = "+639123456789"; 

// --- GLOBALS & TIMERS ---
int lastSeenMsg[10] = {0};
volatile long latestDistances[10] = {999, 999, 999, 999, 999}; 
volatile unsigned long lastSeenTime[10] = {0}; 
volatile int globalMode = 1; 

unsigned long lastWarnSmsTime = 0;
unsigned long lastCritSmsTime = 0;
unsigned long lastStrobeFlash = 0;
bool strobeState = false;

// Carousel Variables
int displayNodeIndex = 1;
unsigned long lastCarouselTick = 0;

RTC_DATA_ATTR int bootFailureCount = 0; 

void playSOS() {
  pinMode(blueLED, OUTPUT);
  while(true) {
    for(int i=0; i<3; i++) { digitalWrite(blueLED, HIGH); delay(200); digitalWrite(blueLED, LOW); delay(200); }
    delay(200);
    for(int i=0; i<3; i++) { digitalWrite(blueLED, HIGH); delay(600); digitalWrite(blueLED, LOW); delay(200); }
    delay(2000);
  }
}

void handleFailure(String comp) {
  bootFailureCount++;
  display.clearDisplay(); display.setCursor(0,0);
  display.print("FAIL: "); display.println(comp);
  display.print("STRIKES: "); display.println(bootFailureCount);
  display.display();
  if (bootFailureCount >= 3) playSOS();
  delay(5000); ESP.restart();
}

// =========================================================
// CORE 0 TASK: BACKGROUND WI-FI & DATABASE (Invisible)
// =========================================================
void TaskWiFiUpload(void *pvParameters) {
  WiFi.begin(ssid, password);
  for(;;) {
    if(WiFi.status() == WL_CONNECTED) {
      WiFiClientSecure client;
      client.setInsecure(); // Good for prototyping bypass
      
      // 1. Fetch Dynamic Numbers 
      HTTPClient http;
      http.begin(client, fetchNumbersURL);
      if(http.GET() == 200) emergencyNumber = http.getString();
      http.end(); 

      // 2. Upload Latest Threat Level
      HTTPClient httpUp;
      httpUp.begin(client, databaseURL);
      httpUp.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "mode=" + String(globalMode) + "&node1=" + String(latestDistances[1]);
      httpUp.POST(postData);
      httpUp.end();
    }
    vTaskDelay(pdMS_TO_TICKS(60000)); // Sleep Core 0 for 60 seconds
  }
}

// =========================================================
// SETUP & LOOP (Runs on Core 1)
// =========================================================
void setup() {
  Serial.begin(115200);
  sim800.begin(9600, SERIAL_8N1, 16, 17); 
  
  pinMode(strobeRelay, OUTPUT); pinMode(sirenRelay, OUTPUT);
  digitalWrite(strobeRelay, HIGH); digitalWrite(sirenRelay, HIGH);

  // POST: OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) playSOS();
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println("POST: OLED... OK"); display.display(); delay(500);

  // POST: GSM MODULE
  display.println("POST: SIM800L..."); display.display();
  sim800.println("AT"); delay(500);
  if(sim800.find("OK")) { display.println("SIM800L... OK"); } else { display.println("SIM800L... WARNING"); }
  display.display(); delay(500);
  sim800.println("AT+CMGF=1"); delay(100);

  // POST: SX1262 LORA
  display.println("POST: Radio..."); display.display();
  if (radio.begin(915.0, 125.0, 9, 7, 0x12, 20) != RADIOLIB_ERR_NONE) handleFailure("RADIO");
  radio.setPacketReceivedAction(setFlag);
  radio.startReceive();

  bootFailureCount = 0; // Clears strikes on success
  display.clearDisplay(); display.setCursor(0,0);
  display.println("MAIN GATEWAY ONLINE"); display.display(); delay(1500);

  // DEPLOY WI-FI TO CORE 0
  xTaskCreatePinnedToCore(TaskWiFiUpload, "WiFiTask", 10000, NULL, 1, NULL, 0); 
}

void loop() {
  unsigned long currentMillis = millis();

  // ==========================================
  // CATCH LORA PACKETS
  // ==========================================
  if (receivedFlag) {
    receivedFlag = false;
    String incoming; 
    if (radio.readData(incoming) == RADIOLIB_ERR_NONE) {
      int p1 = incoming.indexOf('|'); int p2 = incoming.indexOf('|', p1 + 1);
      if (p1 > 0 && p2 > 0) {
        int origin = incoming.substring(0, p1).toInt();
        long dist = incoming.substring(p1 + 1, p2).toInt();
        int msgID = incoming.substring(p2 + 1).toInt();

        if (msgID > lastSeenMsg[origin]) {
          lastSeenMsg[origin] = msgID;
          latestDistances[origin] = dist; 
          lastSeenTime[origin] = currentMillis; 
          updateGlobalThreatLevel(); 
        }
      }
    }
    radio.startReceive();
  }

  // ==========================================
  // OLED CAROUSEL (Updates every 2 seconds)
  // ==========================================
  if (currentMillis - lastCarouselTick > 2000) {
    lastCarouselTick = currentMillis;
    displayNodeIndex++;
    if (displayNodeIndex > 3) displayNodeIndex = 1; // Assuming 3 nodes for testing

    display.clearDisplay(); display.setCursor(0,0);
    if(globalMode==1) display.println("SYS: SAFE");
    if(globalMode==2) display.println("SYS: WARNING");
    if(globalMode==3) display.println("SYS: CRITICAL DANGER");
    
    if(latestDistances[displayNodeIndex] == 999 && lastSeenTime[displayNodeIndex] == 0) {
      display.print("Node "); display.print(displayNodeIndex); display.println(": OFFLINE");
    } else {
      display.print("Node "); display.print(displayNodeIndex); display.print(": "); 
      display.setTextSize(2); display.print(latestDistances[displayNodeIndex]); display.println("cm");
      display.setTextSize(1);
    }
    display.display();
  }

  // ==========================================
  // HARDWARE ALARM LOGIC
  // ==========================================
  if (globalMode == 1) {
    digitalWrite(strobeRelay, HIGH); digitalWrite(sirenRelay, HIGH); 
  } 
  else if (globalMode == 2) {
    digitalWrite(sirenRelay, HIGH); 
    if (currentMillis - lastStrobeFlash >= 30000) {
      strobeState = !strobeState; digitalWrite(strobeRelay, strobeState ? LOW : HIGH);
      lastStrobeFlash = currentMillis;
    }
    if (currentMillis - lastWarnSmsTime > 1200000) {
      sendSMS("RIVERGUARD: River rising (Warning Level)."); lastWarnSmsTime = currentMillis;
    }
  } 
  else if (globalMode == 3) {
    digitalWrite(sirenRelay, LOW); digitalWrite(strobeRelay, LOW); // FULL ALARM
    if (currentMillis - lastCritSmsTime > 300000) {
      sendSMS("RIVERGUARD CRITICAL: Flash flood! Evacuate!"); lastCritSmsTime = currentMillis;
    }
  }
}

void updateGlobalThreatLevel() {
  int highestThreat = 1;
  for (int i = 1; i <= 5; i++) { 
    long d = latestDistances[i];
    if (d < 180 && d > 0) highestThreat = 3; 
    else if (d >= 180 && d < 300 && highestThreat < 3) highestThreat = 2; 
  }
  globalMode = highestThreat;
}

void sendSMS(String text) {
  sim800.println("AT+CMGS=\"" + emergencyNumber + "\""); delay(200);
  sim800.print(text); delay(200);
  sim800.write(26); // Send
}