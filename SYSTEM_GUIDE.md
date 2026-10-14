# WorkBand — Industrial Worker Safety System
## Complete Operations & Integration Guide

> **Your setup**: Windows PC + ESP32-001 + Mosquitto (localhost:1883) + InfluxDB (org: VIT, bucket: IoT) + FastAPI (port 8001) + React Frontend (port 5174)

---

## Table of Contents

1. [System Architecture](#1-system-architecture)
2. [Hardware — What's on the ESP32](#2-hardware--whats-on-the-esp32)
3. [Software Components & What Each Does](#3-software-components--what-each-does)
4. [Configuration Files at a Glance](#4-configuration-files-at-a-glance)
5. [Step-by-Step: Flash the ESP32](#5-step-by-step-flash-the-esp32)
6. [Step-by-Step: Start the Full Stack](#6-step-by-step-start-the-full-stack)
7. [Verifying Real IoT Data is Flowing](#7-verifying-real-iot-data-is-flowing)
8. [Dashboard Walkthrough — All Pages & Features](#8-dashboard-walkthrough--all-pages--features)
9. [Understanding the Alert System](#9-understanding-the-alert-system)
10. [Fall Detection — How It Works End-to-End](#10-fall-detection--how-it-works-end-to-end)
11. [Troubleshooting](#11-troubleshooting)
12. [Quick Reference Cheatsheet](#12-quick-reference-cheatsheet)

---

## 1. System Architecture

```
+------------------------------------------------------------------+
|                  ESP32 (Device ID: ESP32-001)                    |
|                                                                  |
|  Core 1 (loop)                    Core 0 (mqttTask)             |
|  -----------------                -----------------             |
|  MPU6050  -> 200 Hz fall detect   WiFi & MQTT client            |
|  MAX30102 -> HR + SpO2            Dequeues TelemetryPackets      |
|  DS18B20  -> Skin temperature     Publishes to Mosquitto         |
|  MQ-135   -> Air quality ADC      Handles LWT (auto-offline)    |
|               |                              |                  |
|         FreeRTOS Queue (thread-safe)         |                  |
|               +------------------------------+                  |
+----------------------------------+-------------------------------+
                                   | MQTT (TCP port 1883)
                                   v
+------------------------------------------------------------------+
|          Mosquitto MQTT Broker (Windows Service)                 |
|          Topics subscribed by backend:                           |
|          workband/telemetry/ESP32-001  (QoS 0 - vitals)         |
|          workband/critical/ESP32-001   (QoS 1 - falls)          |
|          workband/status/ESP32-001     (QoS 1 - LWT)            |
+----------------------------------+-------------------------------+
                                   | aiomqtt (async subscriber)
                                   v
+------------------------------------------------------------------+
|          FastAPI Backend  (http://0.0.0.0:8001)                  |
|                                                                  |
|  process_reading()                                               |
|  +-- Writes to SQLite  (workers, alerts, falls, devices)         |
|  +-- Writes to InfluxDB  (sensor_readings, time-series)          |
|  +-- Runs alert engine  (HR / SpO2 / Temp / Fall rules)         |
|  +-- Broadcasts via WebSocket to all connected dashboards        |
|                                                                  |
|  REST API  /api/*         WebSocket  /ws/monitoring              |
+----------------------------------+-------------------------------+
                                   | HTTP + WebSocket
                                   v
+------------------------------------------------------------------+
|       React Dashboard  (http://localhost:5174)                   |
|       Pages: Overview / Workers / Alerts / Analytics / Devices   |
+------------------------------------------------------------------+
```

### Data Flow Timing

| Stage | Frequency | Protocol |
|---|---|---|
| MPU6050 fall detection | **200 Hz** (every 5 ms) | I2C on Core 1 |
| Periodic vitals publish | Every **2 seconds** | MQTT QoS 0 |
| Fall event publish | **Immediately** on detection | MQTT QoS 1 |
| Backend to Dashboard push | On every reading received | WebSocket |
| Dashboard fallback poll | Every **15 seconds** | HTTP GET |

---

## 2. Hardware — What's on the ESP32

### Sensors

| Sensor | GPIO | What it measures | Update Rate |
|---|---|---|---|
| **MPU6050** (IMU) | SDA=22, SCL=23 (I2C) | X/Y/Z acceleration (G-force) for fall detection | 200 Hz |
| **MAX30102** (PPG) | SDA=22, SCL=23 (I2C) | Heart rate (BPM) + SpO2 (%) | ~25 Hz (buffered) |
| **DS18B20** (Temp) | GPIO 4 (1-Wire) | Skin/body temperature in Celsius | ~1 Hz (async) |
| **MQ-135** (Gas) | GPIO 34 (ADC) | Air quality — raw ADC (0-4095) + voltage (0-3.3V) | 2 Hz |

### Actuators (Local Feedback)

| Actuator | GPIO | Trigger Condition |
|---|---|---|
| LED (alert) | GPIO 27 | Any confirmed alert (HR, SpO2, Fall, Temp) |
| Vibration motor | GPIO 26 | Same as LED |
| Buzzer (active low) | GPIO 25 | Same as LED, buzzes for 2 seconds |

### Wiring Notes

- MPU6050 and MAX30102 **share the I2C bus** (SDA=22, SCL=23) — different addresses (0x68 and 0x57)
- DS18B20 requires a **4.7k ohm pull-up resistor** between data pin and 3.3V
- MQ-135 analog output goes to GPIO 34 (ADC1_CH6) — only ADC1 pins work reliably during WiFi operation

---

## 3. Software Components & What Each Does

### 3a. Arduino Firmware (`arduino/IndustrialWorkerSafetyBand.ino`)

**FreeRTOS Dual-Core Design:**
- **Core 1 (`loop()`)** — 100% dedicated to sensors. Never sleeps longer than 5 ms. Unaffected by network delays.
- **Core 0 (`mqttTask`)** — 100% dedicated to WiFi + MQTT. If MQTT is slow or disconnected, sensors keep running.

**Communication between cores** uses a **FreeRTOS queue** (`telemetryQueue`, depth 8) and a **mutex** (`telemetryMutex`) protecting shared volatile sensor readings.

**Fall Detection State Machine (3-phase):**
```
NORMAL --(G < 0.45g)--> FREE_FALL --(G > 2.0g)--> IMPACT --(rest 1.5s)--> CONFIRMED or REJECTED
             |                                                                        |
        (timeout 600ms)                                                          back to NORMAL
             |
        back to NORMAL
```

### 3b. Backend (`backend/main.py`)

| Feature | Details |
|---|---|
| **MQTT Subscriber** | `aiomqtt.Client` runs as a background asyncio task. Subscribes to `workband/#`. Auto-reconnects every 5s on failure. |
| **InfluxDB Writer** | Writes sensor fields as time-series points with millisecond precision. Measurement: `sensor_readings`. |
| **SQLite Store** | Relational data: workers, alerts, fall_incidents, devices, readings history |
| **Alert Engine** | Rule-based: HR outside 45-130 BPM = CRITICAL; SpO2 < 90% = CRITICAL; Temp > 38C = WARNING; Fall = CRITICAL |
| **WebSocket Hub** | Broadcasts 4 event types: `reading`, `alert`, `fall_incident`, `device_status` |
| **HTTP Fallback** | `POST /api/readings` accepts the same JSON payload as MQTT |

### 3c. Frontend (`frontend/src/main.jsx`)

| Page | What You See |
|---|---|
| **Overview** | Live vitals gauges, trend chart, latest alert strip, device status badge |
| **Workers** | Roster of all workers, search/filter, fall count per worker, click for detail modal |
| **Alerts** | Active/acknowledged/resolved alerts with severity, device, timestamp |
| **Analytics** | Historical chart for any metric over 1h/6h/24h |
| **Devices** | Device online/offline status, last-seen timestamp |

**Real-time updates** arrive via WebSocket (`ws://127.0.0.1:8001/ws/monitoring`). The dashboard updates instantly on every MQTT message from the ESP32 — no page refresh needed.

---

## 4. Configuration Files at a Glance

### `backend/.env` (your current values)

```
PORT=8001                    # FastAPI listens here
HOST=0.0.0.0

MQTT_HOST=localhost           # Mosquitto broker
MQTT_PORT=1883

INFLUX_URL=http://localhost:8086
INFLUX_TOKEN=9dfm-xr7x...    # Your InfluxDB API token
INFLUX_ORG=VIT               # Your InfluxDB org
INFLUX_BUCKET=IoT            # Your InfluxDB bucket

HEART_MIN=45
HEART_MAX=130
SPO2_MIN=90
TEMP_MAX=38
```

### `frontend/.env`

```
VITE_API_URL=http://127.0.0.1:8001
```

### `arduino/IndustrialWorkerSafetyBand.ino` — lines to edit before flashing

```cpp
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_SERVER   "192.168.x.x"   // Your PC's LAN IP
#define MQTT_PORT     1883
#define DEVICE_ID     "ESP32-001"
```

---

## 5. Step-by-Step: Flash the ESP32

### Step 1 — Find Your PC's LAN IP Address

Open PowerShell:
```powershell
ipconfig | findstr "IPv4"
```
Look for the IP on your **Wi-Fi adapter** (e.g., `192.168.1.42`). This is what goes into `MQTT_SERVER`.

> **Important**: Do NOT use `localhost` or `127.0.0.1` in the Arduino code. The ESP32 is a separate network device and cannot reach your PC's loopback interface.

### Step 2 — Install Required Arduino Libraries

In Arduino IDE, go to **Tools > Manage Libraries** and install:

| Library | Author |
|---|---|
| `PubSubClient` | Nick O'Leary |
| `MAX30105` (SparkFun) | SparkFun |
| `DallasTemperature` | Miles Burton |
| `OneWire` | Jim Studt |

Also install **ESP32 board support**: Tools > Board > Boards Manager > search "esp32" > install by Espressif Systems.

### Step 3 — Edit the Arduino Sketch

Open `arduino/IndustrialWorkerSafetyBand.ino` and change **only these 3 lines**:
```cpp
#define WIFI_SSID     "YourActualNetworkName"
#define WIFI_PASSWORD "YourActualPassword"
#define MQTT_SERVER   "192.168.x.x"   // Your PC's LAN IP from Step 1
```

### Step 4 — Flash

1. Select **Tools > Board > ESP32 Dev Module**
2. Select the correct **COM port** under Tools > Port
3. Set Upload Speed to 921600
4. Click **Upload**
5. Open **Serial Monitor** at **115200 baud**

### Step 5 — Verify ESP32 Boot Output

You should see in Serial Monitor:
```
=====================================================
   WORKER SAFETY BAND - MQTT + FreeRTOS ARCHITECTURE
=====================================================
[Core 0] Connecting Wi-Fi.....
[Core 0] Wi-Fi connected! IP: 192.168.1.57
[Core 0] Connecting MQTT to 192.168.1.42:1883...
[Core 0] MQTT CONNECTED!
[Core 0] Published online status (retained, QoS 0).
```

If MQTT shows `rc=-2` = broker unreachable — check IP and that Mosquitto is running.

---

## 6. Step-by-Step: Start the Full Stack

Run each of these in **separate PowerShell windows** in this order:

### Window 1 — Mosquitto MQTT Broker (run as Administrator)
```powershell
net start mosquitto
# OR run directly: mosquitto -v
```
Confirm: `Listening on port 1883`

### Window 2 — InfluxDB
```powershell
influxd
```
Confirm: listening on `:8086`

> **First-time InfluxDB setup**: Open `http://localhost:8086` and complete the setup wizard. Create org `VIT` and bucket `IoT`. Copy the API token into `backend/.env` as `INFLUX_TOKEN`.

### Window 3 — Backend
```powershell
cd "C:\Users\User\Documents\Adithya\College\College Year 3\Semester 1\Embedded Systems and IoT\WorkBand\backend"
python main.py
```
Confirm you see all of these lines:
```
INFO  Loaded environment from ...backend\.env
INFO  InfluxDB connected (http://localhost:8086) - bucket: IoT
INFO  WorkBand backend started (MQTT=True, InfluxDB=True)
INFO  MQTT connected! Subscribing to workband/# ...
INFO  Uvicorn running on http://0.0.0.0:8001
```

### Window 4 — Frontend
```powershell
cd "C:\Users\User\Documents\Adithya\College\College Year 3\Semester 1\Embedded Systems and IoT\WorkBand\frontend"
npm run dev
```
Open: **http://localhost:5174**

---

## 7. Verifying Real IoT Data is Flowing

### Check 1 — ESP32 Serial Monitor
Every 2 seconds you should see:
```
Heart Rate: 76.3 BPM | SpO2: 98.0%
Temperature: 36.82 C
MQ-135 ADC: 315 | Voltage: 0.85V
[Core 0] MQTT PUB workband/telemetry/ESP32-001 (128 bytes)
```
If you see `MQTT PUB FAILED` — Mosquitto is down or IP is wrong.

### Check 2 — Backend Log (Window 3)
For every ESP32 publish:
```
INFO  [MQTT] Normal reading processed | device=ESP32-001 fall=False alerts=0
```
For a fall:
```
CRITICAL  CRITICAL MQTT message on workband/critical/ESP32-001
INFO  [MQTT-CRITICAL] CRITICAL reading processed | device=ESP32-001 fall=True alerts=1
```

### Check 3 — Health Endpoint
```powershell
Invoke-RestMethod http://127.0.0.1:8001/api/health | ConvertTo-Json
```
Expected:
```json
{
  "status": "ok",
  "mqtt_enabled": true,
  "influxdb_connected": true,
  "influxdb_url": "http://localhost:8086",
  "architecture": "MQTT + InfluxDB + SQLite + FastAPI + WebSocket"
}
```

### Check 4 — Live Readings via API
```powershell
Invoke-RestMethod "http://127.0.0.1:8001/api/readings/latest?device_id=ESP32-001" | ConvertTo-Json -Depth 5
```
The `timestamp` field should be within the last 2 seconds if the device is live.

### Check 5 — InfluxDB Data Explorer
1. Open `http://localhost:8086`
2. Go to **Data Explorer**
3. Select bucket: **IoT**, measurement: **sensor_readings**, field: **heart_rate**
4. Set time range to last 5 minutes
5. You should see a live-updating chart of heart rate values

### Check 6 — Worker Online Status
```powershell
Invoke-RestMethod "http://127.0.0.1:8001/api/device/status?device_id=ESP32-001" | ConvertTo-Json
```
Expected: `"status": "online"` — updates every time a reading arrives.

### Check 7 — MQTT Wire-Level Inspection
```powershell
# Subscribe directly to see raw JSON from ESP32:
mosquitto_sub -h localhost -t "workband/#" -v
```
You will see:
```
workband/telemetry/ESP32-001 {"device_id":"ESP32-001","heart_rate":76.3,"spo2":98.0,...,"fall_detected":false}
```

---

## 8. Dashboard Walkthrough — All Pages & Features

### Overview Page
- **Status bar**: Backend connection badge, Simulation toggle, last update time
- **Device badge**: Shows `ESP32-001 — Online` when MQTT data flows
- **Vitals cards**: Heart Rate, SpO2, Temperature, Air Quality — update in real-time via WebSocket
- **Trend chart**: Line graph of the last N readings (up to 1h)

> **Live test**: Place your finger on the MAX30102 sensor. Within ~4 seconds (100-sample buffer fills), BPM and SpO2 cards will update on the dashboard.

### Workers Page
- Roster of all workers with name, role, department, device ID, fall count
- Search bar and status tabs (All / Online / High Risk)
- **Click any worker** to open a detail modal:
  - Current vitals (if device is online)
  - Fall history count and emergency contact
  - If device is offline: shows "OFFLINE" badge with last-seen time

### Alerts Page
- All Active, Acknowledged, and Resolved alerts
- Severity colours: Red = Critical, Yellow = Warning
- Click **Acknowledge** or **Resolve** to manage alerts
- Alerts auto-resolve when the underlying condition clears on the next reading

### Analytics Page
- Select device and time range (1h / 6h / 24h)
- If InfluxDB is connected: queries time-series from bucket `IoT`
- If InfluxDB is offline: queries SQLite readings table as fallback

### Devices Page
- All known ESP32 devices with status
- `online` = reading received within last 15 seconds
- `offline` = no reading for >15s OR broker received LWT message

### Fall Incident Alerts (Real-time Banner)
When a fall is confirmed:
1. Red emergency banner slides in from the top of the dashboard
2. Shows: worker name, time, peak G-force, free-fall duration
3. Click **View Forensic Proof** to open the kinematic dossier:
   - Phase breakdown (Free Fall > Impact > Rest)
   - G-force trace over time
   - Vitals at moment of impact
4. Worker fall counter increments automatically

---

## 9. Understanding the Alert System

### Threshold Rules

| Metric | Condition | Severity |
|---|---|---|
| Heart Rate | < 45 BPM or > 130 BPM | **CRITICAL** |
| SpO2 | < 90% | **CRITICAL** |
| Temperature | > 38 C | **WARNING** |
| Fall | Confirmed by 3-phase kinematic algorithm | **CRITICAL** |

### Alert Lifecycle

```
Reading arrives
     |
     v
evaluate_alerts() runs
     |
     v (new threshold violation)
INSERT into alerts table
     |
     v
WebSocket broadcast (type: "alert")
     |
     v
Dashboard Alerts page + banner updates

Next reading with normal values:
     |
     v
Alert auto-resolves (status = "Resolved")
```

### Tuning Thresholds

Edit `backend/.env` and restart the backend — no code changes needed:
```
HEART_MIN=50    # Stricter lower bound
HEART_MAX=120   # Stricter upper bound
TEMP_MAX=37.5   # Lower fever threshold
```

---

## 10. Fall Detection — How It Works End-to-End

### Phase 1: Free Fall (Core 1 — MPU6050 at 200 Hz)

The ESP32 checks G-force every 5 ms. When total G drops below **0.45g** (weightlessness):
- State: NORMAL -> FREE_FALL
- Timer starts. If impact doesn't come within **600 ms**, it resets (false positive rejected)

### Phase 2: Impact Detection

When G exceeds **2.0g** while in FREE_FALL state:
- State: FREE_FALL -> IMPACT
- Peak G-force is recorded
- 1,500 ms observation window begins

### Phase 3: Rest Stability Confirmation

During the 1,500 ms window, G must stay between **0.70g - 1.35g** (lying still):
- Requires **20 consecutive stable samples**
- G variation must be < **0.30g**
- If criteria met: **FALL CONFIRMED**
- If not: rejected (person got up, object dropped, etc.)

### On Confirmation

1. Buzzer/LED/motor activate for 2 seconds (immediate local feedback)
2. `FallProofData` struct populated: peak G, free-fall duration, rest variation, stable samples
3. Packet sent to **front** of FreeRTOS queue (bypasses normal telemetry)
4. Core 0 publishes to `workband/critical/ESP32-001` (QoS 1, guaranteed delivery)
5. Backend stores fall incident with full kinematic proof JSON
6. Worker fall count increments
7. Dashboard shows emergency banner within ~200ms of the fall

### Simulating a Fall (Without the Physical Device)

```powershell
Invoke-RestMethod -Method POST -Uri "http://127.0.0.1:8001/api/incidents/simulate-fall" `
  -ContentType "application/json" `
  -Body '{"device_id":"ESP32-001","peak_impact_g":3.5,"free_fall_duration_ms":240,"heart_rate":118,"spo2":96,"temperature":37.1}'
```

---

## 11. Troubleshooting

### ESP32 Won't Connect to MQTT

| Symptom | Cause | Fix |
|---|---|---|
| `rc=-2` in Serial | Broker unreachable | Check `MQTT_SERVER` IP. Ensure Mosquitto is running. |
| `rc=-4` | Connection refused | Configure Mosquitto for anonymous: run `setup_services.ps1` |
| WiFi connects but MQTT fails | Windows Firewall | Allow port 1883: `New-NetFirewallRule -DisplayName "Mosquitto" -Direction Inbound -LocalPort 1883 -Protocol TCP -Action Allow` |
| No WiFi at all | Wrong credentials | Check `WIFI_SSID` and `WIFI_PASSWORD` in sketch |

### Backend Not Seeing MQTT Data

| Symptom | Fix |
|---|---|
| `MQTT connection lost` every 5s in logs | Mosquitto not running: `net start mosquitto` |
| Connecting but never `MQTT connected!` | Wrong MQTT_HOST in `backend/.env` (should be `localhost`) |
| Health shows `mqtt_enabled: false` | `aiomqtt` not installed: `pip install aiomqtt` |

### InfluxDB Not Storing Data

| Symptom | Fix |
|---|---|
| Health shows `influxdb_connected: false` | Start InfluxDB: run `influxd` |
| Connected but no data in Data Explorer | Check INFLUX_ORG=`VIT` and INFLUX_BUCKET=`IoT` match exactly |
| 401 Unauthorized in logs | Wrong token: regenerate in InfluxDB UI > Data > API Tokens |

### Dashboard Not Updating in Real-Time

| Symptom | Fix |
|---|---|
| Values only update every 15 seconds | WebSocket disconnected — check browser console for WS errors |
| "Cannot reach FastAPI backend" error | Backend not running, or wrong VITE_API_URL in `frontend/.env` |
| Frontend on 5174 but backend on different port | Update `frontend/.env` VITE_API_URL and restart `npm run dev` |

### Port Already in Use

```powershell
# Find what's using the port:
netstat -ano | findstr ":8001"

# Kill the process by PID:
taskkill /F /PID <PID_NUMBER>
```

---

## 12. Quick Reference Cheatsheet

### Start Everything (in order)

```powershell
# 1 - Mosquitto (Admin PowerShell)
net start mosquitto

# 2 - InfluxDB
influxd

# 3 - Backend
cd "C:\Users\User\Documents\Adithya\College\College Year 3\Semester 1\Embedded Systems and IoT\WorkBand\backend"
python main.py

# 4 - Frontend
cd "C:\Users\User\Documents\Adithya\College\College Year 3\Semester 1\Embedded Systems and IoT\WorkBand\frontend"
npm run dev
# Then open: http://localhost:5174
```

### Key API Endpoints

| Endpoint | Description |
|---|---|
| `GET /api/health` | Full system status (MQTT, InfluxDB, uptime) |
| `GET /api/workers` | All workers + fall counts |
| `GET /api/workers/ESP32-001` | Single worker detail |
| `GET /api/readings/latest?device_id=ESP32-001` | Most recent sensor reading |
| `GET /api/readings/history?device_id=ESP32-001&hours=1` | 1-hour history |
| `GET /api/alerts` | All active alerts |
| `GET /api/incidents/falls` | All fall records with kinematic proof |
| `POST /api/readings` | Manually inject a reading |
| `POST /api/incidents/simulate-fall` | Simulate a fall event |
| `WS  /ws/monitoring` | WebSocket real-time stream |

### Arduino Key Constants (in the .ino file)

| Constant | Default | Effect |
|---|---|---|
| `TELEMETRY_INTERVAL` | 2000 ms | How often vitals are sent |
| `FREE_FALL_THRESHOLD` | 0.45g | Sensitivity to free-fall phase |
| `IMPACT_THRESHOLD` | 2.0g | Minimum G to count as impact |
| `STABLE_SAMPLES_REQUIRED` | 20 | Reduce to trigger fall confirmation more easily |
| `ALERT_DURATION` | 2000 ms | How long buzzer/LED activates |

### MQTT Topics

```
workband/telemetry/ESP32-001   <- Normal vitals every 2s (QoS 0)
workband/critical/ESP32-001    <- Fall events + anomalies (QoS 1)
workband/status/ESP32-001      <- Online/Offline LWT (QoS 1, retained)
```

### File Map

```
WorkBand/
|-- arduino/
|   +-- IndustrialWorkerSafetyBand.ino    <- Flash this to the ESP32
|-- backend/
|   |-- .env                               <- All backend config
|   |-- main.py                            <- FastAPI + MQTT + DB logic
|   |-- requirements.txt                   <- Python deps
|   +-- safety_band.db                     <- SQLite (auto-created)
|-- frontend/
|   |-- .env                               <- VITE_API_URL
|   |-- src/main.jsx                       <- Entire React dashboard
|   +-- vite.config.js                     <- Dev server + proxy
|-- setup_services.ps1                     <- Install Mosquitto + InfluxDB
+-- SYSTEM_GUIDE.md                        <- This file
```
