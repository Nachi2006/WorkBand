#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"

// FreeRTOS headers
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ======================== PIN DEFINITIONS ========================
#define SDA_PIN       22
#define SCL_PIN       23
#define ONE_WIRE_BUS  4
#define MQ135_PIN     34
#define BUZZER_PIN    25
#define MOTOR_PIN     26
#define LED_PIN       27
#define MPU           0x68

// ======================== CONFIGURATION ========================
#define WIFI_SSID        "POCO C65"
#define WIFI_PASSWORD    "Nachi@2006"
#define MQTT_SERVER      "192.68.56.1"     // Your PC's Wi-Fi IPv4
#define MQTT_PORT        1883
#define DEVICE_ID        "ESP32-001"

// MQTT Topic Hierarchy
#define TOPIC_TELEMETRY  "workband/telemetry/" DEVICE_ID
#define TOPIC_CRITICAL   "workband/critical/" DEVICE_ID
#define TOPIC_STATUS     "workband/status/" DEVICE_ID
#define TOPIC_COMMAND    "workband/command/" DEVICE_ID

// ======================== FALL DETECTION THRESHOLDS (±8G SCALE) ========================
#define MPU_SCALE_FACTOR        4096.0 // LSB/g for ±8g full-scale range (0x10)
#define FREE_FALL_THRESHOLD     0.50   // G - below this = weightlessness
#define IMPACT_THRESHOLD        2.20   // G - above this = collision (now accurately measured up to 8G!)
#define FREE_FALL_TIMEOUT       600    // ms - max free-fall duration before rejection
#define POST_IMPACT_TIME        1500   // ms - observation window after impact
#define REST_MIN                0.70   // G - lower bound of stable rest
#define REST_MAX                1.35   // G - upper bound of stable rest
#define STABLE_SAMPLES_REQUIRED 20     // N samples in rest range to confirm
#define MAX_REST_VARIATION      0.35   // G - max variation during rest window

// ======================== CALIBRATED SENSOR INTERVALS & THRESHOLDS ========================
#define MPU_INTERVAL            5      // ms - 200 Hz fall detection sampling
#define MAX30102_POLL_INTERVAL  20     // ms - Prevents I2C bus saturation while draining FIFO
#define HEART_MIN               45     // BPM
#define HEART_MAX               130    // BPM
#define SPO2_MIN                90     // %
#define FINGER_IR_MIN           45000  // Rejects ambient light, detects real skin contact
#define FINGER_CONFIRM_SAMPLES  12     // Consecutive IR samples to confirm contact
#define HR_ANOMALY_DEBOUNCE     3      // Consecutive bad HR/SpO2 calculations required before alert

#define TEMP_CONV_INTERVAL      250    // ms - Matches 10-bit DS18B20 conversion time (187.5ms)
#define TEMP_MAX                38.0   // °C - High skin/ambient temp alert threshold

#define MQ_INTERVAL             250    // ms - 4 Hz gas sampling
#define MQ_DELTA_THRESHOLD      600    // ADC rise above clean-air baseline to trigger gas alert
#define MQ_ABSOLUTE_MAX         1800   // Absolute ADC ceiling for toxic gas alert

#define ALERT_DURATION          2000   // ms - buzzer/LED/motor duration
#define ALERT_COOLDOWN          5000   // ms - minimum gap between non-fall alerts
#define TELEMETRY_INTERVAL      1000   // ms - 1 Hz MQTT publish interval
#define SERIAL_LOG_INTERVAL     1000   // ms - 1 Hz Serial Monitor summary

// ======================== SENSOR OBJECTS ========================
MAX30105 particleSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
bool max30102Available = false;

// ======================== MQTT CLIENT ========================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======================== HEART RATE / SPO2 STATE ========================
uint32_t irBuffer[100];
uint32_t redBuffer[100];
int bufferIndex = 0;
int32_t spo2;
int8_t validSpO2;
int32_t heartRate;
int8_t validHeartRate;
float smoothBPM = 0;
float smoothSpO2 = 0;
int fingerSamples = 0;
bool fingerPresent = false;
int hrAnomalyCount = 0;

