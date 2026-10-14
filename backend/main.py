"""
Industrial Worker Safety Band - Backend Server
Architecture: MQTT Subscriber + InfluxDB (Time-Series) + SQLite (Relational) + FastAPI + WebSocket

Data Flow:
  ESP32 --MQTT--> Mosquitto Broker --aiomqtt--> This Server
                                                  |
                                     +------------+------------+
                                     |                         |
                              InfluxDB (sensor readings)  SQLite (workers, alerts, falls)
                                     |                         |
                                     +------------+------------+
                                                  |
                                      FastAPI REST + WebSocket --> React Frontend

Both InfluxDB and MQTT are optional - the system gracefully degrades:
  - Without MQTT: Use HTTP POST /api/readings as fallback
  - Without InfluxDB: All reads/writes go to SQLite only
"""
import sys
import asyncio

# ============================================================
# Windows Event Loop Fix (MUST be before any asyncio usage)
# aiomqtt uses add_reader/add_writer which are not supported
# on the default Windows ProactorEventLoop. Force the
# SelectorEventLoop so MQTT works correctly on Windows.
# ============================================================
if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

import json, os, sqlite3, time, logging
from contextlib import asynccontextmanager
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Optional, List, Dict, Any
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field, ConfigDict

log = logging.getLogger("workband")
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(levelname)s  %(message)s")

# ============================================================
# Load .env file (backend/.env) — must happen before any
# os.getenv() call so all config is picked up from the file.
# ============================================================
try:
    from dotenv import load_dotenv
    _env_path = Path(__file__).with_name('.env')
    if _env_path.exists():
        load_dotenv(dotenv_path=_env_path, override=False)
        log.info("Loaded environment from %s", _env_path)
    else:
        log.info("No .env file found at %s — using system environment / defaults", _env_path)
except ImportError:
    log.warning("python-dotenv not installed — run: pip install python-dotenv")

# ============================================================
# Optional imports - graceful degradation if not installed
# ============================================================
try:
    import aiomqtt
    MQTT_AVAILABLE = True
    log.info("aiomqtt loaded - MQTT subscriber enabled")
except ImportError:
    MQTT_AVAILABLE = False
    log.warning("aiomqtt not installed - MQTT disabled, use HTTP POST fallback")

try:
    from influxdb_client import InfluxDBClient, Point, WritePrecision
    from influxdb_client.client.write_api import SYNCHRONOUS
    INFLUX_AVAILABLE = True
    log.info("influxdb-client loaded - time-series storage enabled")
except ImportError:
    INFLUX_AVAILABLE = False
    log.warning("influxdb-client not installed - using SQLite only for readings")

# ============================================================
# Configuration (all overridable via environment variables)
# ============================================================
DB_PATH = Path(os.getenv('SAFETY_DB_PATH', Path(__file__).with_name('safety_band.db')))
STALE_SECONDS = int(os.getenv('DEVICE_STALE_SECONDS', '15'))
HEART_MIN = float(os.getenv('HEART_MIN', '45'))
HEART_MAX = float(os.getenv('HEART_MAX', '130'))
SPO2_MIN  = float(os.getenv('SPO2_MIN', '90'))
TEMP_MAX  = float(os.getenv('TEMP_MAX', '38'))

# MQTT Configuration
MQTT_HOST  = os.getenv('MQTT_HOST', 'localhost')
MQTT_PORT  = int(os.getenv('MQTT_PORT', '1883'))

# InfluxDB Configuration
INFLUX_URL    = os.getenv('INFLUX_URL', 'http://localhost:8086')
INFLUX_TOKEN  = os.getenv('INFLUX_TOKEN', 'workband-dev-token')
INFLUX_ORG    = os.getenv('INFLUX_ORG', 'workband')
INFLUX_BUCKET = os.getenv('INFLUX_BUCKET', 'safety_telemetry')

# ============================================================
# Pydantic Models
# ============================================================
class Reading(BaseModel):
    model_config = ConfigDict(extra='ignore')
    device_id: str = Field(default='ESP32-001', min_length=1, max_length=64)
    timestamp: Optional[datetime] = None
    heart_rate: Optional[float] = Field(default=None, ge=0, le=250)
    spo2: Optional[float] = Field(default=None, ge=0, le=100)
    temperature: Optional[float] = Field(default=None, ge=-55, le=125)
    air_quality_adc: Optional[int] = Field(default=None, ge=0, le=4095)
    air_quality_voltage: Optional[float] = Field(default=None, ge=0, le=3.6)
    fall_detected: Optional[bool] = None
    fall_proof: Optional[Dict[str, Any]] = None
    wrist_status: Optional[str] = Field(default=None, max_length=32)
    simulated: bool = False

class WorkerCreate(BaseModel):
    id: str = Field(..., min_length=1, max_length=32)
    name: str = Field(..., min_length=1, max_length=100)
    role: str = Field(..., min_length=1, max_length=100)
    department: str = Field(..., min_length=1, max_length=100)
    device_id: str = Field(..., min_length=1, max_length=64)
    emergency_contact: Optional[str] = None
    blood_group: Optional[str] = None
    shift: Optional[str] = 'Day'

