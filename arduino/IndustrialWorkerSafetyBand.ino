#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"

// FreeRTOS headers (natively included in ESP32 Arduino Core)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ======================== PIN DEFINITIONS ========================
#define SDA_PIN 22
#define SCL_PIN 23
#define ONE_WIRE_BUS 4
#define MQ135_PIN 34
#define BUZZER_PIN 25
#define MOTOR_PIN 26
#define LED_PIN 27
#define MPU 0x68

// ======================== CONFIGURATION ========================
// Configure these values before flashing.
// Use your computer's LAN IPv4 (e.g. 192.168.1.x), not localhost.
#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define MQTT_SERVER      "192.168.1.20"     // IP of Windows PC running Mosquitto
#define MQTT_PORT        1883
#define DEVICE_ID        "ESP32-001"

// MQTT Topic Hierarchy
// workband/telemetry/{device_id}  → periodic vitals   (QoS 0, high-throughput)
// workband/critical/{device_id}   → fall events + proof (QoS 1, guaranteed delivery)
// workband/status/{device_id}     → online/offline LWT  (QoS 1, retained)
#define TOPIC_TELEMETRY  "workband/telemetry/" DEVICE_ID
#define TOPIC_CRITICAL   "workband/critical/" DEVICE_ID
#define TOPIC_STATUS     "workband/status/" DEVICE_ID

// ======================== FALL DETECTION THRESHOLDS ========================
#define FREE_FALL_THRESHOLD   0.45   // G - below this = weightlessness
#define IMPACT_THRESHOLD      2.0    // G - above this = collision
#define FREE_FALL_TIMEOUT     600    // ms - max free-fall duration before rejection
#define POST_IMPACT_TIME      1500   // ms - observation window after impact
#define REST_MIN              0.70   // G - lower bound of stable rest
#define REST_MAX              1.35   // G - upper bound of stable rest
#define STABLE_SAMPLES_REQUIRED 20   // N samples in rest range to confirm
#define MAX_REST_VARIATION    0.30   // G - max variation during rest window

// ======================== SENSOR INTERVALS & THRESHOLDS ========================
#define MPU_INTERVAL          5      // ms - 200 Hz fall detection sampling
#define HEART_MIN             45     // BPM
#define HEART_MAX             130    // BPM
#define SPO2_MIN              90     // %
#define FINGER_IR_MIN         300    // Raw IR threshold for finger presence
#define FINGER_CONFIRM_SAMPLES 10   // Consecutive IR samples to confirm finger
#define TEMP_MAX              38.0   // °C
#define MQ_INTERVAL           500    // ms - air quality sampling interval
#define ALERT_DURATION        2000   // ms - buzzer/LED/motor duration
#define TELEMETRY_INTERVAL    2000   // ms - MQTT publish interval for vitals

// ======================== SENSOR OBJECTS ========================
MAX30105 particleSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ======================== MQTT CLIENT ========================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======================== HEART RATE / SPO2 STATE ========================
uint32_t irBuffer[100];
uint32_t redBuffer[100];
int maxSamples = 0;
int32_t spo2;
int8_t validSpO2;
int32_t heartRate;
int8_t validHeartRate;
float smoothBPM = 0;
float smoothSpO2 = 0;
int fingerSamples = 0;
bool fingerPresent = false;

// ======================== TIMING STATE ========================
unsigned long lastMPUCheck = 0;
unsigned long lastMQCheck = 0;
unsigned long lastTempRequest = 0;
bool temperatureRequested = false;

// ======================== FALL STATE MACHINE ========================
enum FallState { NORMAL, FREE_FALL, IMPACT };
FallState fallState = NORMAL;
unsigned long freeFallTime = 0;
unsigned long impactTime = 0;
int stableSamples = 0;
float postImpactMin = 10.0;
float postImpactMax = 0.0;
float peakImpactAcc = 0.0;

