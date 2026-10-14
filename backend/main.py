import asyncio, json, os, sqlite3, time
from contextlib import asynccontextmanager
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Optional
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field, ConfigDict

DB_PATH = Path(os.getenv('SAFETY_DB_PATH', Path(__file__).with_name('safety_band.db')))
STALE_SECONDS = int(os.getenv('DEVICE_STALE_SECONDS', '15'))
HEART_MIN = float(os.getenv('HEART_MIN', '45'))
HEART_MAX = float(os.getenv('HEART_MAX', '130'))
SPO2_MIN = float(os.getenv('SPO2_MIN', '90'))
TEMP_MAX = float(os.getenv('TEMP_MAX', '38'))
# MQ-135 is stored as raw ADC. No validated danger threshold is assumed.

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
    wrist_status: Optional[str] = Field(default=None, max_length=32)
    simulated: bool = False

def db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    with db() as c:
        c.execute('''CREATE TABLE IF NOT EXISTS readings(id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL, timestamp TEXT NOT NULL, heart_rate REAL, spo2 REAL, temperature REAL, air_quality_adc INTEGER, air_quality_voltage REAL, fall_detected INTEGER, wrist_status TEXT, simulated INTEGER NOT NULL DEFAULT 0)''')
        c.execute('''CREATE INDEX IF NOT EXISTS idx_readings_device_time ON readings(device_id,timestamp)''')
        c.execute('''CREATE TABLE IF NOT EXISTS alerts(id INTEGER PRIMARY KEY AUTOINCREMENT, incident_key TEXT NOT NULL, device_id TEXT NOT NULL, alert_type TEXT NOT NULL, description TEXT NOT NULL, severity TEXT NOT NULL, timestamp TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'Active', resolved_at TEXT, simulated INTEGER NOT NULL DEFAULT 0)''')
        c.execute('CREATE INDEX IF NOT EXISTS idx_alerts_active ON alerts(device_id,incident_key,status)')
        c.execute('''CREATE TABLE IF NOT EXISTS devices(device_id TEXT PRIMARY KEY, last_seen TEXT NOT NULL, simulated INTEGER NOT NULL DEFAULT 0)''')

class Hub:
    def __init__(self): self.clients=set()
    async def connect(self, ws):
        await ws.accept(); self.clients.add(ws)
    def disconnect(self, ws): self.clients.discard(ws)
    async def broadcast(self, payload):
        dead=[]
        for ws in list(self.clients):
            try: await ws.send_json(payload)
            except Exception: dead.append(ws)
        for ws in dead: self.disconnect(ws)
hub=Hub()

@asynccontextmanager
async def lifespan(app):
    init_db(); yield
app=FastAPI(title='Industrial Worker Safety Band API', version='1.0.0', lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=os.getenv('CORS_ORIGINS','http://localhost:5173,http://127.0.0.1:5173').split(','), allow_credentials=False, allow_methods=['*'], allow_headers=['*'])

def iso(dt=None):
    if dt is None: return datetime.now(timezone.utc).isoformat()
    if dt.tzinfo is None: dt=dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).isoformat()

def rowdict(r):
    d=dict(r)
    if 'fall_detected' in d and d['fall_detected'] is not None: d['fall_detected']=bool(d['fall_detected'])
    if 'simulated' in d: d['simulated']=bool(d['simulated'])
    return d

def evaluate_alerts(r: dict):
    rules=[]
    hr=r.get('heart_rate'); spo2=r.get('spo2'); temp=r.get('temperature')
    if hr is not None and (hr < HEART_MIN or hr > HEART_MAX): rules.append(('heart_rate','critical',f'Heart rate {hr:.1f} BPM is outside configured demo range {HEART_MIN:g}–{HEART_MAX:g} BPM.'))
    if spo2 is not None and spo2 < SPO2_MIN: rules.append(('spo2','critical',f'SpO₂ {spo2:.1f}% is below configured threshold {SPO2_MIN:g}%.'))
    if temp is not None and temp > TEMP_MAX: rules.append(('temperature','warning',f'Temperature {temp:.2f} °C exceeds configured threshold {TEMP_MAX:g} °C.'))
    if r.get('fall_detected') is True: rules.append(('fall','critical','Firmware reported a confirmed fall.'))
    # MQ-135 is intentionally not thresholded: raw ADC alone is not calibrated gas concentration.
    return rules