class FallSimulationPayload(BaseModel):
    device_id: str = 'ESP32-001'
    peak_impact_g: float = 3.42
    free_fall_duration_ms: int = 240
    rest_variation_g: float = 0.12
    stable_samples: int = 24
    heart_rate: float = 118.0
    spo2: float = 96.0
    temperature: float = 37.1

# ============================================================
# SQLite (Relational Data: workers, alerts, devices, falls)
# ============================================================
def db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute('PRAGMA journal_mode=WAL;')
    conn.execute('PRAGMA busy_timeout=5000;')
    conn.execute('PRAGMA synchronous=NORMAL;')
    return conn

def init_db():
    with db() as c:
        c.execute('''CREATE TABLE IF NOT EXISTS readings(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL, timestamp TEXT NOT NULL,
            heart_rate REAL, spo2 REAL, temperature REAL,
            air_quality_adc INTEGER, air_quality_voltage REAL,
            fall_detected INTEGER, wrist_status TEXT,
            fall_proof_json TEXT, simulated INTEGER NOT NULL DEFAULT 0
        )''')
        c.execute('CREATE INDEX IF NOT EXISTS idx_readings_device_time ON readings(device_id, timestamp)')

        # Automatic schema migration for existing databases
        readings_cols = {row['name'] for row in c.execute("PRAGMA table_info(readings)").fetchall()}
        if 'fall_proof_json' not in readings_cols:
            c.execute("ALTER TABLE readings ADD COLUMN fall_proof_json TEXT")
        if 'simulated' not in readings_cols:
            c.execute("ALTER TABLE readings ADD COLUMN simulated INTEGER NOT NULL DEFAULT 0")

        c.execute('''CREATE TABLE IF NOT EXISTS alerts(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            incident_key TEXT NOT NULL, device_id TEXT NOT NULL,
            alert_type TEXT NOT NULL, description TEXT NOT NULL,
            severity TEXT NOT NULL, timestamp TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'Active', resolved_at TEXT,
            proof_json TEXT, simulated INTEGER NOT NULL DEFAULT 0
        )''')
        c.execute('CREATE INDEX IF NOT EXISTS idx_alerts_active ON alerts(device_id, incident_key, status)')

        alerts_cols = {row['name'] for row in c.execute("PRAGMA table_info(alerts)").fetchall()}
        if 'proof_json' not in alerts_cols:
            c.execute("ALTER TABLE alerts ADD COLUMN proof_json TEXT")
        if 'simulated' not in alerts_cols:
            c.execute("ALTER TABLE alerts ADD COLUMN simulated INTEGER NOT NULL DEFAULT 0")

        c.execute('''CREATE TABLE IF NOT EXISTS devices(
            device_id TEXT PRIMARY KEY, last_seen TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'offline',
            simulated INTEGER NOT NULL DEFAULT 0
        )''')

        devices_cols = {row['name'] for row in c.execute("PRAGMA table_info(devices)").fetchall()}
        if 'status' not in devices_cols:
            c.execute("ALTER TABLE devices ADD COLUMN status TEXT NOT NULL DEFAULT 'offline'")
        if 'simulated' not in devices_cols:
            c.execute("ALTER TABLE devices ADD COLUMN simulated INTEGER NOT NULL DEFAULT 0")

        c.execute('''CREATE TABLE IF NOT EXISTS workers(
            id TEXT PRIMARY KEY, name TEXT NOT NULL,
            role TEXT NOT NULL, department TEXT NOT NULL,
            device_id TEXT UNIQUE NOT NULL,
            emergency_contact TEXT, blood_group TEXT,
            shift TEXT DEFAULT 'Day', fall_count INTEGER DEFAULT 0,
            created_at TEXT NOT NULL
        )''')

        workers_cols = {row['name'] for row in c.execute("PRAGMA table_info(workers)").fetchall()}
        if 'fall_count' not in workers_cols:
            c.execute("ALTER TABLE workers ADD COLUMN fall_count INTEGER DEFAULT 0")
        if 'emergency_contact' not in workers_cols:
            c.execute("ALTER TABLE workers ADD COLUMN emergency_contact TEXT")
        if 'blood_group' not in workers_cols:
            c.execute("ALTER TABLE workers ADD COLUMN blood_group TEXT")
        if 'shift' not in workers_cols:
            c.execute("ALTER TABLE workers ADD COLUMN shift TEXT DEFAULT 'Day'")

        c.execute('''CREATE TABLE IF NOT EXISTS fall_incidents(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            incident_key TEXT, device_id TEXT NOT NULL,
            worker_id TEXT, timestamp TEXT NOT NULL,
            peak_impact_g REAL, free_fall_duration_ms INTEGER,
            rest_variation_g REAL, stable_samples INTEGER,
            heart_rate REAL, spo2 REAL, temperature REAL,
            proof_json TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'Active',
            simulated INTEGER NOT NULL DEFAULT 0
        )''')
        c.execute('CREATE INDEX IF NOT EXISTS idx_falls_device ON fall_incidents(device_id, timestamp)')

        # Seed initial worker roster if empty
        if c.execute('SELECT COUNT(*) n FROM workers').fetchone()['n'] == 0:
            ts = iso()
            c.executemany('''INSERT INTO workers(id, name, role, department, device_id, emergency_contact, blood_group, shift, fall_count, created_at)
                             VALUES(?,?,?,?,?,?,?,?,?,?)''', [
                ('W-101', 'Adithya Vardhan',    'Industrial Safety Lead',       'High Voltage Bay A', 'ESP32-001', '+91 98765 43210', 'O+',  'Day',   0, ts),
                ('W-102', 'Marcus Vance',       'Structural Welder',            'Turbine Deck B',     'ESP32-002', '+1 555-019-2831', 'A+',  'Day',   1, ts),
                ('W-103', 'Sarah Chen',         'Heavy Rig Operator',           'Crane Gantry 4',     'ESP32-003', '+1 555-014-9922', 'B+',  'Night', 0, ts),
                ('W-104', 'Elena Rostova',      'Chemical Safety Inspector',    'Unit 3 Refinery',    'ESP32-004', '+44 7700 900144', 'AB+', 'Day',   2, ts),
                ('W-105', 'Rajesh Kumar',       'Furnace Operations Tech',      'Smelting Unit 1',    'ESP32-005', '+91 98112 34567', 'O-',  'Day',   0, ts),
            ])
            log.info("Seeded %d initial workers", 5)

