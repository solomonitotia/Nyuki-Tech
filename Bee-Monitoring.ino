// SIM800L BCD Protocol - Enhanced Version with FIXED Buffer Management
// Updated: August 2025
// Fix: Duplicate transmission and sleep condition issues resolved

#define SIM800L_IP5306_VERSION_20200811

#include "utilities.h"

// Select your modem
#define TINY_GSM_MODEM_SIM800

// Set Serial for Serial Monitor and AT Commands
#define SerialMonitor Serial
#define SerialAT Serial1

// Define the serial console for debug prints
#define TINY_GSM_DEBUG SerialMonitor

// Define how you're planning to connect to the internet
#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false

#include "esp_sleep.h"

// Battery monitoring configuration
#define BATTERY_ADC_PIN 35

// BCD DateTime Structure
struct BCDDateTime {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint8_t year;
};

// ✅ FORWARD DECLARATION - Declare struct before RTC variables
struct SensorPayload {
  uint32_t timestamp;
  uint32_t measured_time;     // NEW: When measurement was taken
  uint32_t transmitted_time;  // NEW: When payload was transmitted
  uint16_t payload_id;
  float temperature;
  float humidity;
  float battery_voltage;
  int battery_percentage;
  bool valid;
  bool transmitted;  // ✅ NEW: Track transmission status to prevent duplicates
};

int getBatteryPercentage(float voltage) {
  if (voltage >= 4.1) return 100;
  if (voltage >= 3.9) return 80;
  if (voltage >= 3.8) return 60;
  if (voltage >= 3.7) return 40;
  if (voltage >= 3.6) return 20;
  if (voltage >= 3.4) return 10;
  return 0;
}

// ✅ RTC MEMORY VARIABLES - These survive deep sleep
RTC_DATA_ATTR int rtc_buffer_index = 0;
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR uint16_t rtc_payload_id = 0;
RTC_DATA_ATTR bool rtc_first_boot = true;
// ✅ CRITICAL FIX: Store actual payload data in RTC memory
RTC_DATA_ATTR SensorPayload rtc_payload_buffer[8];  // Store up to 8 payloads in RTC

// GSM and MQTT Configuration
#define GSM_PIN ""
const char apn[] = "safaricom";
const char gprsUser[] = "saf";
const char gprsPass[] = "data";

const char* broker = "mqtt.wirelessplanet.co.ke";
const char* mqtt_user = "admin";
const char* mqtt_pass = "root";

const char *json_topic = "BeeHive Monitoring";
const char *bcd_topic_base = "beehive/frame";
const char* deviceName = "Bee-01"; 

// Timing Configuration
// #define DEVELOPMENT_MODE true  

#ifdef DEVELOPMENT_MODE
  const int MEASUREMENT_INTERVAL = 10;  // 10 seconds for development
  const int TRANSMISSION_INTERVAL = 60; // 1 minute for development
  const char* mode_name = "DEVELOPMENT";
#else
  const int MEASUREMENT_INTERVAL = 150;  // 15 minutes for production
  const int TRANSMISSION_INTERVAL = 1200; // 2 hours for production
  const char* mode_name = "PRODUCTION";
#endif

// BCD Protocol Configuration
#define FRAME_START 0x68
#define FRAME_END 0x16
#define DEVICE_TYPE 0x10
#define CONTROL_CODE_NORMAL 0x84

// Device Configuration
const uint32_t DEVICE_BASE_ID = 2300800;
const uint8_t DEVICE_NUMBER = 1;
const uint32_t ENCODED_DEVICE_ID = DEVICE_BASE_ID + DEVICE_NUMBER;

// ✅ FIXED: Use smaller buffer size that fits in RTC memory
#define PAYLOAD_BUFFER_SIZE 8

// Libraries
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <time.h>

// DHT Setup
#define DHTPIN 15
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient mqtt(client);

// ✅ FIXED: Global variables now use RTC data directly
int& bufferIndex = rtc_buffer_index;          // Reference to RTC memory
uint16_t& current_payload_id = rtc_payload_id; // Reference to RTC memory
SensorPayload* payloadBuffer = rtc_payload_buffer; // Point to RTC buffer

bool networkConnected = false;
bool timeIsSynced = false;
unsigned long timeOffset = 0;
unsigned long lastTimeSync = 0;
unsigned long lastMeasurement = 0;
unsigned long lastTransmission = 0;
const unsigned long TIME_SYNC_INTERVAL = 3600000;

// Function Prototypes
void setupModemAndNetwork();
void connectMQTT();
void takeMeasurement();
void transmitPayloads();
void enterDeepSleep();
bool syncPreciseNetworkTime();
void handleSerialCommands();
void displaySystemStatus();
void handleWakeUp();
bool shouldEnterDeepSleep();

// BCD Protocol Functions
uint8_t decimalToBCD(uint8_t decimal) {
  return ((decimal / 10) << 4) | (decimal % 10);
}