// ======================== ALERT STATE ========================
bool alertActive = false;
unsigned long alertStart = 0;

// ======================== TELEMETRY STATE ========================
// These are written by Core 1 (sensor loop) and read by Core 0 (MQTT task).
// Protected by a FreeRTOS mutex for thread safety.
volatile float telemetryHeartRate = NAN;
volatile float telemetrySpO2 = NAN;
volatile float telemetryTemperature = NAN;
volatile int telemetryAirADC = -1;
volatile float telemetryAirVoltage = NAN;
SemaphoreHandle_t telemetryMutex;

// ======================== FREERTOS IPC ========================
struct FallProofData {
    float peak_impact_g;
    int free_fall_duration_ms;
    float rest_variation_g;
    int stable_samples;
};

struct TelemetryPacket {
    bool is_critical;        // True = fall event, skip to front of queue
    bool fall_detected;
    float heart_rate;
    float spo2;
    float temperature;
    int air_quality_adc;
    float air_quality_voltage;
    FallProofData fall_proof;
};

QueueHandle_t telemetryQueue = NULL;
TaskHandle_t mqttTaskHandle = NULL;
unsigned long lastPostTime = 0;

// ======================== MPU6050 I2C HELPERS ========================
void writeMPU(byte reg, byte value) {
    Wire.beginTransmission(MPU);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

void readAcceleration(float &ax, float &ay, float &az) {
    Wire.beginTransmission(MPU);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU, 6);
    if (Wire.available() < 6) { ax = 0; ay = 0; az = 0; return; }
    int16_t axRaw = (Wire.read() << 8) | Wire.read();
    int16_t ayRaw = (Wire.read() << 8) | Wire.read();
    int16_t azRaw = (Wire.read() << 8) | Wire.read();
    ax = axRaw / 16384.0;
    ay = ayRaw / 16384.0;
    az = azRaw / 16384.0;
}

// ======================== ALERT ACTUATORS ========================
void resetFall() {
    fallState = NORMAL;
    stableSamples = 0;
    postImpactMin = 10.0;
    postImpactMax = 0.0;
    peakImpactAcc = 0.0;
}

void startAlert(const char* reason) {
    if (alertActive) return;
    alertActive = true;
    alertStart = millis();
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(MOTOR_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println();
    Serial.println("================================");
    Serial.print("ALERT: "); Serial.println(reason);
    Serial.println("================================");
}

void updateAlert() {
    if (!alertActive) return;
    if (millis() - alertStart >= ALERT_DURATION) {
        digitalWrite(LED_PIN, LOW);
        digitalWrite(MOTOR_PIN, LOW);
        digitalWrite(BUZZER_PIN, HIGH);
        alertActive = false;
        Serial.println("Alert ended\n");
    }
}

// ======================== QUEUE TELEMETRY TO CORE 0 ========================
void queueTelemetry(bool isCritical, bool fallDetected, FallProofData proof = {0, 0, 0, 0}) {
    if (!telemetryQueue) return;

    TelemetryPacket pkt;
    pkt.is_critical = isCritical;
    pkt.fall_detected = fallDetected;

    // Read shared telemetry state under mutex
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        pkt.heart_rate = telemetryHeartRate;
        pkt.spo2 = telemetrySpO2;
        pkt.temperature = telemetryTemperature;
        pkt.air_quality_adc = telemetryAirADC;
        pkt.air_quality_voltage = telemetryAirVoltage;
        xSemaphoreGive(telemetryMutex);
    }
    pkt.fall_proof = proof;

    // Critical messages bypass the normal queue and go to the front
    if (isCritical) {
        xQueueSendToFront(telemetryQueue, &pkt, 0);
    } else {
        xQueueSend(telemetryQueue, &pkt, 0);
    }
}