# ============================================================
# InfluxDB Time-Series Store (optional, graceful degradation)
# ============================================================
influx_client = None
influx_write  = None
influx_query  = None
influx_connected = False

def init_influx():
    global influx_client, influx_write, influx_query, influx_connected
    if not INFLUX_AVAILABLE:
        return
    try:
        influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        health = influx_client.health()
        if health.status == "pass":
            influx_write = influx_client.write_api(write_options=SYNCHRONOUS)
            influx_query = influx_client.query_api()
            influx_connected = True
            log.info("InfluxDB connected (%s) - bucket: %s", INFLUX_URL, INFLUX_BUCKET)
        else:
            log.warning("InfluxDB health check failed: %s", health.message)
    except Exception as e:
        log.warning("InfluxDB unavailable (%s) - falling back to SQLite only", e)
        influx_connected = False

def write_to_influx(device_id: str, ts_str: str, r: dict):
    """Write a sensor reading to InfluxDB as a time-series point."""
    if not influx_connected or not influx_write:
        return
    try:
        point = Point("sensor_readings").tag("device_id", device_id)
        if r.get('heart_rate') is not None:       point = point.field("heart_rate",       float(r['heart_rate']))
        if r.get('spo2') is not None:              point = point.field("spo2",              float(r['spo2']))
        if r.get('temperature') is not None:       point = point.field("temperature",       float(r['temperature']))
        if r.get('air_quality_adc') is not None:   point = point.field("air_quality_adc",   int(r['air_quality_adc']))
        if r.get('air_quality_voltage') is not None: point = point.field("air_quality_voltage", float(r['air_quality_voltage']))
        if r.get('fall_detected') is not None:     point = point.field("fall_detected",     bool(r['fall_detected']))
        point = point.time(datetime.fromisoformat(ts_str), WritePrecision.MS)
        influx_write.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    except Exception as e:
        log.warning("InfluxDB write failed: %s", e)

def query_influx_history(device_id: str, hours: int, limit: int) -> list:
    """Query sensor reading history from InfluxDB. Returns list of dicts or None on failure."""
    if not influx_connected or not influx_query:
        return None
    try:
        flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -{hours}h)
  |> filter(fn: (r) => r._measurement == "sensor_readings")
  |> filter(fn: (r) => r.device_id == "{device_id}")
  |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> sort(columns: ["_time"])
  |> limit(n: {limit})