BCDDateTime getCurrentBCDTime() {
  time_t now;
  if (timeIsSynced) {
    now = (millis() / 1000) + timeOffset;
  } else {
    now = time(nullptr);
  }
  
  struct tm* timeinfo = localtime(&now);
  
  BCDDateTime bcdTime;
  bcdTime.second = decimalToBCD(timeinfo->tm_sec);
  bcdTime.minute = decimalToBCD(timeinfo->tm_min);
  bcdTime.hour = decimalToBCD(timeinfo->tm_hour);
  bcdTime.day = decimalToBCD(timeinfo->tm_mday);
  bcdTime.month = decimalToBCD(timeinfo->tm_mon + 1);
  bcdTime.year = decimalToBCD(timeinfo->tm_year % 100);
  
  return bcdTime;
}

bool syncPreciseNetworkTime() {
  SerialMonitor.println("Syncing time with cellular network...");
  
  modem.sendAT("+CLTS=1");
  if (modem.waitResponse(10000L) != 1) {
    SerialMonitor.println("Failed to enable network time sync");
    return false;
  }
  
  delay(2000);
  
  modem.sendAT("+CCLK?");
  String response = "";
  if (modem.waitResponse(10000L, response) == 1) {
    int startPos = response.indexOf('"');
    int endPos = response.indexOf('"', startPos + 1);
    
    if (startPos != -1 && endPos != -1) {
      String timeStr = response.substring(startPos + 1, endPos);
      SerialMonitor.print("Network time string: ");
      SerialMonitor.println(timeStr);
      
      if (timeStr.length() >= 17) {
        int year = 2000 + timeStr.substring(0, 2).toInt();
        int month = timeStr.substring(3, 5).toInt();
        int day = timeStr.substring(6, 8).toInt();
        int hour = timeStr.substring(9, 11).toInt();
        int minute = timeStr.substring(12, 14).toInt();
        int second = timeStr.substring(15, 17).toInt();
        
        struct tm networkTime;
        networkTime.tm_year = year - 1900;
        networkTime.tm_mon = month - 1;
        networkTime.tm_mday = day;
        networkTime.tm_hour = hour;
        networkTime.tm_min = minute;
        networkTime.tm_sec = second;
        networkTime.tm_isdst = -1;
        
        time_t networkEpoch = mktime(&networkTime);
        timeOffset = networkEpoch - (millis() / 1000);
        timeIsSynced = true;
        
        SerialMonitor.printf("Network time synced successfully!\n");
        SerialMonitor.printf("Network time: %04d-%02d-%02d %02d:%02d:%02d\n", 
                           year, month, day, hour, minute, second);
        SerialMonitor.printf("Unix timestamp: %lu\n", (unsigned long)networkEpoch);
        
        return true;
      }
    }
  }
  
  SerialMonitor.println("Failed to parse network time");
  return false;
}

uint8_t calculateChecksum(uint8_t* data, int length) {
  uint16_t sum = 0;
  for (int i = 0; i < length; i++) {
    sum += data[i];
  }
  return sum & 0xFF;
}

void createBCDProtocolFrame(SensorPayload& payload, uint8_t* frame, int& frameLength) {
  frameLength = 0;
  
  frame[frameLength++] = FRAME_START;
  frame[frameLength++] = DEVICE_TYPE;
  
  uint32_t deviceId = ENCODED_DEVICE_ID;
  uint8_t digits[7];
  for (int i = 6; i >= 0; i--) {
    digits[i] = deviceId % 10;
    deviceId /= 10;
  }
  
  frame[frameLength++] = (digits[6] << 4) | digits[5];
  frame[frameLength++] = (digits[4] << 4) | digits[3];
  frame[frameLength++] = (digits[2] << 4) | digits[1];
  frame[frameLength++] = (digits[0] << 4) | 0x0;
  frame[frameLength++] = 0x00;
  frame[frameLength++] = 0x00;
  frame[frameLength++] = 0x00;
  
  frame[frameLength++] = CONTROL_CODE_NORMAL;
  
  int dataLengthPos = frameLength++;
  int dataStart = frameLength;
  
  frame[frameLength++] = 0x90;
  frame[frameLength++] = 0x1F;
  frame[frameLength++] = payload.payload_id & 0xFF;
  
  uint16_t tempScaled = (uint16_t)(abs(payload.temperature) * 10);
  frame[frameLength++] = decimalToBCD(tempScaled % 100);
  frame[frameLength++] = decimalToBCD(tempScaled / 100);
  if (payload.temperature < 0) {
    frame[frameLength - 1] |= 0x80;
  }
  
  uint16_t humidScaled = (uint16_t)(payload.humidity * 10);
  frame[frameLength++] = decimalToBCD(humidScaled % 100);
  frame[frameLength++] = decimalToBCD(humidScaled / 100);
  
  // Battery voltage (scaled by 100)
  uint16_t voltageScaled = (uint16_t)(payload.battery_voltage * 100);
  frame[frameLength++] = decimalToBCD(voltageScaled % 100);
  frame[frameLength++] = decimalToBCD(voltageScaled / 100);
  
  // Battery percentage
  frame[frameLength++] = decimalToBCD(payload.battery_percentage);
  
  BCDDateTime bcdTime = getCurrentBCDTime();
  frame[frameLength++] = bcdTime.second;
  frame[frameLength++] = bcdTime.minute;
  frame[frameLength++] = bcdTime.hour;
  frame[frameLength++] = bcdTime.day;
  frame[frameLength++] = bcdTime.month;
  frame[frameLength++] = bcdTime.year;
  
  frame[frameLength++] = 0x00;
  frame[frameLength++] = 0xFF;
  
  frame[dataLengthPos] = frameLength - dataStart;
  
  uint8_t checksum = calculateChecksum(&frame[1], frameLength - 1);
  frame[frameLength++] = checksum;
  frame[frameLength++] = FRAME_END;
}

