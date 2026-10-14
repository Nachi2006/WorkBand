#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"
#define SDA_PIN 22
#define SCL_PIN 23
#define ONE_WIRE_BUS 4
#define MQ135_PIN 34
#define BUZZER_PIN 25
#define MOTOR_PIN 26
#define LED_PIN 27
#define MPU 0x68
// Configure these values before flashing. Use your computer LAN IPv4, not localhost.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define BACKEND_URL "http://192.168.1.20:8000/api/readings"
#define DEVICE_ID "ESP32-001"
#define FREE_FALL_THRESHOLD 0.45
#define IMPACT_THRESHOLD 2.0
#define FREE_FALL_TIMEOUT 600
#define POST_IMPACT_TIME 1500
#define REST_MIN 0.70
#define REST_MAX 1.35
#define STABLE_SAMPLES_REQUIRED 20
#define MAX_REST_VARIATION 0.30
#define MPU_INTERVAL 5
#define HEART_MIN 45
#define HEART_MAX 130
#define SPO2_MIN 90
#define FINGER_IR_MIN 300
#define FINGER_CONFIRM_SAMPLES 10
#define TEMP_MAX 38.0
#define MQ_INTERVAL 500
#define TEMP_INTERVAL 2000
#define ALERT_DURATION 2000
MAX30105 particleSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
uint32_t irBuffer[100];
uint32_t redBuffer[100];
int maxSamples=0;
int32_t spo2;
int8_t validSpO2;
int32_t heartRate;
int8_t validHeartRate;
float smoothBPM=0;
float smoothSpO2=0;
int fingerSamples=0;
bool fingerPresent=false;
unsigned long lastMPUCheck=0;
unsigned long lastMQCheck=0;
unsigned long lastTempCheck=0;
unsigned long lastTempRequest=0;
bool temperatureRequested=false;
enum FallState{
    NORMAL,
    FREE_FALL,
    IMPACT
};
FallState fallState=NORMAL;
unsigned long freeFallTime=0;
unsigned long impactTime=0;
int stableSamples=0;
float postImpactMin=10.0;
float postImpactMax=0.0;
bool alertActive=false;
// Telemetry snapshot. Local sensor processing and actuator behavior remain in place.
float telemetryHeartRate=NAN;
float telemetrySpO2=NAN;
float telemetryTemperature=NAN;
int telemetryAirADC=-1;
float telemetryAirVoltage=NAN;
bool telemetryFallPending=false;
unsigned long lastPostTime=0;
const unsigned long POST_INTERVAL=2000;
unsigned long alertStart=0;
void writeMPU(byte reg,byte value){
    Wire.beginTransmission(MPU);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}
void readAcceleration(float &ax,float &ay,float &az){
    Wire.beginTransmission(MPU);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU,6);
    if(Wire.available()<6){
        ax=0;
        ay=0;
        az=0;
        return;
    }