// ======================== FALL DETECTION STATE MACHINE ========================
// Runs at 200 Hz on Core 1. Never blocked by network operations.
void checkFall() {
    float ax, ay, az;
    readAcceleration(ax, ay, az);
    float total = sqrt(ax * ax + ay * ay + az * az);

    switch (fallState) {
        case NORMAL:
            if (total < FREE_FALL_THRESHOLD) {
                fallState = FREE_FALL;
                freeFallTime = millis();
                Serial.println("FREE FALL DETECTED!");
            }
            break;

        case FREE_FALL:
            if (millis() - freeFallTime > FREE_FALL_TIMEOUT) {
                Serial.println("Free fall rejected (timeout).");
                resetFall();
            } else if (total >= IMPACT_THRESHOLD) {
                fallState = IMPACT;
                impactTime = millis();
                peakImpactAcc = total;
                stableSamples = 0;
                postImpactMin = 10.0;
                postImpactMax = 0.0;
                Serial.printf("IMPACT DETECTED! Initial G: %.2f\n", total);
            }
            break;

        case IMPACT:
            if (total > peakImpactAcc) peakImpactAcc = total;

            if (total >= REST_MIN && total <= REST_MAX) {
                stableSamples++;
                if (total < postImpactMin) postImpactMin = total;
                if (total > postImpactMax) postImpactMax = total;
            } else {
                stableSamples = 0;
                postImpactMin = 10.0;
                postImpactMax = 0.0;
            }

            if (millis() - impactTime >= POST_IMPACT_TIME) {
                float variation = postImpactMax - postImpactMin;
                int freeFallDuration = impactTime - freeFallTime;

                Serial.printf("Stable: %d | Variation: %.2f G | Peak: %.2f G\n",
                              stableSamples, variation, peakImpactAcc);

                if (stableSamples >= STABLE_SAMPLES_REQUIRED && variation <= MAX_REST_VARIATION) {
                    Serial.println("\n********************************");
                    Serial.println("       FALL CONFIRMED           ");
                    Serial.println("********************************");

                    startAlert("FALL DETECTED - WORKER DOWN");

                    FallProofData proof;
                    proof.peak_impact_g = peakImpactAcc;
                    proof.free_fall_duration_ms = freeFallDuration;
                    proof.rest_variation_g = variation;
                    proof.stable_samples = stableSamples;

                    // CRITICAL: Dispatch immediately via MQTT QoS 1 on Core 0
                    queueTelemetry(true, true, proof);
                } else {
                    Serial.println("Impact rejected (insufficient rest stability).");
                }
                resetFall();
            }
            break;
    }
}