void displayBCDData(SensorPayload& payload) {
  SerialMonitor.println("=== BCD ENCODED DATA ===");
  SerialMonitor.printf("Payload ID: %d\n", payload.payload_id);
  SerialMonitor.printf("Temperature: %.1f°C\n", payload.temperature);
  SerialMonitor.printf("Humidity: %.1f%%\n", payload.humidity);
  SerialMonitor.printf("Battery Voltage: %.2fV\n", payload.battery_voltage);
  SerialMonitor.printf("Battery Percentage: %d%%\n", payload.battery_percentage);
  
  // Show timing information
  time_t measureTime = payload.measured_time;
  struct tm* measureTm = localtime(&measureTime);
  SerialMonitor.printf("Measured at: %04d-%02d-%02d %02d:%02d:%02d UTC (%lu)\n",
                measureTm->tm_year + 1900, measureTm->tm_mon + 1, measureTm->tm_mday,
                measureTm->tm_hour, measureTm->tm_min, measureTm->tm_sec, payload.measured_time);
  
  if (payload.transmitted_time > 0) {
    time_t transTime = payload.transmitted_time;
    struct tm* transTm = localtime(&transTime);
    SerialMonitor.printf("Transmitted at: %04d-%02d-%02d %02d:%02d:%02d UTC (%lu)\n",
                  transTm->tm_year + 1900, transTm->tm_mon + 1, transTm->tm_mday,
                  transTm->tm_hour, transTm->tm_min, transTm->tm_sec, payload.transmitted_time);
  } else {
    SerialMonitor.println("Transmitted at: Not yet transmitted");
  }
  
  SerialMonitor.printf("Transmitted: %s\n", payload.transmitted ? "YES" : "NO");
  SerialMonitor.println("========================");
}

// ✅ FIXED: Enhanced transmission status tracking with timestamp
bool transmitBCDFrame(SensorPayload& payload) {
  // Skip if already transmitted
  if (payload.transmitted) {
    SerialMonitor.printf("⚠ Frame ID %d already transmitted, skipping\n", payload.payload_id);
    return true; // Return true since it was already sent successfully
  }
  
  // Record transmission time
  uint32_t transmissionTime;
  if (timeIsSynced) {
    transmissionTime = (millis() / 1000) + timeOffset;
  } else {
    transmissionTime = time(nullptr);
  }
  payload.transmitted_time = transmissionTime;  // NEW: Record when transmitted
  
  SerialMonitor.printf("📤 Preparing to send Frame ID %d...\n", payload.payload_id);
  
  uint8_t protocolFrame[64];
  int frameLength;
  createBCDProtocolFrame(payload, protocolFrame, frameLength);
  
  String frameHex = "";
  for (int i = 0; i < frameLength; i++) {
    if (i > 0) frameHex += " ";
    char hex[3];
    sprintf(hex, "%02X", protocolFrame[i]);
    frameHex += hex;
  }
  
  // NEW: Extended payload format with timing data
  String bcdPayload = frameHex + "," + String(payload.payload_id) + "," + 
                      String(payload.measured_time) + "," + String(payload.transmitted_time) + "," +
                      String(payload.battery_voltage, 2) + "," + String(payload.battery_percentage);
  String topic = String(bcd_topic_base) + "/" + String(payload.payload_id);
  
  SerialMonitor.printf("📡 Publishing to topic: %s\n", topic.c_str());
  SerialMonitor.printf("📋 Payload length: %d bytes\n", bcdPayload.length());
  
  // Show timing information
  time_t measureTime = payload.measured_time;
  time_t transTime = payload.transmitted_time;
  struct tm* measureTm = localtime(&measureTime);
  struct tm* transTm = localtime(&transTime);
  
  SerialMonitor.printf("⏱ Measured: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                measureTm->tm_year + 1900, measureTm->tm_mon + 1, measureTm->tm_mday,
                measureTm->tm_hour, measureTm->tm_min, measureTm->tm_sec);
  SerialMonitor.printf("⏱ Transmitting: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                transTm->tm_year + 1900, transTm->tm_mon + 1, transTm->tm_mday,
                transTm->tm_hour, transTm->tm_min, transTm->tm_sec);
  SerialMonitor.printf("⏱ Delay: %lu seconds\n", payload.transmitted_time - payload.measured_time);
  
  // Add extra safety check before publishing
  if (!mqtt.connected()) {
    SerialMonitor.println("⚠ MQTT disconnected during transmission attempt");
    return false;
  }
  
  bool success = mqtt.publish(topic.c_str(), bcdPayload.c_str(), false);
  
  if (success) {
    payload.transmitted = true; // ✅ Mark as transmitted to prevent duplicates
    SerialMonitor.printf("✅ Frame ID %d transmitted successfully\n", payload.payload_id);
    
    // Additional verification - check MQTT state after publish
    if (mqtt.state() != 0) {
      SerialMonitor.printf("⚠ MQTT state changed to %d after publish\n", mqtt.state());
    }
    
    return true;
  } else {
    SerialMonitor.printf("❌ Frame ID %d transmission failed (MQTT state: %d)\n", 
                         payload.payload_id, mqtt.state());
    return false;
  }
}