// ======================== TEMPERATURE & MQ-135 STATE ========================
float smoothTemp = NAN;
int tempAnomalyCount = 0;
int mqBaselineADC = 100;
float smoothMQADC = 0;
int mqAnomalyCount = 0;

// ======================== TIMING STATE ========================
unsigned long lastMPUCheck = 0;
unsigned long lastMAXCheck = 0;
unsigned long lastMQCheck = 0;
unsigned long lastTempRequest = 0;
unsigned long lastPostTime = 0;
unsigned long lastSerialLog = 0;
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
volatile bool alertActive = false;
volatile unsigned long alertStart = 0;
unsigned long lastAlertEnd = 0;

// ======================== TELEMETRY STATE ========================
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
    bool is_critical;
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

// ======================== MPU6050 I2C HELPERS ========================
void writeMPU(byte reg, byte value) {
    Wire.beginTransmission(MPU);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

bool readAcceleration(float &ax, float &ay, float &az) {
    Wire.beginTransmission(MPU);
    Wire.write(0x3B);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)MPU, (uint8_t)6) != 6) return false;

    int16_t axRaw = (Wire.read() << 8) | Wire.read();
    int16_t ayRaw = (Wire.read() << 8) | Wire.read();
    int16_t azRaw = (Wire.read() << 8) | Wire.read();

    ax = axRaw / MPU_SCALE_FACTOR;
    ay = ayRaw / MPU_SCALE_FACTOR;
    az = azRaw / MPU_SCALE_FACTOR;
    return true;
}

// ======================== ALERT ACTUATORS ========================
void resetFall() {
    fallState = NORMAL;
    stableSamples = 0;
    postImpactMin = 10.0;
    postImpactMax = 0.0;
    peakImpactAcc = 0.0;
}

void startAlert(const char* reason, bool bypassCooldown = false) {
    if (alertActive) return;
    if (!bypassCooldown && (millis() - lastAlertEnd < ALERT_COOLDOWN)) return;

    alertActive = true;
    alertStart = millis();
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(MOTOR_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW); // Active-low buzzer ON
    Serial.println();
    Serial.println("================================");
    Serial.print("ALERT: "); Serial.println(reason);
    Serial.println("================================");
}

void stopAlertImmediate() {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(MOTOR_PIN, LOW);
    digitalWrite(BUZZER_PIN, HIGH); // Active-low buzzer OFF
    alertActive = false;
    lastAlertEnd = millis();
}

void updateAlert() {
    if (!alertActive) return;
    if (millis() - alertStart >= ALERT_DURATION) {
        stopAlertImmediate();
        Serial.println("Alert ended\n");
    }
}

// ======================== QUEUE TELEMETRY TO CORE 0 ========================
void queueTelemetry(bool isCritical, bool fallDetected, FallProofData proof = {0, 0, 0, 0}) {
    if (!telemetryQueue) return;

    TelemetryPacket pkt;
    pkt.is_critical = isCritical;
    pkt.fall_detected = fallDetected;

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        pkt.heart_rate = telemetryHeartRate;
        pkt.spo2 = telemetrySpO2;
        pkt.temperature = telemetryTemperature;
        pkt.air_quality_adc = telemetryAirADC;
        pkt.air_quality_voltage = telemetryAirVoltage;
        xSemaphoreGive(telemetryMutex);
    }
    pkt.fall_proof = proof;

    if (isCritical) {
        xQueueSendToFront(telemetryQueue, &pkt, 0);
    } else {
        xQueueSend(telemetryQueue, &pkt, 0);
    }
}