@app.get('/api/health')
def health(): return {'status':'ok','database':str(DB_PATH.name),'utc_time':iso()}

@app.post('/api/readings', status_code=201)
async def create_reading(payload: Reading):
    r=payload.model_dump(); ts=iso(r.get('timestamp')); device=r['device_id']
    if not any(r.get(k) is not None for k in ('heart_rate','spo2','temperature','air_quality_adc','air_quality_voltage','fall_detected','wrist_status')):
        raise HTTPException(422,'Payload must contain at least one sensor or status field.')
    with db() as c:
        cur=c.execute('INSERT INTO readings(device_id,timestamp,heart_rate,spo2,temperature,air_quality_adc,air_quality_voltage,fall_detected,wrist_status,simulated) VALUES(?,?,?,?,?,?,?,?,?,?)',(device,ts,r.get('heart_rate'),r.get('spo2'),r.get('temperature'),r.get('air_quality_adc'),r.get('air_quality_voltage'),None if r.get('fall_detected') is None else int(r['fall_detected']),r.get('wrist_status'),int(r['simulated'])))
        reading_id=cur.lastrowid
        c.execute('INSERT INTO devices(device_id,last_seen,simulated) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen,simulated=excluded.simulated',(device,ts,int(r['simulated'])))
        new_alerts=[]
        for key,severity,description in evaluate_alerts(r):
            active=c.execute('SELECT id FROM alerts WHERE device_id=? AND incident_key=? AND status IN (\'Active\',\'Acknowledged\') AND simulated=? ORDER BY id DESC LIMIT 1',(device,key,int(r['simulated']))).fetchone()
            if not active:
                cur=c.execute('INSERT INTO alerts(incident_key,device_id,alert_type,description,severity,timestamp,status,simulated) VALUES(?,?,?,?,?,?,\'Active\',?)',(key,device,key.replace('_',' ').title(),description,severity,ts,int(r['simulated'])))
                a=c.execute('SELECT * FROM alerts WHERE id=?',(cur.lastrowid,)).fetchone(); new_alerts.append(rowdict(a))
        # Resolve active incidents only when a valid reading demonstrates recovery.
        active_rows=c.execute('SELECT * FROM alerts WHERE device_id=? AND status IN (\'Active\',\'Acknowledged\') AND simulated=?',(device,int(r['simulated']))).fetchall()
        for a in active_rows:
            key=a['incident_key']; recovered=False
            if key=='heart_rate' and r.get('heart_rate') is not None: recovered=HEART_MIN<=r['heart_rate']<=HEART_MAX
            elif key=='spo2' and r.get('spo2') is not None: recovered=r['spo2']>=SPO2_MIN
            elif key=='temperature' and r.get('temperature') is not None: recovered=r['temperature']<=TEMP_MAX
            elif key=='fall' and r.get('fall_detected') is False: recovered=True
            if recovered: c.execute('UPDATE alerts SET status=\'Resolved\',resolved_at=? WHERE id=?',(ts,a['id']))
        rr=c.execute('SELECT * FROM readings WHERE id=?',(reading_id,)).fetchone(); latest=rowdict(rr)
    await hub.broadcast({'type':'reading','data':latest})
    for a in new_alerts: await hub.broadcast({'type':'alert','data':a})
    if new_alerts or active_rows: await hub.broadcast({'type':'alerts_changed'})
    return {'accepted':True,'reading_id':reading_id,'timestamp':ts,'alerts_created':len(new_alerts)}

@app.get('/api/readings/latest')
def latest(device_id: str='ESP32-001', simulated: Optional[bool]=None):
    with db() as c:
        q='SELECT * FROM readings WHERE device_id=?'; args=[device_id]
        if simulated is not None: q+=' AND simulated=?'; args.append(int(simulated))
        q+=' ORDER BY id DESC LIMIT 1'; r=c.execute(q,args).fetchone()
    return {'data':rowdict(r) if r else None}