void connectMQTT() {
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    SerialMonitor.print("Attempting MQTT connection...");
    
    String clientId = String(deviceName) + "_" + String(random(0xffff), HEX);
    bool connected = mqtt.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    
    if (connected) {
      SerialMonitor.println("MQTT connected successfully!");
      return;
    } else {
      SerialMonitor.print("failed, rc=");
      SerialMonitor.println(mqtt.state());
      delay(5000);
      attempts++;
    }
  }
}

// ✅ FIXED: Proper buffer overflow handling
void takeMeasurement() {
  SerialMonitor.println("=== TAKING MEASUREMENT ===");
  SerialMonitor.printf("Current buffer index: %d\n", bufferIndex);
  
  // ✅ FIXED: Buffer full condition - force immediate transmission
  if (bufferIndex >= PAYLOAD_BUFFER_SIZE) {
    SerialMonitor.println("⚠ Buffer full! Forcing immediate transmission...");
    if (networkConnected && mqtt.connected()) {
      transmitPayloads();
    } else {
      SerialMonitor.println("⚠ No network - dropping oldest payload to make room");
      // Shift buffer left to drop oldest payload
      for (int i = 0; i < PAYLOAD_BUFFER_SIZE - 1; i++) {
        rtc_payload_buffer[i] = rtc_payload_buffer[i + 1];
      }
      bufferIndex = PAYLOAD_BUFFER_SIZE - 1; // Reset to last slot
    }
  }
  
  // Create new payload
  SensorPayload& payload = rtc_payload_buffer[bufferIndex];
  
  uint32_t currentTime;
  if (timeIsSynced) {
    currentTime = (millis() / 1000) + timeOffset;
  } else {
    currentTime = time(nullptr);
  }
  
  payload.timestamp = currentTime;        // Legacy field for compatibility
  payload.measured_time = currentTime;    // NEW: Record measurement time
  payload.transmitted_time = 0;           // NEW: Will be set when transmitted
  payload.payload_id = current_payload_id++;
  payload.transmitted = false; // ✅ Initialize transmission status
  
  // Read sensor with retry
  float temp = dht.readTemperature();
  float humid = dht.readHumidity();
  
  if (isnan(temp) || isnan(humid)) {
    delay(2000);
    temp = dht.readTemperature();
    humid = dht.readHumidity();
  }
  
  // Read battery voltage and percentage
  int analogVal = analogRead(BATTERY_ADC_PIN);
  float inputVoltage = ((float(analogVal)/4096) * 3.3) * 2.068;
  int batteryPercent = getBatteryPercentage(inputVoltage);
  
  payload.temperature = temp;
  payload.humidity = humid;
  payload.battery_voltage = inputVoltage;
  payload.battery_percentage = batteryPercent;
  payload.valid = !isnan(payload.temperature) && !isnan(payload.humidity) &&
                  payload.temperature > -40 && payload.temperature < 80 &&
                  payload.humidity >= 0 && payload.humidity <= 100 &&
                  payload.battery_voltage > 0 && payload.battery_voltage < 5.0;
  
  if (payload.valid) {
    bufferIndex++; // ✅ Increment AFTER storing data
    
    SerialMonitor.printf("✓ Measurement %d stored: T=%.1f°C, H=%.1f%%, V=%.2fV, B=%d%%, Buffer: %d/%d\n",
                  payload.payload_id, payload.temperature, payload.humidity, 
                  payload.battery_voltage, payload.battery_percentage,
                  bufferIndex, PAYLOAD_BUFFER_SIZE);
    
    // Show measurement timestamp
    time_t measureTime = payload.measured_time;
    struct tm* timeinfo = localtime(&measureTime);
    SerialMonitor.printf("✓ Measured at: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    displayBCDData(payload);
  } else {
    SerialMonitor.println("✗ Invalid sensor readings! Skipping measurement.");
    SerialMonitor.printf("Raw values - Temp: %.2f, Humidity: %.2f, Voltage: %.2f\n", temp, humid, inputVoltage);
  }
}

// ✅ FIXED: Prevent duplicate transmissions with static flag
static bool transmissionInProgress = false;

void transmitPayloads() {
  // ✅ CRITICAL: Prevent re-entry during transmission
  if (transmissionInProgress) {
    SerialMonitor.println("⚠ Transmission already in progress, skipping duplicate call");
    return;
  }
  
  transmissionInProgress = true; // Set flag to prevent re-entry
  
  SerialMonitor.println("=== TRANSMISSION STARTING ===");
  SerialMonitor.printf("Transmission flag set - preventing duplicates\n");
  
  if (!mqtt.connected()) {
    SerialMonitor.println("MQTT not connected, attempting to connect...");
    connectMQTT();
  }
  
  if (!mqtt.connected()) {
    SerialMonitor.println("MQTT connection failed, skipping transmission");
    transmissionInProgress = false; // Clear flag before exit
    return;
  }
  
  SerialMonitor.printf("Checking %d payloads in buffer for transmission...\n", bufferIndex);
  
  int transmitted = 0;
  int skipped = 0;
  int alreadySent = 0;
  
  for (int i = 0; i < bufferIndex; i++) {
    if (rtc_payload_buffer[i].valid) {
      if (rtc_payload_buffer[i].transmitted) {
        alreadySent++;
        SerialMonitor.printf("⚠ Payload %d already transmitted, skipping\n", rtc_payload_buffer[i].payload_id);
      } else {
        SerialMonitor.printf("→ Transmitting payload %d...\n", rtc_payload_buffer[i].payload_id);
        if (transmitBCDFrame(rtc_payload_buffer[i])) {
          transmitted++;
        }
        delay(500); // Increased delay between transmissions
      }
    } else {
      skipped++;
    }
  }
  
  SerialMonitor.printf("✓ Transmission complete: %d transmitted, %d already sent, %d skipped\n", 
                transmitted, alreadySent, skipped);
  
  // ✅ FIXED: Only clear buffer if at least some payloads were transmitted
  if (transmitted > 0) {
    // Check if all valid payloads have been transmitted
    bool allTransmitted = true;
    for (int i = 0; i < bufferIndex; i++) {
      if (rtc_payload_buffer[i].valid && !rtc_payload_buffer[i].transmitted) {
        allTransmitted = false;
        break;
      }
    }
    
    if (allTransmitted) {
      bufferIndex = 0; // Clear buffer completely
      SerialMonitor.println("✓ All payloads transmitted - buffer cleared");
    } else {
      SerialMonitor.printf("⚠ Some payloads not transmitted - buffer retained (%d payloads)\n", bufferIndex);
    }
  }
  
  transmissionInProgress = false; // Clear flag when done
  SerialMonitor.println("✓ Transmission flag cleared");
}

void handleSerialCommands() {
  if (SerialMonitor.available()) {
    String command = SerialMonitor.readString();
    command.trim();
    
    if (command == "measure" || command == "m") {
      SerialMonitor.println("=== MANUAL MEASUREMENT ===");
      takeMeasurement();
    } else if (command == "transmit" || command == "t") {
      SerialMonitor.println("=== MANUAL TRANSMISSION ===");
      if (!networkConnected) setupModemAndNetwork();
      if (networkConnected) transmitPayloads();
    } else if (command == "status" || command == "s") {
      displaySystemStatus();
    } else if (command == "clear" || command == "c") {
      bufferIndex = 0;
      SerialMonitor.println("✓ Buffer cleared manually");
    } else if (command == "help" || command == "h") {
      SerialMonitor.println("=== AVAILABLE COMMANDS ===");
      SerialMonitor.println("m/measure  - Take measurement now");
      SerialMonitor.println("t/transmit - Transmit data now");
      SerialMonitor.println("s/status   - Show system status");
      SerialMonitor.println("c/clear    - Clear buffer");
      SerialMonitor.println("h/help     - Show this help");
    }
  }
}

void displaySystemStatus() {
  SerialMonitor.println("=== SYSTEM STATUS ===");
  SerialMonitor.printf("Mode: %s\n", mode_name);
  SerialMonitor.printf("Boot count: %d\n", bootCount);
  SerialMonitor.printf("Current payload ID: %d\n", current_payload_id);
  SerialMonitor.printf("Buffer: %d/%d payloads in RTC memory\n", bufferIndex, PAYLOAD_BUFFER_SIZE);
  SerialMonitor.printf("Network: %s\n", networkConnected ? "Connected" : "Disconnected");
  SerialMonitor.printf("MQTT: %s\n", mqtt.connected() ? "Connected" : "Disconnected");
  SerialMonitor.printf("Time synced: %s\n", timeIsSynced ? "YES" : "NO");
  
  // Show current battery status
  int analogVal = analogRead(BATTERY_ADC_PIN);
  float inputVoltage = ((float(analogVal)/4096) * 3.3) * 2.068;
  int batteryPercent = getBatteryPercentage(inputVoltage);
  SerialMonitor.printf("Battery: %.2fV (%d%%)\n", inputVoltage, batteryPercent);
  
  // Show stored payloads with transmission status
  if (bufferIndex > 0) {
    SerialMonitor.println("--- Stored Payloads ---");
    for (int i = 0; i < bufferIndex; i++) {
      time_t measureTime = rtc_payload_buffer[i].measured_time;
      struct tm* measureTm = localtime(&measureTime);
      
      SerialMonitor.printf("Payload %d: ID=%d, T=%.1f°C, H=%.1f%%, V=%.2fV, B=%d%%, Valid=%s, Sent=%s\n",
                    i, rtc_payload_buffer[i].payload_id,
                    rtc_payload_buffer[i].temperature,
                    rtc_payload_buffer[i].humidity,
                    rtc_payload_buffer[i].battery_voltage,
                    rtc_payload_buffer[i].battery_percentage,
                    rtc_payload_buffer[i].valid ? "YES" : "NO",
                    rtc_payload_buffer[i].transmitted ? "YES" : "NO");
      
      SerialMonitor.printf("  Measured: %04d-%02d-%02d %02d:%02d:%02d UTC",
                    measureTm->tm_year + 1900, measureTm->tm_mon + 1, measureTm->tm_mday,
                    measureTm->tm_hour, measureTm->tm_min, measureTm->tm_sec);
      
      if (rtc_payload_buffer[i].transmitted_time > 0) {
        time_t transTime = rtc_payload_buffer[i].transmitted_time;
        struct tm* transTm = localtime(&transTime);
        SerialMonitor.printf(", Transmitted: %04d-%02d-%02d %02d:%02d:%02d UTC",
                      transTm->tm_year + 1900, transTm->tm_mon + 1, transTm->tm_mday,
                      transTm->tm_hour, transTm->tm_min, transTm->tm_sec);
        SerialMonitor.printf(", Delay: %lus", rtc_payload_buffer[i].transmitted_time - rtc_payload_buffer[i].measured_time);
      } else {
        SerialMonitor.printf(", Transmitted: Not yet");
      }
      SerialMonitor.println();
    }
  }
  SerialMonitor.println("====================");
}

void enterDeepSleep() {
  SerialMonitor.println("=== PREPARING FOR DEEP SLEEP ===");
  
  unsigned long currentTime = millis();
  unsigned long timeToNextMeasurement = (MEASUREMENT_INTERVAL * 1000UL) - (currentTime - lastMeasurement);
  
  if (timeToNextMeasurement < 60000) {
    SerialMonitor.println("Next measurement too soon, skipping deep sleep");
    return;
  }
  
  unsigned long sleepTime = (timeToNextMeasurement - 30000) / 1000;
  
  if (sleepTime < 60) {
    SerialMonitor.println("Sleep time too short, skipping deep sleep");
    return;
  }
  
  SerialMonitor.printf("Will sleep for %lu seconds\n", sleepTime);
  SerialMonitor.printf("RTC buffer contains %d payloads (will survive sleep)\n", bufferIndex);
  
  // ✅ RTC data is automatically preserved - no need to save/restore
  rtc_first_boot = false; // Mark that we've been running
  
  if (mqtt.connected()) {
    SerialMonitor.println("Disconnecting MQTT...");
    mqtt.disconnect();
    delay(500);
  }
  
  SerialMonitor.println("Powering down modem...");
  modem.poweroff();
  delay(2000);
  
  digitalWrite(MODEM_POWER_ON, LOW);
  digitalWrite(LED_GPIO, LED_OFF);
  
  esp_sleep_enable_timer_wakeup(sleepTime * 1000000ULL);
  
  SerialMonitor.println("Entering deep sleep now...");
  SerialMonitor.flush();
  
  esp_deep_sleep_start();
}

void handleWakeUp() {
  ++bootCount;
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  SerialMonitor.println("=== WAKE UP EVENT ===");
  SerialMonitor.printf("Boot count: %d\n", bootCount);
  SerialMonitor.printf("RTC buffer index: %d\n", rtc_buffer_index);
  SerialMonitor.printf("RTC payload ID: %d\n", rtc_payload_id);
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER:
      SerialMonitor.println("Wake-up: Timer");
      break;
    default:
      SerialMonitor.printf("Wake-up: Not from deep sleep (reason: %d)\n", wakeup_reason);
      break;
  }
  
  // ✅ RTC variables are automatically preserved - just reset network flags
  networkConnected = false;
  timeIsSynced = false;
  
  SerialMonitor.printf("✓ Restored from RTC: %d payloads, next ID: %d\n", 
                bufferIndex, current_payload_id);
  SerialMonitor.println("=== WAKE UP COMPLETE ===");
}