// ======================== FALL DETECTION STATE MACHINE ========================
void checkFall() {
    float ax, ay, az;
    if (!readAcceleration(ax, ay, az)) return; // Skip on transient I2C glitch

    float total = sqrt(ax * ax + ay * ay + az * az);

    switch (fallState) {
        case NORMAL:
            if (total < FREE_FALL_THRESHOLD) {
                fallState = FREE_FALL;
                freeFallTime = millis();
                Serial.printf("[MPU6050] Free fall started (%.2f G)\n", total);
            }
            break;

        case FREE_FALL:
            if (millis() - freeFallTime > FREE_FALL_TIMEOUT) {
                resetFall();
            } else if (total >= IMPACT_THRESHOLD) {
                fallState = IMPACT;
                impactTime = millis();
                peakImpactAcc = total;
                stableSamples = 0;
                postImpactMin = 10.0;
                postImpactMax = 0.0;
                Serial.printf("[MPU6050] Impact detected! Peak G: %.2f\n", total);
            }
            break;

        case IMPACT:
            if (total > peakImpactAcc) peakImpactAcc = total;

            if (total >= REST_MIN && total <= REST_MAX) {
                stableSamples++;
                if (total < postImpactMin) postImpactMin = total;
                if (total > postImpactMax) postImpactMax = total;
            } else if (total > IMPACT_THRESHOLD) {
                // Secondary bounce - reset post-impact rest counter
                stableSamples = 0;
                postImpactMin = 10.0;
                postImpactMax = 0.0;
            }

            if (millis() - impactTime >= POST_IMPACT_TIME) {
                float variation = (postImpactMax >= postImpactMin) ? (postImpactMax - postImpactMin) : 0.0;
                int freeFallDuration = impactTime - freeFallTime;

                Serial.printf("[MPU6050] Post-Impact Check -> Stable: %d | Var: %.2f G | Peak: %.2f G\n",
                              stableSamples, variation, peakImpactAcc);

                if (stableSamples >= STABLE_SAMPLES_REQUIRED && variation <= MAX_REST_VARIATION) {
                    Serial.println("\n********************************");
                    Serial.println("       FALL CONFIRMED           ");
                    Serial.println("********************************");

                    startAlert("FALL DETECTED - WORKER DOWN", true);

                    FallProofData proof = { peakImpactAcc, freeFallDuration, variation, stableSamples };
                    queueTelemetry(true, true, proof);
                } else {
                    Serial.println("[MPU6050] Impact rejected (worker still moving / upright).");
                }
                resetFall();
            }
            break;
    }
}

// ======================== CALIBRATED MAX30102 PPG PROCESSING ========================
void checkMAX30102() {
    if (!max30102Available) return;

    particleSensor.check();
    while (particleSensor.available()) {
        uint32_t ir = particleSensor.getIR();
        uint32_t red = particleSensor.getRed();
        particleSensor.nextSample();

        if (ir >= FINGER_IR_MIN) {
            if (fingerSamples < FINGER_CONFIRM_SAMPLES) fingerSamples++;
        } else {
            fingerSamples = 0;
            if (fingerPresent) Serial.println("[MAX30102] Sensor contact lost.");
            fingerPresent = false;
            bufferIndex = 0;
            smoothBPM = 0;
            smoothSpO2 = 0;
            hrAnomalyCount = 0;
            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                telemetryHeartRate = NAN;
                telemetrySpO2 = NAN;
                xSemaphoreGive(telemetryMutex);
            }
            continue;
        }

        if (fingerSamples >= FINGER_CONFIRM_SAMPLES && !fingerPresent) {
            Serial.println("[MAX30102] Skin contact confirmed — acquiring pulse...");
            fingerPresent = true;
        }
        if (!fingerPresent) continue;

        if (bufferIndex < 100) {
            redBuffer[bufferIndex] = red;
            irBuffer[bufferIndex] = ir;
            bufferIndex++;
        }
    }

    if (!fingerPresent || bufferIndex < 100) return;

    maxim_heart_rate_and_oxygen_saturation(
        irBuffer, 100, redBuffer,
        &spo2, &validSpO2, &heartRate, &validHeartRate
    );

    if (validHeartRate && heartRate >= 45 && heartRate <= 165) {
        if (smoothBPM == 0) {
            smoothBPM = heartRate;
        } else if (abs(heartRate - smoothBPM) <= 30) {
            smoothBPM = 0.75 * smoothBPM + 0.25 * heartRate;
        } else {
            smoothBPM = 0.90 * smoothBPM + 0.10 * heartRate;
        }
    }

    if (validSpO2 && spo2 >= 80 && spo2 <= 100) {
        smoothSpO2 = (smoothSpO2 == 0) ? spo2 : (0.80 * smoothSpO2 + 0.20 * spo2);
    }

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        telemetryHeartRate = smoothBPM > 0 ? smoothBPM : NAN;
        telemetrySpO2 = smoothSpO2 > 0 ? smoothSpO2 : NAN;
        xSemaphoreGive(telemetryMutex);
    }

    bool currentReadingBad = false;
    const char* alertReason = "";

    if (smoothBPM > 0) {
        if (smoothBPM < HEART_MIN)      { currentReadingBad = true; alertReason = "LOW HEART RATE"; }
        else if (smoothBPM > HEART_MAX) { currentReadingBad = true; alertReason = "HIGH HEART RATE"; }
    }
    if (smoothSpO2 > 0 && smoothSpO2 < SPO2_MIN) {
        currentReadingBad = true;
        alertReason = "LOW SpO2";
    }

    if (currentReadingBad) {
        hrAnomalyCount++;
        if (hrAnomalyCount >= HR_ANOMALY_DEBOUNCE) {
            startAlert(alertReason);
            queueTelemetry(true, false);
            hrAnomalyCount = 0;
        }
    } else {
        hrAnomalyCount = 0;
    }

    // Shift oldest 25 samples (1 second) out for 1 Hz sliding window updates
    for (int i = 25; i < 100; i++) {
        redBuffer[i - 25] = redBuffer[i];
        irBuffer[i - 25] = irBuffer[i];
    }
    bufferIndex = 75;
}