@app.get('/api/readings/history')
def history(device_id: str='ESP32-001', hours: int=Query(24,ge=1,le=168), limit: int=Query(1000,ge=1,le=5000), simulated: Optional[bool]=None):
    since=iso(datetime.now(timezone.utc)-timedelta(hours=hours))
    with db() as c:
        q='SELECT * FROM readings WHERE device_id=? AND timestamp>=?'; args=[device_id,since]
        if simulated is not None: q+=' AND simulated=?'; args.append(int(simulated))
        q+=' ORDER BY id DESC LIMIT ?'; args.append(limit)
        rows=c.execute(q,args).fetchall()
    return {'data':[rowdict(x) for x in reversed(rows)],'hours':hours}

@app.get('/api/alerts')
def get_alerts(device_id: Optional[str]=None, status: Optional[str]=None, limit: int=Query(200,ge=1,le=1000), simulated: Optional[bool]=None):
    with db() as c:
        q='SELECT * FROM alerts WHERE 1=1'; args=[]
        if device_id: q+=' AND device_id=?'; args.append(device_id)
        if status: q+=' AND status=?'; args.append(status)
        if simulated is not None: q+=' AND simulated=?'; args.append(int(simulated))
        q+=' ORDER BY id DESC LIMIT ?'; args.append(limit)
        rows=c.execute(q,args).fetchall()
    return {'data':[rowdict(x) for x in rows]}

@app.patch('/api/alerts/{alert_id}/acknowledge')
async def acknowledge(alert_id: int):
    with db() as c:
        row=c.execute('SELECT * FROM alerts WHERE id=?',(alert_id,)).fetchone()
        if not row: raise HTTPException(404,'Alert not found')
        if row['status']=='Active': c.execute('UPDATE alerts SET status=\'Acknowledged\' WHERE id=?',(alert_id,))
        updated=c.execute('SELECT * FROM alerts WHERE id=?',(alert_id,)).fetchone()
    data=rowdict(updated); await hub.broadcast({'type':'alert_updated','data':data})
    return {'data':data}

@app.get('/api/device/status')
def device_status(device_id: str='ESP32-001'):
    with db() as c:
        d=c.execute('SELECT * FROM devices WHERE device_id=?',(device_id,)).fetchone()
        r=c.execute('SELECT * FROM readings WHERE device_id=? AND simulated=0 ORDER BY id DESC LIMIT 1',(device_id,)).fetchone()
    if not d: return {'device_id':device_id,'status':'Offline','last_seen':None,'last_reading':None,'simulated':False}
    last=datetime.fromisoformat(d['last_seen']); online=(datetime.now(timezone.utc)-last.astimezone(timezone.utc)).total_seconds()<=STALE_SECONDS
    return {'device_id':device_id,'status':'Online' if online else 'Offline','last_seen':d['last_seen'],'last_reading':rowdict(r) if r else None,'simulated':bool(d['simulated'])}

@app.get('/api/dashboard/summary')
def summary():
    with db() as c:
        devices=c.execute('SELECT * FROM devices').fetchall()
        active=c.execute("SELECT COUNT(*) n FROM alerts WHERE status IN ('Active','Acknowledged') AND simulated=0").fetchone()['n']
        critical=c.execute("SELECT COUNT(*) n FROM alerts WHERE severity='critical' AND status IN ('Active','Acknowledged') AND simulated=0").fetchone()['n']
        recent=c.execute('SELECT * FROM alerts WHERE simulated=0 ORDER BY id DESC LIMIT 5').fetchall()
        latest_real=c.execute('SELECT * FROM readings WHERE simulated=0 ORDER BY id DESC LIMIT 1').fetchone()
    now=datetime.now(timezone.utc); online=sum((now-datetime.fromisoformat(d['last_seen']).astimezone(timezone.utc)).total_seconds()<=STALE_SECONDS for d in devices if not d['simulated'])
    return {'connected_devices':online,'known_devices':sum(not d['simulated'] for d in devices),'active_alerts':active,'critical_incidents':critical,'latest_reading':rowdict(latest_real) if latest_real else None,'recent_alerts':[rowdict(a) for a in recent]}

@app.websocket('/ws/monitoring')
async def monitoring(ws: WebSocket):
    await hub.connect(ws)
    try:
        await ws.send_json({'type':'connected','data':{'timestamp':iso()}})
        while True: await ws.receive_text()
    except WebSocketDisconnect: hub.disconnect(ws)
    except Exception: hub.disconnect(ws)