// ✅ FIXED: Improved sleep condition logic
bool shouldEnterDeepSleep() {
  unsigned long currentTime = millis();
  unsigned long timeToNextMeasurement = (MEASUREMENT_INTERVAL * 1000UL) - (currentTime - lastMeasurement);
  unsigned long timeToNextTransmission = (TRANSMISSION_INTERVAL * 1000UL) - (currentTime - lastTransmission);
  
  #ifdef DEVELOPMENT_MODE
    return false; // No deep sleep in dev mode
  #endif
  
  // ✅ FIXED: Allow sleep when buffer has room OR when buffer is full but transmitted
  bool bufferHasRoom = (bufferIndex < PAYLOAD_BUFFER_SIZE);
  
  // Check if all payloads in buffer are transmitted
  bool allTransmitted = true;
  if (bufferIndex > 0) {
    for (int i = 0; i < bufferIndex; i++) {
      if (rtc_payload_buffer[i].valid && !rtc_payload_buffer[i].transmitted) {
        allTransmitted = false;
        break;
      }
    }
  }
  
  bool canSleep = (bufferHasRoom || allTransmitted) && 
                  (timeToNextMeasurement > 120000) && 
                  (timeToNextTransmission > 120000);
  
  if (!canSleep) {
    SerialMonitor.printf("Sleep blocked - Buffer: %d/%d, HasRoom: %s, AllSent: %s, NextMeasure: %lus, NextTrans: %lus\n",
                  bufferIndex, PAYLOAD_BUFFER_SIZE, 
                  bufferHasRoom ? "YES" : "NO",
                  allTransmitted ? "YES" : "NO",
                  timeToNextMeasurement/1000,
                  timeToNextTransmission/1000);
  }
  
  return canSleep;
}