// ======================== DS18B20 TEMPERATURE (EMA FILTERED) ========================
void checkTemperature() {
    unsigned long now = millis();
    if (!temperatureRequested) {
        sensors.requestTemperatures();
        temperatureRequested = true;
        lastTempRequest = now;
        return;
    }
    if (now - lastTempRequest >= TEMP_CONV_INTERVAL) {
        float rawTemp = sensors.getTempCByIndex(0);
        // Filter out -127C (disconnected) and 85.0C (power-on reset value)
        if (rawTemp != DEVICE_DISCONNECTED_C && rawTemp > -20.0 && rawTemp < 75.0 && fabs(rawTemp - 85.0) > 0.1) {
            smoothTemp = isnan(smoothTemp) ? rawTemp : (0.70 * smoothTemp + 0.30 * rawTemp);

            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                telemetryTemperature = smoothTemp;
                xSemaphoreGive(telemetryMutex);
            }

            if (smoothTemp > TEMP_MAX) {
                tempAnomalyCount++;
                if (tempAnomalyCount >= 3) {
                    startAlert("HIGH TEMPERATURE");
                    queueTelemetry(true, false);
                    tempAnomalyCount = 0;
                }
            } else {
                tempAnomalyCount = 0;
            }
        }
        temperatureRequested = false;
    }
}

// ======================== MQ-135 GAS SENSOR (BASELINE + ALERT) ========================
void checkMQ135() {
    long sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(MQ135_PIN);
    }
    int rawADC = sum / 8;
    smoothMQADC = (smoothMQADC == 0) ? rawADC : (0.75 * smoothMQADC + 0.25 * rawADC);

    int calibratedADC = (int)round(smoothMQADC);
    float voltage = calibratedADC * (3.3 / 4095.0);

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        telemetryAirADC = calibratedADC;
        telemetryAirVoltage = voltage;
        xSemaphoreGive(telemetryMutex);
    }

    // Trigger gas alert if reading spikes above clean-air baseline or hits absolute ceiling
    if ((calibratedADC - mqBaselineADC >= MQ_DELTA_THRESHOLD) || (calibratedADC >= MQ_ABSOLUTE_MAX)) {
        mqAnomalyCount++;
        if (mqAnomalyCount >= 4) { // 1 full second of sustained gas spike
            startAlert("GAS / AIR QUALITY HAZARD");
            queueTelemetry(true, false);
            mqAnomalyCount = 0;
        }
    } else {
        mqAnomalyCount = 0;
    }
}