int16_t axRaw=(Wire.read()<<8)|Wire.read();
int16_t ayRaw=(Wire.read()<<8)|Wire.read();
int16_t azRaw=(Wire.read()<<8)|Wire.read();
ax=axRaw/16384.0;
ay=ayRaw/16384.0;
az=azRaw/16384.0;
}
void resetFall(){
    fallState=NORMAL;
    stableSamples=0;
    postImpactMin=10.0;
    postImpactMax=0.0;
}
void startAlert(const char* reason){
    if(alertActive)
    return;
    alertActive=true;
    alertStart=millis();
    digitalWrite(LED_PIN,HIGH);
    digitalWrite(MOTOR_PIN,HIGH);
    digitalWrite(BUZZER_PIN,LOW);
    Serial.println();
    Serial.println("================================");
    Serial.print("ALERT: ");
    Serial.println(reason);
    Serial.println("================================");
}
void updateAlert(){
    if(!alertActive)
    return;
    if(millis()-alertStart>=ALERT_DURATION){
        digitalWrite(LED_PIN,LOW);
        digitalWrite(MOTOR_PIN,LOW);
        digitalWrite(BUZZER_PIN,HIGH);
        alertActive=false;
        Serial.println("Alert ended");
        Serial.println();
    }
}
void checkFall(){
    float ax;
    float ay;
    float az;
    readAcceleration(ax,ay,az);
    float total=sqrt(
    ax*ax+
    ay*ay+
    az*az
    );
    switch(fallState){
    case NORMAL:
    if(total<FREE_FALL_THRESHOLD){
        fallState=FREE_FALL;
        freeFallTime=millis();
        Serial.println("FREE FALL DETECTED!");
    }
break;
case FREE_FALL:
if(millis()-freeFallTime>FREE_FALL_TIMEOUT){
    Serial.println("Free fall rejected.");
    resetFall();
}
else if(total>=IMPACT_THRESHOLD){
    fallState=IMPACT;
    impactTime=millis();
    stableSamples=0;
    postImpactMin=10.0;
    postImpactMax=0.0;
    Serial.println("IMPACT DETECTED!");
}
break;
case IMPACT:
if(total>=REST_MIN &&
total<=REST_MAX){
    stableSamples++;
    if(total<postImpactMin)
    postImpactMin=total;
    if(total>postImpactMax)
    postImpactMax=total;
}
else{
    stableSamples=0;
    postImpactMin=10.0;
    postImpactMax=0.0;
}
if(millis()-impactTime>=POST_IMPACT_TIME){
    float variation=
    postImpactMax-postImpactMin;
    Serial.print("Stable samples: ");
    Serial.println(stableSamples);
    Serial.print("Rest variation: ");
    Serial.print(variation,2);
    Serial.println(" g");
    if(stableSamples>=STABLE_SAMPLES_REQUIRED &&
    variation<=MAX_REST_VARIATION){
        Serial.println();
        Serial.println("********************************");
        Serial.println("       FALL CONFIRMED");
        Serial.println("********************************");
        telemetryFallPending=true;
        startAlert("FALL DETECTED");
    }
else{
    Serial.println("Impact rejected.");
}
resetFall();
}
break;
}
}
void checkMAX30102(){
    particleSensor.check();
    while(particleSensor.available()){
        uint32_t ir=particleSensor.getIR();
        uint32_t red=particleSensor.getRed();
        particleSensor.nextSample();
        if(ir>=FINGER_IR_MIN){
            if(fingerSamples<FINGER_CONFIRM_SAMPLES)
            fingerSamples++;
        }
else{
    fingerSamples=0;
    if(fingerPresent)
    Serial.println("Finger removed.");
    fingerPresent=false;
    maxSamples=0;
    smoothBPM=0;
    smoothSpO2=0;
    continue;
}
if(fingerSamples>=FINGER_CONFIRM_SAMPLES){
    if(!fingerPresent)
    Serial.println("Finger detected.");
    fingerPresent=true;
}
if(!fingerPresent)
continue;
if(maxSamples<100){
    redBuffer[maxSamples]=red;
    irBuffer[maxSamples]=ir;
    maxSamples++;
}
}
if(!fingerPresent){
    telemetryHeartRate=NAN;
    telemetrySpO2=NAN;
    return;
}
if(maxSamples<100)
return;
maxim_heart_rate_and_oxygen_saturation(
irBuffer,
100,
redBuffer,
&spo2,
&validSpO2,
&heartRate,
&validHeartRate
);
if(validHeartRate &&
heartRate>=40 &&
heartRate<=180){
    if(smoothBPM==0)
    smoothBPM=heartRate;
else
smoothBPM=
0.8*smoothBPM+
0.2*heartRate;
}
if(validSpO2 &&
spo2>=80 &&
spo2<=100){
    if(smoothSpO2==0)
    smoothSpO2=spo2;
else
smoothSpO2=
0.8*smoothSpO2+
0.2*spo2;
}
Serial.print("Heart Rate: ");
if(smoothBPM>0)
Serial.print(smoothBPM,1);
else
Serial.print("Not detected");
Serial.print(" BPM | SpO2: ");
if(smoothSpO2>0)
Serial.print(smoothSpO2,1);
else
Serial.print("Not detected");
Serial.println(" %");
telemetryHeartRate=smoothBPM>0?smoothBPM:NAN;
telemetrySpO2=smoothSpO2>0?smoothSpO2:NAN;
if(smoothBPM>0){
    if(smoothBPM<HEART_MIN){
        startAlert("LOW HEART RATE");
    }
else if(smoothBPM>HEART_MAX){
    startAlert("HIGH HEART RATE");
}
}
if(smoothSpO2>0 &&
smoothSpO2<SPO2_MIN){
    startAlert("LOW SpO2");
}
maxSamples=0;
}
void checkTemperature(){
    unsigned long now=millis();
    if(!temperatureRequested){
        sensors.requestTemperatures();
        temperatureRequested=true;
        lastTempRequest=now;
        return;
    }
if(now-lastTempRequest>=800){
    float temperature=
    sensors.getTempCByIndex(0);
    if(temperature!=DEVICE_DISCONNECTED_C && temperature>-55 && temperature<125 && temperature!=85.0){
        telemetryTemperature=temperature;
    }
    Serial.print("Temperature: ");
    Serial.print(temperature,2);
    Serial.println(" C");
    if(temperature>TEMP_MAX){
        Serial.println("HIGH TEMPERATURE!");
    }
temperatureRequested=false;
}
}
void checkMQ135(){
    int value=analogRead(MQ135_PIN);
    float voltage=
    value*(3.3/4095.0);
    telemetryAirADC=value;
    telemetryAirVoltage=voltage;
    Serial.print("MQ-135 ADC: ");
    Serial.print(value);
    Serial.print(" | Voltage: ");
    Serial.print(voltage,2);
    Serial.println(" V");
}
void setup(){
    Serial.begin(115200);
    pinMode(BUZZER_PIN,OUTPUT);
    pinMode(MOTOR_PIN,OUTPUT);
    pinMode(LED_PIN,OUTPUT);
    pinMode(MQ135_PIN,INPUT);
    digitalWrite(BUZZER_PIN,HIGH);
    digitalWrite(MOTOR_PIN,LOW);
    digitalWrite(LED_PIN,LOW);
    Wire.begin(SDA_PIN,SCL_PIN);
    Wire.setClock(400000);
    writeMPU(0x6B,0x00);
    writeMPU(0x1C,0x00);
    writeMPU(0x1B,0x00);
    writeMPU(0x1A,0x03);
    writeMPU(0x19,0x04);
    sensors.begin();
    sensors.setWaitForConversion(false);
    if(!particleSensor.begin(
    Wire,
    I2C_SPEED_FAST
    )){
        Serial.println("MAX30102 NOT FOUND!");
        while(1){
        }
}
particleSensor.setup(
100,
1,
2,
100,
411
);
particleSensor.setPulseAmplitudeRed(0x7F);
particleSensor.setPulseAmplitudeIR(0x7F);
Serial.println();
Serial.println("================================");
Serial.println("     WORKER SAFETY BAND");
Serial.println("================================");
Serial.println("MPU6050  : READY");
Serial.println("MAX30102 : READY");
Serial.println("DS18B20  : READY");
Serial.println("MQ-135   : READY");
Serial.println();
Serial.println("REAL-TIME FALL DETECTION ACTIVE");
Serial.println("MPU6050 CHECK: 5 ms");
Serial.println();
WiFi.mode(WIFI_STA);
WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
Serial.println("Connecting to Wi-Fi for dashboard telemetry...");
}
void postTelemetry(){
    if(WiFi.status()!=WL_CONNECTED) return;
    unsigned long now=millis();
    if(now-lastPostTime<POST_INTERVAL) return;
    lastPostTime=now;
    String payload="{\"device_id\":\"" DEVICE_ID "\"";
    if(!isnan(telemetryHeartRate)) payload+=",\"heart_rate\":"+String(telemetryHeartRate,1);
    if(!isnan(telemetrySpO2)) payload+=",\"spo2\":"+String(telemetrySpO2,1);
    if(!isnan(telemetryTemperature)) payload+=",\"temperature\":"+String(telemetryTemperature,2);
    if(telemetryAirADC>=0) payload+=",\"air_quality_adc\":"+String(telemetryAirADC);
    if(!isnan(telemetryAirVoltage)) payload+=",\"air_quality_voltage\":"+String(telemetryAirVoltage,2);
    payload+=String(",\"fall_detected\":")+(telemetryFallPending?"true":"false")+"}";
    HTTPClient http;
    http.setConnectTimeout(500);
    http.setTimeout(700);
    if(http.begin(BACKEND_URL)){
        http.addHeader("Content-Type","application/json");
        int code=http.POST(payload);
        if(code>=200 && code<300){
            Serial.printf("Dashboard POST: HTTP %d\n",code);
            telemetryFallPending=false;
        }else{
            Serial.printf("Dashboard POST failed: HTTP %d\n",code);
        }
        http.end();
    }
}

void loop(){
    unsigned long now=millis();
    if(now-lastMPUCheck>=MPU_INTERVAL){
        lastMPUCheck=now;
        checkFall();
    }
updateAlert();
checkMAX30102();
if(WiFi.status()!=WL_CONNECTED){
    static unsigned long lastWiFiRetry=0;
    if(now-lastWiFiRetry>=10000){lastWiFiRetry=now;WiFi.reconnect();}
}
postTelemetry();
if(now-lastMQCheck>=MQ_INTERVAL){
    lastMQCheck=now;
    checkMQ135();
}
checkTemperature();
}
