/*
 * Smart Irrigation System - ESP32 Firmware
 *
 * Sensors:
 *   - DHT22: Temperature & Humidity (GPIO 4)
 *   - LDR: Light intensity (GPIO 34, analog)
 *   - Soil Moisture Sensor (GPIO 35, analog)
 *
 * Actuators:
 *   - Water Valve via Relay (GPIO 26)
 *
 * Communication:
 *   - WiFi + MQTT for real-time data publishing and threshold config
 *
 * Author: Smart Irrigation Project
 * Date: 2026
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ==================== PIN DEFINITIONS ====================
#define DHT_PIN          4
#define DHT_TYPE         DHT22
#define LDR_PIN          34
#define SOIL_MOISTURE_PIN 35
#define VALVE_RELAY_PIN  26
#define STATUS_LED_PIN   2   // Built-in LED

// ==================== WiFi CONFIGURATION ====================
const char* WIFI_SSID     = "jooo";
const char* WIFI_PASSWORD = "00000000";

// ==================== MQTT CONFIGURATION ====================
const char* MQTT_SERVER   = "192.168.0.4";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "youssefirrgationsystem";       // Leave empty if no auth
const char* MQTT_PASSWORD = "00000000jO";       // Leave empty if no auth
const char* DEVICE_ID     = "esp32_irrigation_01";

// MQTT Topics
#define TOPIC_SENSOR_DATA    "irrigation/sensors"
#define TOPIC_VALVE_STATUS   "irrigation/valve/status"
#define TOPIC_VALVE_COMMAND  "irrigation/valve/command"
#define TOPIC_THRESHOLDS     "irrigation/thresholds"
#define TOPIC_SYSTEM_STATUS  "irrigation/system/status"
#define TOPIC_LOG            "irrigation/log"

// ==================== DEFAULT THRESHOLDS ====================
struct IrrigationThresholds {
  float soilMoistureMin;      // Below this → water needed (%)
  float soilMoistureMax;      // Above this → stop watering (%)
  float temperatureMax;       // Don't water above this temp (°C)
  float temperatureMin;       // Don't water below this temp (°C)
  float humidityMin;          // Low humidity increases watering need (%)
  int   lightThreshold;       // Light level threshold (0-100%)
  bool  waterAtNight;         // Allow watering at night
  unsigned long wateringDuration; // Max watering duration (ms)
  unsigned long cooldownPeriod;   // Min time between watering sessions (ms)
};

IrrigationThresholds thresholds = {
  .soilMoistureMin    = 30.0,
  .soilMoistureMax    = 70.0,
  .temperatureMax     = 40.0,
  .temperatureMin     = 5.0,
  .humidityMin        = 20.0,
  .lightThreshold     = 50,
  .waterAtNight       = true,
  .wateringDuration   = 300000,   // 5 minutes max
  .cooldownPeriod     = 1800000   // 30 minutes cooldown
};

// ==================== GLOBAL OBJECTS ====================
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

// ==================== SENSOR DATA STRUCT ====================
struct SensorData {
  float temperature;
  float humidity;
  float soilMoisture;
  int   lightLevel;
  bool  valveOpen;
};

SensorData currentData;

// ==================== TIMING ====================
unsigned long lastSensorRead     = 0;
unsigned long lastMqttPublish    = 0;
unsigned long lastWateringStart  = 0;
unsigned long wateringStartTime  = 0;
unsigned long lastWifiCheck      = 0;

const unsigned long SENSOR_READ_INTERVAL  = 2000;    // 2 seconds
const unsigned long MQTT_PUBLISH_INTERVAL = 5000;    // 5 seconds
const unsigned long WIFI_CHECK_INTERVAL   = 30000;   // 30 seconds

// ==================== STATE ====================
bool valveIsOpen       = false;
bool manualOverride    = false;
bool systemEnabled     = true;
int  wifiRetryCount    = 0;
const int MAX_WIFI_RETRIES = 20;

// ==================== FUNCTION DECLARATIONS ====================
void setupWiFi();
void setupMQTT();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void readSensors();
float readSoilMoisture();
int readLightLevel();
void evaluateIrrigation();
void openValve();
void closeValve();
void publishSensorData();
void publishValveStatus();
void publishSystemStatus();
void publishLog(const char* message);
void handleThresholdUpdate(const char* json);
void saveThresholds();
void loadThresholds();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  Smart Irrigation System - Starting");
  Serial.println("========================================\n");

  // Initialize pins
  pinMode(VALVE_RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(VALVE_RELAY_PIN, LOW);  // Valve closed by default
  digitalWrite(STATUS_LED_PIN, LOW);

  // Initialize DHT sensor
  dht.begin();

  // Load saved thresholds from flash
  loadThresholds();

  // Connect to WiFi
  setupWiFi();

  // Setup MQTT
  setupMQTT();

  Serial.println("[SYSTEM] Initialization complete.\n");
  publishLog("System started successfully");
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long now = millis();

  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Periodic WiFi check
  if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Connection lost. Reconnecting...");
      setupWiFi();
    }
  }

  // Read sensors at interval
  if (now - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = now;
    readSensors();

    // Evaluate automatic irrigation logic
    if (systemEnabled && !manualOverride) {
      evaluateIrrigation();
    }

    // Safety: enforce max watering duration
    if (valveIsOpen && (now - wateringStartTime >= thresholds.wateringDuration)) {
      Serial.println("[SAFETY] Maximum watering duration reached. Closing valve.");
      publishLog("Max watering duration reached - valve closed");
      closeValve();
    }
  }

  // Publish data to MQTT at interval
  if (now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL) {
    lastMqttPublish = now;
    publishSensorData();
    publishValveStatus();
  }
}

// ==================== WiFi ====================
void setupWiFi() {
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  wifiRetryCount = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetryCount < MAX_WIFI_RETRIES) {
    delay(500);
    Serial.print(".");
    wifiRetryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
    Serial.printf("[WIFI] IP Address: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    Serial.println(" FAILED!");
    Serial.println("[WIFI] Will retry in background.");
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

// ==================== MQTT ====================
void setupMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
}

void reconnectMQTT() {
  int retries = 0;
  while (!mqttClient.connected() && retries < 3) {
    Serial.println("[MQTT] Attempting connection...");
    String clientId = String(DEVICE_ID) + "_" + String(random(0xffff), HEX);

    bool connected;
    if (strlen(MQTT_USER) > 0) {
      connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
    } else {
      connected = mqttClient.connect(clientId.c_str());
    }

    if (connected) {
      Serial.println("[MQTT] Connected!");
      // Subscribe to control topics
      mqttClient.subscribe(TOPIC_VALVE_COMMAND);
      mqttClient.subscribe(TOPIC_THRESHOLDS);
      // Publish online status
      publishSystemStatus();
    } else {
      Serial.printf("[MQTT] Failed, rc=%d. Retrying in 2s...\n", mqttClient.state());
      delay(2000);
      retries++;
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  Serial.printf("[MQTT] Received on %s: %s\n", topic, message);

  if (strcmp(topic, TOPIC_VALVE_COMMAND) == 0) {
    // Manual valve control commands
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) return;

    const char* command = doc["command"];
    if (strcmp(command, "open") == 0) {
      manualOverride = true;
      openValve();
      publishLog("Manual override: valve opened");
    } else if (strcmp(command, "close") == 0) {
      manualOverride = true;
      closeValve();
      publishLog("Manual override: valve closed");
    } else if (strcmp(command, "auto") == 0) {
      manualOverride = false;
      publishLog("Switched to automatic mode");
    } else if (strcmp(command, "enable") == 0) {
      systemEnabled = true;
      publishLog("System enabled");
    } else if (strcmp(command, "disable") == 0) {
      systemEnabled = false;
      closeValve();
      publishLog("System disabled - valve closed");
    }
    publishValveStatus();
  }
  else if (strcmp(topic, TOPIC_THRESHOLDS) == 0) {
    handleThresholdUpdate(message);
  }
}

// ==================== SENSOR READING ====================
void readSensors() {
  // Read DHT22
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    currentData.temperature = t;
    currentData.humidity = h;
  } else {
    Serial.println("[SENSOR] DHT22 read failed!");
  }

  // Read soil moisture
  currentData.soilMoisture = readSoilMoisture();

  // Read light level
  currentData.lightLevel = readLightLevel();

  // Update valve state
  currentData.valveOpen = valveIsOpen;

  Serial.printf("[SENSOR] Temp: %.1f°C | Hum: %.1f%% | Soil: %.1f%% | Light: %d%%\n",
    currentData.temperature, currentData.humidity,
    currentData.soilMoisture, currentData.lightLevel);
}

float readSoilMoisture() {
  // Read multiple samples and average for stability
  long total = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    total += analogRead(SOIL_MOISTURE_PIN);
    delay(10);
  }
  int rawValue = total / samples;

  // Map raw ADC value (0-4095) to percentage (0-100%)
  // Calibration: 4095 = completely dry (0%), 0 = fully saturated (100%)
  // Adjust these values based on your specific sensor calibration
  float moisture = map(rawValue, 4095, 0, 0, 100);
  moisture = constrain(moisture, 0.0, 100.0);
  return moisture;
}

int readLightLevel() {
  long total = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    total += analogRead(LDR_PIN);
    delay(5);
  }
  int rawValue = total / samples;

  // Map to percentage: 0 = dark, 100 = bright
  // LDR with voltage divider: higher ADC = more light
  int light = map(rawValue, 0, 4095, 0, 100);
  light = constrain(light, 0, 100);
  return light;
}

// ==================== IRRIGATION LOGIC ====================
void evaluateIrrigation() {
  unsigned long now = millis();

  // Check cooldown period
  if (!valveIsOpen && (now - lastWateringStart < thresholds.cooldownPeriod) && lastWateringStart > 0) {
    return; // Still in cooldown
  }

  // Check if temperature is in safe range
  if (currentData.temperature > thresholds.temperatureMax ||
      currentData.temperature < thresholds.temperatureMin) {
    if (valveIsOpen) {
      Serial.println("[LOGIC] Temperature out of safe range. Closing valve.");
      publishLog("Temperature out of range - valve closed");
      closeValve();
    }
    return;
  }

  // Check light/night conditions
  bool isDark = (currentData.lightLevel < thresholds.lightThreshold);
  if (isDark && !thresholds.waterAtNight) {
    if (valveIsOpen) {
      closeValve();
      publishLog("Night detected, watering disabled at night - valve closed");
    }
    return;
  }

  // Core decision: soil moisture
  if (!valveIsOpen && currentData.soilMoisture < thresholds.soilMoistureMin) {
    // Soil is too dry → start watering
    Serial.printf("[LOGIC] Soil moisture (%.1f%%) below minimum (%.1f%%). Opening valve.\n",
      currentData.soilMoisture, thresholds.soilMoistureMin);

    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg),
      "Auto: Soil dry (%.1f%% < %.1f%%) - watering started",
      currentData.soilMoisture, thresholds.soilMoistureMin);
    publishLog(logMsg);
    openValve();
  }
  else if (valveIsOpen && currentData.soilMoisture >= thresholds.soilMoistureMax) {
    // Soil is sufficiently moist → stop watering
    Serial.printf("[LOGIC] Soil moisture (%.1f%%) reached target (%.1f%%). Closing valve.\n",
      currentData.soilMoisture, thresholds.soilMoistureMax);

    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg),
      "Auto: Soil moist enough (%.1f%% >= %.1f%%) - watering stopped",
      currentData.soilMoisture, thresholds.soilMoistureMax);
    publishLog(logMsg);
    closeValve();
  }
}

// ==================== VALVE CONTROL ====================
void openValve() {
  if (!valveIsOpen) {
    digitalWrite(VALVE_RELAY_PIN, HIGH);
    valveIsOpen = true;
    wateringStartTime = millis();
    lastWateringStart = millis();
    Serial.println("[VALVE] Opened");
    publishValveStatus();
  }
}

void closeValve() {
  if (valveIsOpen) {
    digitalWrite(VALVE_RELAY_PIN, LOW);
    valveIsOpen = false;
    Serial.println("[VALVE] Closed");
    publishValveStatus();
  }
}

// ==================== MQTT PUBLISHING ====================
void publishSensorData() {
  StaticJsonDocument<512> doc;
  doc["device_id"]      = DEVICE_ID;
  doc["temperature"]    = round(currentData.temperature * 10.0) / 10.0;
  doc["humidity"]       = round(currentData.humidity * 10.0) / 10.0;
  doc["soil_moisture"]  = round(currentData.soilMoisture * 10.0) / 10.0;
  doc["light_level"]    = currentData.lightLevel;
  doc["valve_open"]     = valveIsOpen;
  doc["manual_override"] = manualOverride;
  doc["system_enabled"]  = systemEnabled;
  doc["uptime_seconds"]  = millis() / 1000;

  char buffer[512];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_SENSOR_DATA, buffer, true);
}

void publishValveStatus() {
  StaticJsonDocument<256> doc;
  doc["device_id"]      = DEVICE_ID;
  doc["valve_open"]     = valveIsOpen;
  doc["manual_override"] = manualOverride;
  doc["system_enabled"]  = systemEnabled;

  if (valveIsOpen) {
    doc["watering_elapsed_sec"] = (millis() - wateringStartTime) / 1000;
  }

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_VALVE_STATUS, buffer, true);
}

void publishSystemStatus() {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["status"]    = "online";
  doc["ip"]        = WiFi.localIP().toString();
  doc["rssi"]      = WiFi.RSSI();

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_SYSTEM_STATUS, buffer, true);
}

void publishLog(const char* message) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["message"]   = message;
  doc["uptime"]    = millis() / 1000;

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(TOPIC_LOG, buffer);
}

// ==================== THRESHOLD MANAGEMENT ====================
void handleThresholdUpdate(const char* json) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.printf("[THRESHOLD] JSON parse error: %s\n", error.c_str());
    return;
  }

  if (doc.containsKey("soil_moisture_min"))    thresholds.soilMoistureMin    = doc["soil_moisture_min"];
  if (doc.containsKey("soil_moisture_max"))    thresholds.soilMoistureMax    = doc["soil_moisture_max"];
  if (doc.containsKey("temperature_max"))      thresholds.temperatureMax     = doc["temperature_max"];
  if (doc.containsKey("temperature_min"))      thresholds.temperatureMin     = doc["temperature_min"];
  if (doc.containsKey("humidity_min"))         thresholds.humidityMin        = doc["humidity_min"];
  if (doc.containsKey("light_threshold"))      thresholds.lightThreshold     = doc["light_threshold"];
  if (doc.containsKey("water_at_night"))       thresholds.waterAtNight       = doc["water_at_night"];
  if (doc.containsKey("watering_duration"))    thresholds.wateringDuration   = doc["watering_duration"];
  if (doc.containsKey("cooldown_period"))      thresholds.cooldownPeriod     = doc["cooldown_period"];

  saveThresholds();
  publishLog("Thresholds updated and saved");
  Serial.println("[THRESHOLD] Updated successfully.");
}

void saveThresholds() {
  preferences.begin("irrigation", false);
  preferences.putFloat("soilMin",      thresholds.soilMoistureMin);
  preferences.putFloat("soilMax",      thresholds.soilMoistureMax);
  preferences.putFloat("tempMax",      thresholds.temperatureMax);
  preferences.putFloat("tempMin",      thresholds.temperatureMin);
  preferences.putFloat("humMin",       thresholds.humidityMin);
  preferences.putInt("lightThr",       thresholds.lightThreshold);
  preferences.putBool("nightWater",    thresholds.waterAtNight);
  preferences.putULong("waterDur",     thresholds.wateringDuration);
  preferences.putULong("cooldown",     thresholds.cooldownPeriod);
  preferences.end();
}

void loadThresholds() {
  preferences.begin("irrigation", true);  // read-only
  if (preferences.isKey("soilMin")) {
    thresholds.soilMoistureMin  = preferences.getFloat("soilMin",   30.0);
    thresholds.soilMoistureMax  = preferences.getFloat("soilMax",   70.0);
    thresholds.temperatureMax   = preferences.getFloat("tempMax",   40.0);
    thresholds.temperatureMin   = preferences.getFloat("tempMin",    5.0);
    thresholds.humidityMin      = preferences.getFloat("humMin",    20.0);
    thresholds.lightThreshold   = preferences.getInt("lightThr",     50);
    thresholds.waterAtNight     = preferences.getBool("nightWater",  true);
    thresholds.wateringDuration = preferences.getULong("waterDur",  300000);
    thresholds.cooldownPeriod   = preferences.getULong("cooldown", 1800000);
    Serial.println("[FLASH] Thresholds loaded from memory.");
  } else {
    Serial.println("[FLASH] No saved thresholds. Using defaults.");
  }
  preferences.end();
}