// ======================== UNIFIED SERIAL MONITOR LOGGER ========================
void logTelemetryToSerial() {
    float hr, sp, temp, mqV;
    int mqAdc;
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        hr = telemetryHeartRate;
        sp = telemetrySpO2;
        temp = telemetryTemperature;
        mqAdc = telemetryAirADC;
        mqV = telemetryAirVoltage;
        xSemaphoreGive(telemetryMutex);
    }
    Serial.printf("[VITALS] HR: %s BPM | SpO2: %s%% | Temp: %s°C | MQ-135: %d (%.2fV, Base:%d)\n",
                  isnan(hr)   ? "--" : String(hr, 1).c_str(),
                  isnan(sp)   ? "--" : String(sp, 1).c_str(),
                  isnan(temp) ? "--" : String(temp, 2).c_str(),
                  mqAdc,
                  isnan(mqV)  ? 0.0 : mqV,
                  mqBaselineADC);
}

// ======================== REMOTE MQTT COMMAND CALLBACK ========================
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.printf("[Core 0] Command received on %s: %s\n", topic, msg.c_str());

    if (msg.indexOf("stop") >= 0 || msg.indexOf("ack") >= 0 || msg.indexOf("off") >= 0) {
        stopAlertImmediate();
    } else if (msg.indexOf("alert") >= 0 || msg.indexOf("buzz") >= 0 || msg.indexOf("test") >= 0) {
        startAlert("REMOTE DASHBOARD ALERT", true);
    }
}