void setupModemAndNetwork() {
  setupModem();
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(6000);

  SerialMonitor.println("Initializing modem...");
  modem.restart();

  String modemInfo = modem.getModemInfo();
  SerialMonitor.print("Modem Info: ");
  SerialMonitor.println(modemInfo);

  #if TINY_GSM_USE_GPRS
  if (GSM_PIN && modem.getSimStatus() != 3) {
      modem.simUnlock(GSM_PIN);
  }
  #endif

  SerialMonitor.print("Waiting for network...");
  if (!modem.waitForNetwork()) {
      SerialMonitor.println(" fail");
      delay(10000);
      return;
  }
  SerialMonitor.println(" success");

  if (modem.isNetworkConnected()) {
      SerialMonitor.println("Network connected");
      networkConnected = true;
  }
  
  SerialMonitor.print(F("Connecting to "));
  SerialMonitor.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
      SerialMonitor.println(" fail");
      delay(10000);
      return;
  }
  SerialMonitor.println(" success");

  if (modem.isGprsConnected()) {
      SerialMonitor.println("GPRS connected");
  }

  mqtt.setServer(broker, 1883);
  
  if (syncPreciseNetworkTime()) {
      SerialMonitor.println("Time synchronized with cellular network");
      lastTimeSync = millis();
  } else {
      SerialMonitor.println("Warning: Time sync failed");
  }
}