// ======================== MAX30102 PPG PROCESSING ========================
void checkMAX30102() {
    particleSensor.check();
    while (particleSensor.available()) {
        uint32_t ir = particleSensor.getIR();
        uint32_t red = particleSensor.getRed();
        particleSensor.nextSample();

        if (ir >= FINGER_IR_MIN) {
            if (fingerSamples < FINGER_CONFIRM_SAMPLES) fingerSamples++;
        } else {
            fingerSamples = 0;
            if (fingerPresent) Serial.println("Finger removed.");
            fingerPresent = false;
            maxSamples = 0;
            smoothBPM = 0;
            smoothSpO2 = 0;
            continue;
        }

        if (fingerSamples >= FINGER_CONFIRM_SAMPLES) {
            if (!fingerPresent) Serial.println("Finger detected.");
            fingerPresent = true;
        }
        if (!fingerPresent) continue;

        if (maxSamples < 100) {
            redBuffer[maxSamples] = red;
            irBuffer[maxSamples] = ir;
            maxSamples++;
        }
    }

    if (!fingerPresent) {
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            telemetryHeartRate = NAN;
            telemetrySpO2 = NAN;
            xSemaphoreGive(telemetryMutex);
        }
        return;
    }
    if (maxSamples < 100) return;

    maxim_heart_rate_and_oxygen_saturation(
        irBuffer, 100, redBuffer,
        &spo2, &validSpO2, &heartRate, &validHeartRate
    );

    if (validHeartRate && heartRate >= 40 && heartRate <= 180) {
        smoothBPM = (smoothBPM == 0) ? heartRate : 0.8 * smoothBPM + 0.2 * heartRate;
    }
    if (validSpO2 && spo2 >= 80 && spo2 <= 100) {
        smoothSpO2 = (smoothSpO2 == 0) ? spo2 : 0.8 * smoothSpO2 + 0.2 * spo2;
    }

    Serial.printf("Heart Rate: %.1f BPM | SpO2: %.1f%%\n",
                  smoothBPM > 0 ? smoothBPM : 0, smoothSpO2 > 0 ? smoothSpO2 : 0);

    // Update shared telemetry state
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        telemetryHeartRate = smoothBPM > 0 ? smoothBPM : NAN;
        telemetrySpO2 = smoothSpO2 > 0 ? smoothSpO2 : NAN;
        xSemaphoreGive(telemetryMutex);
    }

    // Check for critical biometric thresholds
    bool isAnomaly = false;
    if (smoothBPM > 0) {
        if (smoothBPM < HEART_MIN)      { startAlert("LOW HEART RATE");  isAnomaly = true; }
        else if (smoothBPM > HEART_MAX) { startAlert("HIGH HEART RATE"); isAnomaly = true; }
    }
    if (smoothSpO2 > 0 && smoothSpO2 < SPO2_MIN) {
        startAlert("LOW SpO2");
        isAnomaly = true;
    }
    if (isAnomaly) queueTelemetry(true, false);

    maxSamples = 0;
}

// ======================== DS18B20 TEMPERATURE ========================
void checkTemperature() {
    unsigned long now = millis();
    if (!temperatureRequested) {
        sensors.requestTemperatures();
        temperatureRequested = true;
        lastTempRequest = now;
        return;
    }
    if (now - lastTempRequest >= 800) {
        float temperature = sensors.getTempCByIndex(0);
        if (temperature != DEVICE_DISCONNECTED_C && temperature > -55 && temperature < 125 && temperature != 85.0) {
            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                telemetryTemperature = temperature;
                xSemaphoreGive(telemetryMutex);
            }
        }
        Serial.printf("Temperature: %.2f°C\n", temperature);
        if (temperature > TEMP_MAX) {
            startAlert("HIGH TEMPERATURE");
            queueTelemetry(true, false);
        }
        temperatureRequested = false;
    }
}

// ======================== MQ-135 AIR QUALITY ========================
void checkMQ135() {
    int value = analogRead(MQ135_PIN);
    float voltage = value * (3.3 / 4095.0);
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        telemetryAirADC = value;
        telemetryAirVoltage = voltage;
        xSemaphoreGive(telemetryMutex);
    }
    Serial.printf("MQ-135 ADC: %d | Voltage: %.2fV\n", value, voltage);
}