// ======================== MQTT RECONNECT WITH LWT ========================
void mqttReconnect() {
    while (!mqtt.connected()) {
        Serial.printf("[Core 0] Connecting MQTT to %s:%d...\n", MQTT_SERVER, MQTT_PORT);
        String lwt = "{\"status\":\"offline\",\"device_id\":\"" DEVICE_ID "\"}";

        if (mqtt.connect(DEVICE_ID, NULL, NULL, TOPIC_STATUS, 1, true, lwt.c_str())) {
            Serial.println("[Core 0] MQTT CONNECTED!");
            String onlineMsg = "{\"status\":\"online\",\"device_id\":\"" DEVICE_ID "\"}";
            mqtt.publish(TOPIC_STATUS, onlineMsg.c_str(), true);
            mqtt.subscribe(TOPIC_COMMAND);
        } else {
            Serial.printf("[Core 0] MQTT connect failed, rc=%d. Retrying in 2s...\n", mqtt.state());
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

// ======================== CORE 0: MQTT TELEMETRY TASK ========================
void mqttTask(void *pvParameters) {
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
    }

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(1024);

    TelemetryPacket pkt;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            static unsigned long lastWiFiRetry = 0;
            if (millis() - lastWiFiRetry >= 5000) {
                lastWiFiRetry = millis();
                WiFi.reconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!mqtt.connected()) {
            mqttReconnect();
        }
        mqtt.loop();

        while (xQueueReceive(telemetryQueue, &pkt, pdMS_TO_TICKS(20)) == pdPASS) {
            String payload = "{\"device_id\":\"" DEVICE_ID "\"";
            if (!isnan(pkt.heart_rate))          payload += ",\"heart_rate\":"          + String(pkt.heart_rate, 1);
            if (!isnan(pkt.spo2))                payload += ",\"spo2\":"                + String(pkt.spo2, 1);
            if (!isnan(pkt.temperature))         payload += ",\"temperature\":"         + String(pkt.temperature, 2);
            if (pkt.air_quality_adc >= 0)        payload += ",\"air_quality_adc\":"     + String(pkt.air_quality_adc);
            if (!isnan(pkt.air_quality_voltage)) payload += ",\"air_quality_voltage\":" + String(pkt.air_quality_voltage, 2);
            payload += String(",\"fall_detected\":") + (pkt.fall_detected ? "true" : "false");

            if (pkt.fall_detected) {
                payload += ",\"fall_proof\":{";
                payload += "\"peak_impact_g\":" + String(pkt.fall_proof.peak_impact_g, 2) + ",";
                payload += "\"free_fall_duration_ms\":" + String(pkt.fall_proof.free_fall_duration_ms) + ",";
                payload += "\"rest_variation_g\":" + String(pkt.fall_proof.rest_variation_g, 2) + ",";
                payload += "\"stable_samples\":" + String(pkt.fall_proof.stable_samples);
                payload += "}";
            }
            payload += "}";

            const char* topic = pkt.is_critical ? TOPIC_CRITICAL : TOPIC_TELEMETRY;
            bool ok = mqtt.publish(topic, payload.c_str());

            if (pkt.is_critical && !ok) {
                vTaskDelay(pdMS_TO_TICKS(100));
                mqtt.publish(topic, payload.c_str());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ======================== SETUP ========================
void setup() {
    Serial.begin(115200);

    // 1. Actuator pins + brief 150ms self-test on boot
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(MOTOR_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(MOTOR_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
    digitalWrite(LED_PIN, LOW);
    digitalWrite(MOTOR_PIN, LOW);
    digitalWrite(BUZZER_PIN, HIGH);

    // 2. Calibrate ESP32 ADC for MQ-135 (full 0 - 3.3V range) & sample baseline
    pinMode(MQ135_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(MQ135_PIN, ADC_11db);
    long mqInitSum = 0;
    for (int i = 0; i < 32; i++) {
        mqInitSum += analogRead(MQ135_PIN);
        delay(5);
    }
    mqBaselineADC = mqInitSum / 32;
    smoothMQADC = mqBaselineADC;

    // 3. I2C bus at 400 kHz with 50ms timeout protection
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);
    Wire.setTimeOut(50);

    // 4. MPU6050 configuration (±8g scale so impacts >2G never clip!)
    writeMPU(0x6B, 0x00); // Wake up
    writeMPU(0x1C, 0x10); // ±8g accelerometer range (4096 LSB/g)
    writeMPU(0x1B, 0x08); // ±500°/s gyroscope
    writeMPU(0x1A, 0x03); // DLPF ~43 Hz bandwidth
    writeMPU(0x19, 0x04); // 200 Hz sample rate

    // 5. DS18B20 temperature (non-blocking, 10-bit fast conversion)
    sensors.begin();
    sensors.setWaitForConversion(false);
    sensors.setResolution(10);

    // 6. MAX30102 Calibrated PPG Setup (non-blocking if wire is loose)
    if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("[WARNING] MAX30102 NOT FOUND! Continuing with other sensors...");
        max30102Available = false;
    } else {
        max30102Available = true;
        particleSensor.setup(0x24, 4, 2, 100, 411, 4096); // 25 Hz effective rate
    }

    telemetryMutex = xSemaphoreCreateMutex();
    telemetryQueue = xQueueCreate(16, sizeof(TelemetryPacket));

    xTaskCreatePinnedToCore(mqttTask, "MQTTTask", 8192, NULL, 1, &mqttTaskHandle, 0);
}

// ======================== LOOP (CORE 1 - SENSOR SAMPLING) ========================
void loop() {
    unsigned long now = millis();

    // 1. High-frequency fall detection (every 5 ms = 200 Hz)
    if (now - lastMPUCheck >= MPU_INTERVAL) {
        lastMPUCheck = now;
        checkFall();
    }

    // 2. Alert actuator state machine
    updateAlert();

    // 3. Heart rate & SpO2 PPG processing (polled every 20 ms to protect I2C bus)
    if (now - lastMAXCheck >= MAX30102_POLL_INTERVAL) {
        lastMAXCheck = now;
        checkMAX30102();
    }

    // 4. Air quality & gas hazard detection (every 250 ms)
    if (now - lastMQCheck >= MQ_INTERVAL) {
        lastMQCheck = now;
        checkMQ135();
    }

    // 5. Body temperature (every 250 ms async conversion)
    checkTemperature();

    // 6. Unified Serial output (every 1 second)
    if (now - lastSerialLog >= SERIAL_LOG_INTERVAL) {
        lastSerialLog = now;
        logTelemetryToSerial();
    }

    // 7. Periodic MQTT telemetry publish (every 1 second)
    if (now - lastPostTime >= TELEMETRY_INTERVAL) {
        lastPostTime = now;
        queueTelemetry(false, false);
    }
}