void setup() {
    SerialMonitor.begin(115200);
    delay(10);

    handleWakeUp();
    
    SerialMonitor.println("=== BEEHIVE MONITORING SYSTEM ===");
    SerialMonitor.printf("Mode: %s\n", mode_name);
    SerialMonitor.printf("Encoded Device ID: %lu\n", ENCODED_DEVICE_ID);
    SerialMonitor.printf("Using RTC memory buffer (survives deep sleep)\n");

    dht.begin();
    SerialMonitor.println("DHT sensor initialized");

    // Initialize basic time
    struct tm tm;
    tm.tm_year = 2025 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 16;
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = 0;
    time_t t = mktime(&tm);
    
    setupModemAndNetwork();
    if (networkConnected) {
        connectMQTT();
    }
    
    SerialMonitor.println("✓ System initialized successfully");
    
    unsigned long currentTime = millis();
    lastMeasurement = currentTime - (MEASUREMENT_INTERVAL * 1000UL) - 1000;
    lastTransmission = currentTime;
    
    SerialMonitor.println("=== SYSTEM READY ===");
    SerialMonitor.printf("Device: %s (ID: %lu)\n", deviceName, ENCODED_DEVICE_ID);
    SerialMonitor.printf("RTC Buffer: %d/%d payloads\n", bufferIndex, PAYLOAD_BUFFER_SIZE);
    SerialMonitor.println("Ready to send BCD data with battery monitoring via RTC buffer");
    SerialMonitor.println("================================");
}