// ======================== MQTT RECONNECT WITH LWT ========================
void mqttReconnect() {
    while (!mqtt.connected()) {
        Serial.print("[Core 0] Connecting MQTT to ");
        Serial.print(MQTT_SERVER);
        Serial.print(":");
        Serial.print(MQTT_PORT);
        Serial.println("...");

        // Last Will & Testament: Broker auto-publishes "offline" if ESP32 drops ungracefully
        String lwt = "{\"status\":\"offline\",\"device_id\":\"" DEVICE_ID "\"}";

        if (mqtt.connect(DEVICE_ID, NULL, NULL, TOPIC_STATUS, 1, true, lwt.c_str())) {
            Serial.println("[Core 0] MQTT CONNECTED!");

            // Publish retained "online" status
            String onlineMsg = "{\"status\":\"online\",\"device_id\":\"" DEVICE_ID "\"}";
            mqtt.publish(TOPIC_STATUS, onlineMsg.c_str(), true);

            Serial.println("[Core 0] Published online status (retained, QoS 0).");
        } else {
            Serial.printf("[Core 0] MQTT connect failed, rc=%d. Retrying in 3s...\n", mqtt.state());
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

// ======================== CORE 0: MQTT TELEMETRY TASK ========================
// Runs entirely on Core 0. Handles Wi-Fi, MQTT connection, and message publishing.
// Core 1 (loop()) handles all sensors and actuators without any network stalls.
void mqttTask(void *pvParameters) {
    Serial.println("[Core 0] MQTT Telemetry Task started.");

    // Initialize Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[Core 0] Connecting Wi-Fi");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[Core 0] Wi-Fi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[Core 0] Wi-Fi initial connect failed. Will retry in loop.");
    }

    // Configure MQTT client
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setBufferSize(1024);  // Increase for JSON payloads

    TelemetryPacket pkt;

    for (;;) {
        // Ensure Wi-Fi is connected
        if (WiFi.status() != WL_CONNECTED) {
            static unsigned long lastWiFiRetry = 0;
            unsigned long now = millis();
            if (now - lastWiFiRetry >= 5000) {
                lastWiFiRetry = now;
                Serial.println("[Core 0] Wi-Fi disconnected. Reconnecting...");
                WiFi.reconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Ensure MQTT is connected
        if (!mqtt.connected()) {
            mqttReconnect();
        }
        mqtt.loop();  // Process MQTT keepalive and incoming messages

        // Check queue for outgoing telemetry packets (block up to 100ms)
        if (xQueueReceive(telemetryQueue, &pkt, pdMS_TO_TICKS(100)) == pdPASS) {
            // Build JSON payload
            String payload = "{\"device_id\":\"" DEVICE_ID "\"";
            if (!isnan(pkt.heart_rate))       payload += ",\"heart_rate\":"       + String(pkt.heart_rate, 1);
            if (!isnan(pkt.spo2))             payload += ",\"spo2\":"             + String(pkt.spo2, 1);
            if (!isnan(pkt.temperature))      payload += ",\"temperature\":"      + String(pkt.temperature, 2);
            if (pkt.air_quality_adc >= 0)     payload += ",\"air_quality_adc\":"  + String(pkt.air_quality_adc);
            if (!isnan(pkt.air_quality_voltage)) payload += ",\"air_quality_voltage\":" + String(pkt.air_quality_voltage, 2);
            payload += String(",\"fall_detected\":") + (pkt.fall_detected ? "true" : "false");

            // Append kinematic proof data for confirmed falls
            if (pkt.fall_detected) {
                payload += ",\"fall_proof\":{";
                payload += "\"peak_impact_g\":" + String(pkt.fall_proof.peak_impact_g, 2) + ",";
                payload += "\"free_fall_duration_ms\":" + String(pkt.fall_proof.free_fall_duration_ms) + ",";
                payload += "\"rest_variation_g\":" + String(pkt.fall_proof.rest_variation_g, 2) + ",";
                payload += "\"stable_samples\":" + String(pkt.fall_proof.stable_samples);
                payload += "}";
            }
            payload += "}";

            // Choose topic and QoS based on criticality
            const char* topic = pkt.is_critical ? TOPIC_CRITICAL : TOPIC_TELEMETRY;

            bool ok = mqtt.publish(topic, payload.c_str());
            if (ok) {
                Serial.printf("[Core 0] MQTT PUB %s (%d bytes)%s\n",
                              topic, payload.length(),
                              pkt.is_critical ? " [CRITICAL!]" : "");
            } else {
                Serial.printf("[Core 0] MQTT PUB FAILED on %s\n", topic);
            }

            // Retry critical messages if initial publish failed
            if (pkt.is_critical && !ok) {
                vTaskDelay(pdMS_TO_TICKS(200));
                mqtt.publish(topic, payload.c_str());
                Serial.println("[Core 0] Critical message retried.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ======================== SETUP ========================
void setup() {
    Serial.begin(115200);

    // Actuator pins
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(MOTOR_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(MQ135_PIN, INPUT);
    digitalWrite(BUZZER_PIN, HIGH);  // Buzzer off (active low)
    digitalWrite(MOTOR_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    // I2C bus at 400 kHz
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

    // MPU6050 configuration
    writeMPU(0x6B, 0x00);  // Wake up
    writeMPU(0x1C, 0x00);  // ±2g accelerometer
    writeMPU(0x1B, 0x00);  // ±250°/s gyroscope
    writeMPU(0x1A, 0x03);  // DLPF ~43 Hz bandwidth
    writeMPU(0x19, 0x04);  // 200 Hz sample rate

    // DS18B20 temperature (non-blocking mode)
    sensors.begin();
    sensors.setWaitForConversion(false);

    // MAX30102 PPG sensor
    if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("MAX30102 NOT FOUND!");
        while (1) { delay(100); }
    }
    particleSensor.setup(100, 1, 2, 100, 411);
    particleSensor.setPulseAmplitudeRed(0x7F);
    particleSensor.setPulseAmplitudeIR(0x7F);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println("   WORKER SAFETY BAND - MQTT + FreeRTOS ARCHITECTURE");
    Serial.println("=====================================================");
    Serial.println("MPU6050  : 200 Hz Tri-Axial Fall Detection (Core 1)");
    Serial.println("MAX30102 : Dual PPG Heart Rate & SpO2 (Core 1)");
    Serial.println("DS18B20  : Async Skin Temperature (Core 1)");
    Serial.println("MQ-135   : Gas/Air Quality ADC (Core 1)");
    Serial.println("MQTT     : PubSubClient on Core 0 (Non-blocking)");
    Serial.println("LWT      : Auto offline detection by broker");
    Serial.println("=====================================================");
    Serial.printf("MQTT Broker: %s:%d\n", MQTT_SERVER, MQTT_PORT);
    Serial.printf("Telemetry:   %s\n", TOPIC_TELEMETRY);
    Serial.printf("Critical:    %s\n", TOPIC_CRITICAL);
    Serial.printf("Status/LWT:  %s\n", TOPIC_STATUS);
    Serial.println("=====================================================\n");

    // Create FreeRTOS synchronization primitives
    telemetryMutex = xSemaphoreCreateMutex();
    telemetryQueue = xQueueCreate(8, sizeof(TelemetryPacket));

    // Spawn Core 0 task for MQTT (loop() runs on Core 1)
    xTaskCreatePinnedToCore(
        mqttTask,        // Function
        "MQTTTask",      // Name
        8192,            // Stack size
        NULL,            // Parameters
        1,               // Priority
        &mqttTaskHandle, // Handle
        0                // Pin to Core 0
    );
}

// ======================== LOOP (CORE 1 - SENSOR SAMPLING) ========================
// This loop runs entirely on Core 1. It is NEVER blocked by network operations.
// All network activity (Wi-Fi, MQTT) runs on Core 0 via the mqttTask.
void loop() {
    unsigned long now = millis();

    // 1. High-frequency fall detection (every 5 ms = 200 Hz)
    if (now - lastMPUCheck >= MPU_INTERVAL) {
        lastMPUCheck = now;
        checkFall();
    }

    // 2. Alert actuator state machine
    updateAlert();

    // 3. Heart rate & SpO2 PPG processing
    checkMAX30102();

    // 4. Air quality (every 500 ms)
    if (now - lastMQCheck >= MQ_INTERVAL) {
        lastMQCheck = now;
        checkMQ135();
    }

    // 5. Body temperature (async conversion)
    checkTemperature();

    // 6. Periodic telemetry publish (every 2 seconds)
    if (now - lastPostTime >= TELEMETRY_INTERVAL) {
        lastPostTime = now;
        queueTelemetry(false, false);
    }
}