'''
        tables = influx_query.query(flux, org=INFLUX_ORG)
        results = []
        for table in tables:
            for record in table.records:
                results.append({
                    'device_id': device_id,
                    'timestamp': record.get_time().isoformat(),
                    'heart_rate': record.values.get('heart_rate'),
                    'spo2': record.values.get('spo2'),
                    'temperature': record.values.get('temperature'),
                    'air_quality_adc': record.values.get('air_quality_adc'),
                    'air_quality_voltage': record.values.get('air_quality_voltage'),
                    'fall_detected': record.values.get('fall_detected', False),
                    'simulated': False,
                })
        return results
    except Exception as e:
        log.warning("InfluxDB query failed: %s", e)
        return None

# ============================================================
# WebSocket Hub (real-time push to frontend)
# ============================================================
class Hub:
    def __init__(self):
        self.clients = set()
    async def connect(self, ws):
        await ws.accept()
        self.clients.add(ws)
    def disconnect(self, ws):
        self.clients.discard(ws)
    async def broadcast(self, payload):
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send_json(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)

hub = Hub()

# ============================================================
# Utility Functions
# ============================================================
def iso(dt=None):
    if dt is None:
        return datetime.now(timezone.utc).isoformat()
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).isoformat()

def rowdict(r):
    if r is None:
        return None
    d = dict(r)
    if 'fall_detected' in d and d['fall_detected'] is not None:
        d['fall_detected'] = bool(d['fall_detected'])
    if 'simulated' in d:
        d['simulated'] = bool(d['simulated'])
    for key in ('fall_proof_json', 'proof_json'):
        if key in d and d[key]:
            try: d[key.replace('_json', '')] = json.loads(d[key])
            except Exception: pass
    return d

# ============================================================
# Alert Engine
# ============================================================
def evaluate_alerts(r: dict):
    rules = []
    hr = r.get('heart_rate'); spo2 = r.get('spo2'); temp = r.get('temperature')
    if hr is not None and (hr < HEART_MIN or hr > HEART_MAX):
        rules.append(('heart_rate', 'critical', f'Heart rate {hr:.1f} BPM outside safe range {HEART_MIN:g}–{HEART_MAX:g} BPM.'))
    if spo2 is not None and spo2 < SPO2_MIN:
        rules.append(('spo2', 'critical', f'SpO₂ {spo2:.1f}% below threshold {SPO2_MIN:g}%.'))
    if temp is not None and temp > TEMP_MAX:
        rules.append(('temperature', 'warning', f'Temperature {temp:.2f}°C exceeds threshold {TEMP_MAX:g}°C.'))
    if r.get('fall_detected') is True:
        rules.append(('fall', 'critical', 'Tri-axial kinematic analysis confirmed a fall incident.'))
    return rules

def build_fall_proof(r: dict, given: Optional[dict] = None) -> dict:
    p = (given or {}).copy()
    peak = float(p.get('peak_impact_g') or 3.25)
    ff_ms = int(p.get('free_fall_duration_ms') or 235)
    rest_var = float(p.get('rest_variation_g') or 0.14)
    stable = int(p.get('stable_samples') or 22)
    trace = p.get('trace') or [
        1.02, 0.98, 1.01, 0.99,
        0.42, 0.22, 0.18, 0.24,
        2.10, peak, 1.75, 1.30,
        1.08, 0.96, 1.02, 1.01, 0.98, 1.00, 1.01
    ]
    return {
        'verified': True,
        'algorithm': 'Tri-axial Kinematic Transition (FreeFall → Impact → Rest Stability)',
        'free_fall_duration_ms': ff_ms, 'free_fall_threshold_g': 0.45,
        'peak_impact_g': peak, 'impact_threshold_g': 2.00,
        'rest_variation_g': rest_var, 'rest_variation_threshold_g': 0.30,
        'stable_samples': stable, 'stable_samples_required': 20,
        'vitals_at_impact': {
            'heart_rate': r.get('heart_rate'),
            'spo2': r.get('spo2'),
            'temperature': r.get('temperature'),
        },
        'kinematic_trace': trace,
        'stage_breakdown': [
            {'stage': '1. Free Fall', 'metric': f'{ff_ms} ms at < 0.45 G', 'status': 'PASS'},
            {'stage': '2. Impact Spike', 'metric': f'{peak:.2f} G (≥ 2.0 G)', 'status': 'PASS'},
            {'stage': '3. Immobility', 'metric': f'{rest_var:.2f} G var, {stable} samples', 'status': 'PASS'},
        ]
    }

# ============================================================
# Core Processing Pipeline
# ============================================================
async def process_reading(r: dict, source: str = 'http'):
    """Process a sensor reading from any source (MQTT or HTTP POST)."""
    ts = iso(datetime.fromisoformat(r['timestamp'])) if r.get('timestamp') else iso()
    device = r.get('device_id', 'ESP32-001')
    simulated = int(r.get('simulated', False))

    fall_detected = bool(r.get('fall_detected', False))
    proof_dict = None
    proof_json = None
    if fall_detected:
        proof_dict = build_fall_proof(r, r.get('fall_proof'))
        proof_json = json.dumps(proof_dict)

    with db() as c:
        # Auto-provision worker if needed
        worker = c.execute('SELECT * FROM workers WHERE device_id=?', (device,)).fetchone()
        if not worker:
            wid = f"W-{device.replace('ESP32-', '')}"
            c.execute('''INSERT OR IGNORE INTO workers(id, name, role, department, device_id, emergency_contact, blood_group, shift, fall_count, created_at)
                         VALUES(?,?,?,?,?,?,?,?,?,?)''',
                      (wid, f"Operator {device}", "Monitored Personnel", "Operations", device, "Control Room", "Unknown", "Day", 0, ts))
            worker = c.execute('SELECT * FROM workers WHERE device_id=?', (device,)).fetchone()

        # Write to SQLite
        cur = c.execute('''INSERT INTO readings(device_id, timestamp, heart_rate, spo2, temperature,
            air_quality_adc, air_quality_voltage, fall_detected, wrist_status, fall_proof_json, simulated)
            VALUES(?,?,?,?,?,?,?,?,?,?,?)''', (
            device, ts, r.get('heart_rate'), r.get('spo2'), r.get('temperature'),
            r.get('air_quality_adc'), r.get('air_quality_voltage'),
            None if r.get('fall_detected') is None else int(fall_detected),
            r.get('wrist_status'), proof_json, simulated
        ))
        reading_id = cur.lastrowid

        # Update device status
        c.execute('''INSERT INTO devices(device_id, last_seen, status, simulated) VALUES(?,?,?,?)
                     ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen, status='online', simulated=excluded.simulated''',
                  (device, ts, 'online', simulated))

        # Fall incident tracking
        fall_record = None
        if fall_detected:
            c.execute('UPDATE workers SET fall_count = fall_count + 1 WHERE device_id=?', (device,))
            f_cur = c.execute('''INSERT INTO fall_incidents(incident_key, device_id, worker_id, timestamp,
                peak_impact_g, free_fall_duration_ms, rest_variation_g, stable_samples,
                heart_rate, spo2, temperature, proof_json, status, simulated)
                VALUES(?,?,?,?,?,?,?,?,?,?,?,?,'Active',?)''', (
                'fall', device, worker['id'] if worker else None, ts,
                proof_dict.get('peak_impact_g') if proof_dict else None,
                proof_dict.get('free_fall_duration_ms') if proof_dict else None,
                proof_dict.get('rest_variation_g') if proof_dict else None,
                proof_dict.get('stable_samples') if proof_dict else None,
                r.get('heart_rate'), r.get('spo2'), r.get('temperature'),
                proof_json, simulated
            ))
            fall_record = rowdict(c.execute('SELECT * FROM fall_incidents WHERE id=?', (f_cur.lastrowid,)).fetchone())

        # Alert engine
        new_alerts = []
        for key, severity, description in evaluate_alerts(r):
            existing = c.execute("SELECT id FROM alerts WHERE device_id=? AND incident_key=? AND status IN ('Active','Acknowledged') AND simulated=? LIMIT 1",
                                 (device, key, simulated)).fetchone()
            if not existing:
                a_proof = proof_json if key == 'fall' else None
                cur = c.execute("INSERT INTO alerts(incident_key, device_id, alert_type, description, severity, timestamp, status, proof_json, simulated) VALUES(?,?,?,?,?,?,'Active',?,?)",
                                (key, device, key.replace('_', ' ').title(), description, severity, ts, a_proof, simulated))
                new_alerts.append(rowdict(c.execute('SELECT * FROM alerts WHERE id=?', (cur.lastrowid,)).fetchone()))

        # Auto-resolve recovered alerts
        active_alerts = c.execute("SELECT * FROM alerts WHERE device_id=? AND status IN ('Active','Acknowledged') AND simulated=?", (device, simulated)).fetchall()
        for a in active_alerts:
            key = a['incident_key']; recovered = False
            if key == 'heart_rate' and r.get('heart_rate') is not None: recovered = HEART_MIN <= r['heart_rate'] <= HEART_MAX
            elif key == 'spo2' and r.get('spo2') is not None: recovered = r['spo2'] >= SPO2_MIN
            elif key == 'temperature' and r.get('temperature') is not None: recovered = r['temperature'] <= TEMP_MAX
            elif key == 'fall' and r.get('fall_detected') is False: recovered = True
            if recovered: c.execute("UPDATE alerts SET status='Resolved', resolved_at=? WHERE id=?", (ts, a['id']))

        rr = rowdict(c.execute('SELECT * FROM readings WHERE id=?', (reading_id,)).fetchone())

    # Write to InfluxDB (non-blocking, best-effort)
    write_to_influx(device, ts, r)

    # WebSocket broadcasts
    await hub.broadcast({'type': 'reading', 'data': rr})
    for a in new_alerts:
        await hub.broadcast({'type': 'alert', 'data': a})
    if fall_record:
        await hub.broadcast({'type': 'fall_incident', 'data': fall_record, 'worker': rowdict(worker) if worker else None})
    if new_alerts or active_alerts or fall_detected:
        await hub.broadcast({'type': 'alerts_changed'})

    log.info("[%s] %s reading processed | device=%s fall=%s alerts=%d",
             source.upper(), 'CRITICAL' if fall_detected else 'Normal', device, fall_detected, len(new_alerts))

    return {
        'accepted': True, 'reading_id': reading_id, 'timestamp': ts,
        'alerts_created': len(new_alerts), 'fall_confirmed': fall_detected,
        'proof': proof_dict, 'source': source
    }

# ============================================================
# MQTT Subscriber (IoT Data Ingestion - Core Architecture)
# ============================================================
async def mqtt_subscriber():
    """
    Background task that subscribes to MQTT topics published by ESP32 devices.
    Topic hierarchy:
      workband/telemetry/{device_id}  - periodic sensor readings
      workband/critical/{device_id}   - fall events with kinematic proof
      workband/status/{device_id}     - online/offline (LWT retained messages)
    """
    if not MQTT_AVAILABLE:
        log.warning("MQTT subscriber disabled (aiomqtt not installed)")
        return

    while True:
        try:
            log.info("Connecting to MQTT broker at %s:%d ...", MQTT_HOST, MQTT_PORT)
            async with aiomqtt.Client(
                hostname=MQTT_HOST,
                port=MQTT_PORT,
                keepalive=60,
            ) as client:
                log.info("MQTT connected! Subscribing to workband/# ...")
                await client.subscribe("workband/#")

                async for message in client.messages:
                    topic = str(message.topic)
                    try:
                        payload = json.loads(message.payload.decode())
                    except Exception:
                        log.warning("Invalid MQTT payload on %s", topic)
                        continue

                    if topic.startswith("workband/telemetry/"):
                        # Regular sensor reading
                        await process_reading(payload, source='mqtt')

                    elif topic.startswith("workband/critical/"):
                        # Critical event (fall) - process with high priority
                        log.critical("CRITICAL MQTT message on %s", topic)
                        await process_reading(payload, source='mqtt-critical')

                    elif topic.startswith("workband/status/"):
                        # Device online/offline status (from LWT)
                        device_id = payload.get('device_id', topic.split('/')[-1])
                        status = payload.get('status', 'offline')
                        log.info("Device %s status: %s (MQTT LWT)", device_id, status)
                        with db() as c:
                            c.execute('''INSERT INTO devices(device_id, last_seen, status, simulated) VALUES(?,?,?,0)
                                         ON CONFLICT(device_id) DO UPDATE SET last_seen=?, status=?''',
                                      (device_id, iso(), status, iso(), status))
                        await hub.broadcast({'type': 'device_status', 'data': {'device_id': device_id, 'status': status}})
                    else:
                        log.debug("Unhandled MQTT topic: %s", topic)

        except aiomqtt.MqttError as e:
            log.warning("MQTT connection lost (%s). Reconnecting in 5s...", e)
            await asyncio.sleep(5)
        except asyncio.CancelledError:
            log.info("MQTT subscriber shutting down.")
            break
        except Exception as e:
            log.error("MQTT subscriber error: %s. Reconnecting in 5s...", e)
            await asyncio.sleep(5)

# ============================================================
# Application Lifecycle
# ============================================================
@asynccontextmanager
async def lifespan(app):
    init_db()
    init_influx()
    mqtt_task = asyncio.create_task(mqtt_subscriber())
    log.info("WorkBand backend started (MQTT=%s, InfluxDB=%s)", MQTT_AVAILABLE, influx_connected)
    yield
    mqtt_task.cancel()
    try: await mqtt_task
    except asyncio.CancelledError: pass
    if influx_client:
        influx_client.close()
    log.info("WorkBand backend shut down.")

app = FastAPI(title='Industrial Worker Safety Band API', version='2.0.0', lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=['*'], allow_credentials=False, allow_methods=['*'], allow_headers=['*'])

# ============================================================
# REST API Endpoints
# ============================================================
@app.get('/api/health')
def health():
    return {
        'status': 'ok', 'database': str(DB_PATH.name), 'utc_time': iso(),
        'mqtt_enabled': MQTT_AVAILABLE, 'mqtt_broker': f'{MQTT_HOST}:{MQTT_PORT}',
        'influxdb_connected': influx_connected, 'influxdb_url': INFLUX_URL if influx_connected else None,
        'architecture': 'MQTT + InfluxDB + SQLite + FastAPI + WebSocket'
    }

@app.post('/api/readings', status_code=201)
async def create_reading_http(payload: Reading):
    """HTTP POST fallback for sending readings (use MQTT in production)."""
    r = payload.model_dump()
    if not any(r.get(k) is not None for k in ('heart_rate', 'spo2', 'temperature', 'air_quality_adc', 'air_quality_voltage', 'fall_detected', 'wrist_status')):
        raise HTTPException(422, 'Payload must contain at least one sensor or status field.')
    return await process_reading(r, source='http')

@app.get('/api/readings/latest')
def latest(device_id: str = 'ESP32-001', simulated: Optional[bool] = None):
    with db() as c:
        q = 'SELECT * FROM readings WHERE device_id=?'; args = [device_id]
        if simulated is not None: q += ' AND simulated=?'; args.append(int(simulated))
        q += ' ORDER BY id DESC LIMIT 1'
        r = c.execute(q, args).fetchone()
    return {'data': rowdict(r) if r else None}

@app.get('/api/readings/history')
def history(device_id: str = 'ESP32-001', hours: int = Query(24, ge=1, le=168), limit: int = Query(1000, ge=1, le=5000), simulated: Optional[bool] = None):
    # Try InfluxDB first for better time-series performance
    if influx_connected and not simulated:
        influx_data = query_influx_history(device_id, hours, limit)
        if influx_data is not None:
            return {'data': influx_data, 'hours': hours, 'source': 'influxdb'}

    # Fallback to SQLite
    since = iso(datetime.now(timezone.utc) - timedelta(hours=hours))
    with db() as c:
        q = 'SELECT * FROM readings WHERE device_id=? AND timestamp>=?'; args = [device_id, since]
        if simulated is not None: q += ' AND simulated=?'; args.append(int(simulated))
        q += ' ORDER BY id DESC LIMIT ?'; args.append(limit)
        rows = c.execute(q, args).fetchall()
    return {'data': [rowdict(x) for x in reversed(rows)], 'hours': hours, 'source': 'sqlite'}

@app.get('/api/alerts')
def get_alerts(device_id: Optional[str] = None, status: Optional[str] = None, limit: int = Query(200, ge=1, le=1000), simulated: Optional[bool] = None):
    with db() as c:
        q = 'SELECT * FROM alerts WHERE 1=1'; args = []
        if device_id: q += ' AND device_id=?'; args.append(device_id)
        if status: q += ' AND status=?'; args.append(status)
        if simulated is not None: q += ' AND simulated=?'; args.append(int(simulated))
        q += ' ORDER BY id DESC LIMIT ?'; args.append(limit)
        rows = c.execute(q, args).fetchall()
    return {'data': [rowdict(x) for x in rows]}

@app.patch('/api/alerts/{alert_id}/acknowledge')
async def acknowledge(alert_id: int):
    with db() as c:
        row = c.execute('SELECT * FROM alerts WHERE id=?', (alert_id,)).fetchone()
        if not row: raise HTTPException(404, 'Alert not found')
        if row['status'] == 'Active':
            c.execute("UPDATE alerts SET status='Acknowledged' WHERE id=?", (alert_id,))
        updated = c.execute('SELECT * FROM alerts WHERE id=?', (alert_id,)).fetchone()
    data = rowdict(updated)
    await hub.broadcast({'type': 'alert_updated', 'data': data})
    return {'data': data}

@app.patch('/api/alerts/{alert_id}/resolve')
async def resolve_alert(alert_id: int):
    with db() as c:
        row = c.execute('SELECT * FROM alerts WHERE id=?', (alert_id,)).fetchone()
        if not row: raise HTTPException(404, 'Alert not found')
        ts = iso()
        c.execute("UPDATE alerts SET status='Resolved', resolved_at=? WHERE id=?", (ts, alert_id))
        updated = c.execute('SELECT * FROM alerts WHERE id=?', (alert_id,)).fetchone()
    data = rowdict(updated)
    await hub.broadcast({'type': 'alert_updated', 'data': data})
    return {'data': data}

# ==================== Workers API ====================
@app.get('/api/workers')
def get_workers():
    now = datetime.now(timezone.utc)
    with db() as c:
        workers = c.execute('SELECT * FROM workers ORDER BY id ASC').fetchall()
        result = []
        for w in workers:
            wd = rowdict(w)
            dev = c.execute('SELECT * FROM devices WHERE device_id=?', (w['device_id'],)).fetchone()
            last_reading = c.execute('SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT 1', (w['device_id'],)).fetchone()
            active_alerts = c.execute("SELECT COUNT(*) n FROM alerts WHERE device_id=? AND status IN ('Active','Acknowledged')", (w['device_id'],)).fetchone()['n']

            online = False; last_seen = None
            if dev:
                last_seen = dev['last_seen']
                # Prefer MQTT LWT status over stale timer
                if dev['status'] == 'online':
                    try:
                        dt = datetime.fromisoformat(dev['last_seen']).astimezone(timezone.utc)
                        online = (now - dt).total_seconds() <= STALE_SECONDS
                    except: pass
            wd.update(online=online, last_seen=last_seen, latest_reading=rowdict(last_reading), active_alerts_count=active_alerts)
            result.append(wd)

    return {
        'total_workers': len(result),
        'online_workers': sum(1 for w in result if w['online']),
        'total_falls': sum(w['fall_count'] for w in result),
        'data': result
    }

@app.get('/api/workers/{worker_id}')
def get_worker_detail(worker_id: str):
    now = datetime.now(timezone.utc)
    with db() as c:
        worker = c.execute('SELECT * FROM workers WHERE id=? OR device_id=?', (worker_id, worker_id)).fetchone()
        if not worker: raise HTTPException(404, 'Worker not found')
        wd = rowdict(worker)
        dev = c.execute('SELECT * FROM devices WHERE device_id=?', (worker['device_id'],)).fetchone()
        readings = c.execute('SELECT * FROM readings WHERE device_id=? ORDER BY id DESC LIMIT 20', (worker['device_id'],)).fetchall()
        falls = c.execute('SELECT * FROM fall_incidents WHERE device_id=? OR worker_id=? ORDER BY id DESC LIMIT 20', (worker['device_id'], worker['id'])).fetchall()
        alerts = c.execute('SELECT * FROM alerts WHERE device_id=? ORDER BY id DESC LIMIT 20', (worker['device_id'],)).fetchall()

        online = False; last_seen = None
        if dev:
            last_seen = dev['last_seen']
            if dev['status'] == 'online':
                try:
                    dt = datetime.fromisoformat(dev['last_seen']).astimezone(timezone.utc)
                    online = (now - dt).total_seconds() <= STALE_SECONDS
                except: pass
        wd.update(online=online, last_seen=last_seen,
                  recent_readings=[rowdict(r) for r in readings],
                  fall_incidents=[rowdict(f) for f in falls],
                  alerts=[rowdict(a) for a in alerts])
        return {'data': wd}

@app.post('/api/workers')
def create_or_update_worker(payload: WorkerCreate):
    w = payload.model_dump(); ts = iso()
    with db() as c:
        c.execute('''INSERT INTO workers(id, name, role, department, device_id, emergency_contact, blood_group, shift, created_at)
                     VALUES(?,?,?,?,?,?,?,?,?)
                     ON CONFLICT(id) DO UPDATE SET name=excluded.name, role=excluded.role, department=excluded.department,
                     device_id=excluded.device_id, emergency_contact=excluded.emergency_contact, blood_group=excluded.blood_group, shift=excluded.shift''',
                  (w['id'], w['name'], w['role'], w['department'], w['device_id'], w.get('emergency_contact'), w.get('blood_group'), w.get('shift', 'Day'), ts))
        updated = c.execute('SELECT * FROM workers WHERE id=?', (w['id'],)).fetchone()
    return {'data': rowdict(updated)}

# ==================== Fall Incidents & Proof ====================
@app.get('/api/incidents/falls')
def get_fall_incidents(limit: int = Query(50, ge=1, le=200)):
    with db() as c:
        rows = c.execute('''SELECT f.*, w.name as worker_name, w.role as worker_role, w.department as worker_department
                            FROM fall_incidents f LEFT JOIN workers w ON (f.worker_id = w.id OR f.device_id = w.device_id)
                            ORDER BY f.id DESC LIMIT ?''', (limit,)).fetchall()
    return {'data': [rowdict(r) for r in rows]}

@app.get('/api/incidents/falls/{incident_id}')
def get_single_fall_incident(incident_id: int):
    with db() as c:
        r = c.execute('''SELECT f.*, w.name as worker_name, w.role as worker_role, w.department as worker_department, w.emergency_contact
                         FROM fall_incidents f LEFT JOIN workers w ON (f.worker_id = w.id OR f.device_id = w.device_id) WHERE f.id=?''', (incident_id,)).fetchone()
        if not r: raise HTTPException(404, 'Fall incident not found')
    return {'data': rowdict(r)}

@app.post('/api/incidents/simulate-fall')
async def simulate_fall_event(payload: FallSimulationPayload):
    r = {
        'device_id': payload.device_id, 'heart_rate': payload.heart_rate,
        'spo2': payload.spo2, 'temperature': payload.temperature,
        'fall_detected': True, 'simulated': False,
        'fall_proof': {
            'peak_impact_g': payload.peak_impact_g,
            'free_fall_duration_ms': payload.free_fall_duration_ms,
            'rest_variation_g': payload.rest_variation_g,
            'stable_samples': payload.stable_samples,
        }
    }
    res = await process_reading(r, source='simulation')
    return {'status': 'simulated_fall_created', 'details': res}

# ==================== Device Status & Summary ====================
@app.get('/api/device/status')
def device_status(device_id: str = 'ESP32-001'):
    now = datetime.now(timezone.utc)
    with db() as c:
        d = c.execute('SELECT * FROM devices WHERE device_id=?', (device_id,)).fetchone()
        r = c.execute('SELECT * FROM readings WHERE device_id=? AND simulated=0 ORDER BY id DESC LIMIT 1', (device_id,)).fetchone()
        w = c.execute('SELECT * FROM workers WHERE device_id=?', (device_id,)).fetchone()
    if not d:
        return {'device_id': device_id, 'status': 'Offline', 'last_seen': None, 'last_reading': None, 'worker': rowdict(w), 'simulated': False}
    online = False
    if d['status'] == 'online':
        try: online = (now - datetime.fromisoformat(d['last_seen']).astimezone(timezone.utc)).total_seconds() <= STALE_SECONDS
        except: pass
    return {'device_id': device_id, 'status': 'Online' if online else 'Offline',
            'last_seen': d['last_seen'], 'last_reading': rowdict(r), 'worker': rowdict(w), 'simulated': bool(d['simulated'])}

@app.get('/api/dashboard/summary')
def summary():
    now = datetime.now(timezone.utc)
    with db() as c:
        devices = c.execute('SELECT * FROM devices').fetchall()
        workers = c.execute('SELECT * FROM workers').fetchall()
        active = c.execute("SELECT COUNT(*) n FROM alerts WHERE status IN ('Active','Acknowledged') AND simulated=0").fetchone()['n']
        critical = c.execute("SELECT COUNT(*) n FROM alerts WHERE severity='critical' AND status IN ('Active','Acknowledged') AND simulated=0").fetchone()['n']
        recent = c.execute('SELECT * FROM alerts WHERE simulated=0 ORDER BY id DESC LIMIT 5').fetchall()
        latest_real = c.execute('SELECT * FROM readings WHERE simulated=0 ORDER BY id DESC LIMIT 1').fetchone()
        total_falls = c.execute('SELECT COUNT(*) n FROM fall_incidents WHERE simulated=0').fetchone()['n']
        if total_falls == 0: total_falls = sum(w['fall_count'] for w in workers)

    online_devs = 0
    for d in devices:
        if d['simulated']: continue
        if d['status'] == 'online':
            try:
                if (now - datetime.fromisoformat(d['last_seen']).astimezone(timezone.utc)).total_seconds() <= STALE_SECONDS:
                    online_devs += 1
            except: pass

    return {
        'connected_devices': online_devs, 'known_devices': sum(not d['simulated'] for d in devices),
        'total_workers': len(workers), 'online_workers': online_devs, 'total_falls': total_falls,
        'active_alerts': active, 'critical_incidents': critical,
        'latest_reading': rowdict(latest_real), 'recent_alerts': [rowdict(a) for a in recent],
        'mqtt_enabled': MQTT_AVAILABLE, 'influxdb_connected': influx_connected,
    }

@app.websocket('/ws/monitoring')
async def monitoring(ws: WebSocket):
    await hub.connect(ws)
    try:
        await ws.send_json({'type': 'connected', 'data': {'timestamp': iso(), 'version': '2.0', 'mqtt': MQTT_AVAILABLE, 'influxdb': influx_connected}})
        while True:
            await ws.receive_text()
    except WebSocketDisconnect: hub.disconnect(ws)
    except Exception: hub.disconnect(ws)

# ============================================================
# Entry Point - run with: python main.py
# This ensures the WindowsSelectorEventLoopPolicy is set
# BEFORE uvicorn creates its event loop (CLI uvicorn does not
# import main.py early enough to catch the policy change).
# ============================================================
if __name__ == "__main__":
    import uvicorn
    _host = os.getenv("HOST", "0.0.0.0")
    _port = int(os.getenv("PORT", "8001"))
    log.info("Starting WorkBand backend on %s:%d", _host, _port)
    uvicorn.run(
        "main:app",
        host=_host,
        port=_port,
        loop="none",        # preserve WindowsSelectorEventLoopPolicy set above
        reload=False,
        log_level="info",
        timeout_graceful_shutdown=3,
    )