void loop() {
    unsigned long currentTime = millis();

    // ✅ FIXED: Handle post-wake-up state properly
    static bool hasHandledWakeupInLoop = false;
    
    // Only run wake-up handling once per boot cycle in loop
    if (!hasHandledWakeupInLoop) {
        // Check if we need to re-establish connections after wake-up
        if (bootCount > 1 && !networkConnected) {
            SerialMonitor.println("=== POST-WAKE-UP RECONNECTION ===");
            setupModemAndNetwork();
            
            if (networkConnected && !mqtt.connected()) {
                connectMQTT();
            }
        }
        hasHandledWakeupInLoop = true;
    }

    // Check modem status (regular monitoring)
    if (!modem.isNetworkConnected() && networkConnected) {
        SerialMonitor.println("Network disconnected - attempting reconnection");
        networkConnected = false;
        setupModemAndNetwork();
        
        if (networkConnected && !mqtt.connected()) {
            connectMQTT();
        }
        return;
    }

    handleSerialCommands();
    
    // Debug output
    static unsigned long lastDebug = 0;
    #ifdef DEVELOPMENT_MODE
    unsigned long debugInterval = 5000;
    #else
    unsigned long debugInterval = 10000;
    #endif
    
    if (currentTime - lastDebug >= debugInterval) {
        SerialMonitor.printf("Running - Buffer: %d/%d, Next measurement: %lus\n", 
                      bufferIndex, PAYLOAD_BUFFER_SIZE,
                      ((MEASUREMENT_INTERVAL * 1000UL) - (currentTime - lastMeasurement)) / 1000);
        lastDebug = currentTime;
    }
    
    if (!mqtt.connected() && networkConnected) {
        connectMQTT();
        delay(100);
    }
    
    // ✅ FIXED: Measurement check with buffer overflow protection
    if (currentTime - lastMeasurement >= (MEASUREMENT_INTERVAL * 1000UL)) {
        SerialMonitor.println("=== MEASUREMENT TIME ===");
        takeMeasurement();
        lastMeasurement = currentTime;
    }
    
    // ✅ FIXED: Transmission check with duplicate prevention
    bool timeForTransmission = (currentTime - lastTransmission >= (TRANSMISSION_INTERVAL * 1000UL));
    bool bufferNeedsTransmission = (bufferIndex >= PAYLOAD_BUFFER_SIZE);
    
    // ✅ CRITICAL: Prevent rapid successive transmission calls
    static unsigned long lastTransmissionAttempt = 0;
    const unsigned long MIN_TRANSMISSION_GAP = 5000; // 5 seconds minimum between attempts
    
    if ((timeForTransmission || bufferNeedsTransmission) && 
        (currentTime - lastTransmissionAttempt >= MIN_TRANSMISSION_GAP)) {
        
        SerialMonitor.println("=== TRANSMISSION TIME ===");
        if (bufferNeedsTransmission) {
            SerialMonitor.println("Trigger: Buffer full");
        }
        if (timeForTransmission) {
            SerialMonitor.println("Trigger: Scheduled time");
        }
        
        lastTransmissionAttempt = currentTime; // Record attempt time
        
        if (!networkConnected) {
            SerialMonitor.println("Network not connected, attempting reconnection...");
            setupModemAndNetwork();
        }
        
        if (networkConnected && bufferIndex > 0) {
            // Double-check we have untransmitted payloads
            bool hasUntransmitted = false;
            for (int i = 0; i < bufferIndex; i++) {
                if (rtc_payload_buffer[i].valid && !rtc_payload_buffer[i].transmitted) {
                    hasUntransmitted = true;
                    break;
                }
            }
            
            if (hasUntransmitted) {
                SerialMonitor.println("📤 Starting transmission of untransmitted payloads...");
                transmitPayloads();
                lastTransmission = currentTime;
            } else {
                SerialMonitor.println("ℹ All payloads already transmitted, skipping");
            }
        } else if (bufferIndex >= PAYLOAD_BUFFER_SIZE) {
            SerialMonitor.println("⚠ Critical: Buffer full but no network connection!");
            // In critical situation, we might need to drop old data
            SerialMonitor.println("⚠ Dropping oldest payload to prevent system hang");
            for (int i = 0; i < PAYLOAD_BUFFER_SIZE - 1; i++) {
                rtc_payload_buffer[i] = rtc_payload_buffer[i + 1];
            }
            bufferIndex = PAYLOAD_BUFFER_SIZE - 1;
        }
    } else if (timeForTransmission || bufferNeedsTransmission) {
        // Log why transmission was skipped
        unsigned long timeUntilNext = MIN_TRANSMISSION_GAP - (currentTime - lastTransmissionAttempt);
        SerialMonitor.printf("⏸ Transmission blocked - %lu ms until next attempt allowed\n", timeUntilNext);
    }
    
    // Periodic time resync
    if (timeIsSynced && networkConnected && (currentTime - lastTimeSync >= TIME_SYNC_INTERVAL)) {
        SerialMonitor.println("=== PERIODIC TIME RESYNC ===");
        if (syncPreciseNetworkTime()) {
            lastTimeSync = currentTime;
        }
    }

    if (networkConnected && mqtt.connected()) {
        mqtt.loop();
    }
    
    delay(1000);
    
    // ✅ FIXED: Deep sleep decision with better logic
    if (shouldEnterDeepSleep()) {
        SerialMonitor.println("✓ Conditions met for deep sleep");
        enterDeepSleep();
    }
}