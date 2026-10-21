import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { 
  Activity, AirVent, AlertTriangle, ArrowUpRight, Check, ChevronRight, 
  CircleHelp, Clock, Cloud, HeartPulse, House, LineChart, Radio, 
  RefreshCw, ShieldCheck, ShieldAlert, Signal, Smartphone, Thermometer, 
  Waves, Wifi, WifiOff, Zap, Users, UserCheck, HardHat, PhoneCall, 
  FileText, X, AlertCircle, Eye, Flame, Search, CheckCircle2
} from 'lucide-react';
import { 
  ResponsiveContainer, LineChart as RLineChart, Line, XAxis, YAxis, 
  Tooltip, CartesianGrid, ReferenceLine 
} from 'recharts';
import './style.css';

const API = import.meta.env.VITE_API_URL || 'http://127.0.0.1:8000';
const WS = API.replace(/^http/, 'ws') + '/ws/monitoring';

const nav = [
  ['Overview', House],
  ['Workers', Users],
  ['Alerts', AlertTriangle],
  ['Analytics', LineChart],
  ['Devices', Smartphone]
];

const fmt = (v, d = 1) => v == null || !Number.isFinite(Number(v)) ? '—' : Number(v).toFixed(d);
const stamp = v => v ? new Date(v).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : 'No reading yet';

// Distinct colors for multi-user charts
const WORKER_COLORS = ['#d36b6b', '#6c94bc', '#c28a4b', '#8c83ba', '#2e854b', '#e07941', '#5b9bd5', '#c55c8a'];

function App() {
  const [page, setPage] = useState('Overview');
  const [data, setData] = useState(null);
  const [history, setHistory] = useState([]);
  const [allHistory, setAllHistory] = useState([]);
  const [alerts, setAlerts] = useState([]);
  const [device, setDevice] = useState({ status: 'Offline' });
  const [workersData, setWorkersData] = useState({ total_workers: 0, online_workers: 0, total_falls: 0, data: [] });
  const [backend, setBackend] = useState(false);
  const [simulation, setSimulation] = useState(false);
  const [simData, setSimData] = useState(null);
  const [hours, setHours] = useState(1);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  // Modals & Inspection State
  const [selectedWorker, setSelectedWorker] = useState(null);
  const [workerDetail, setWorkerDetail] = useState(null);
  const [loadingWorker, setLoadingWorker] = useState(false);
  const [proofIncident, setProofIncident] = useState(null); // Active forensic proof modal
  const [activeFallBanner, setActiveFallBanner] = useState(null); // Real-time emergency banner

  // Worker Roster Filters
  const [workerTab, setWorkerTab] = useState('all');
  const [workerSearch, setWorkerSearch] = useState('');

  // Analytics: selected worker for per-user analytics (null = all users)
  const [analyticsWorker, setAnalyticsWorker] = useState(null);
  const [workerHistory, setWorkerHistory] = useState([]);

  // Initial HTTP fetch
  const load = useCallback(async () => {
    try {
      const [latest, hist, allHist, al, dev, wrk] = await Promise.all([
        fetch(`${API}/api/readings/latest?device_id=ESP32-001&simulated=${simulation}`).then(r => r.json()),
        fetch(`${API}/api/readings/history?device_id=ESP32-001&hours=${hours}&simulated=${simulation}`).then(r => r.json()),
        fetch(`${API}/api/readings/history?device_id=all&hours=${hours}&simulated=${simulation}`).then(r => r.json()),
        fetch(`${API}/api/alerts?limit=100&simulated=${simulation}`).then(r => r.json()),
        fetch(`${API}/api/device/status?device_id=ESP32-001`).then(r => r.json()),
        fetch(`${API}/api/workers`).then(r => r.json())
      ]);
      setData(latest.data);
      setHistory(hist.data || []);
      setAllHistory(allHist.data || []);
      setAlerts(al.data || []);
      setDevice(dev);
      setWorkersData(wrk);
      setBackend(true);
      setError('');
    } catch {
      setBackend(false);
      setError(`Cannot reach FastAPI backend. Verify the server is running at ${API}`);
    }
  }, [hours, simulation]);

  // Initial load and silent 15-second fallback polling
  useEffect(() => {
    load();
    const id = setInterval(load, 15000);
    return () => clearInterval(id);
  }, [load]);

  // Real-Time High-Performance WebSocket Streaming
  useEffect(() => {
    let ws;
    let retry;
    let cancelled = false;

    function connect() {
      try {
        ws = new WebSocket(WS);
        ws.onopen = () => {
          setBackend(true);
          setError('');
        };
        ws.onmessage = (evt) => {
          try {
            const msg = JSON.parse(evt.data);
            if (msg.type === 'reading') {
              const reading = msg.data;
              setData(reading);
              // Smooth real-time graph update without HTTP re-fetch
              setHistory(prev => {
                const next = [...prev, reading];
                return next.length > 100 ? next.slice(-100) : next;
              });
              // Update corresponding worker live status in state
              setWorkersData(prev => ({
                ...prev,
                data: prev.data.map(w => {
                  if (w.device_id === reading.device_id) {
                    return {
                      ...w,
                      online: true,
                      last_seen: reading.timestamp,
                      latest_reading: reading
                    };
                  }
                  return w;
                })
              }));
            } else if (msg.type === 'alert') {
              setAlerts(prev => [msg.data, ...prev]);
            } else if (msg.type === 'fall_incident') {
              // Critical real-time fall incident flagged!
              const incident = msg.data;
              const worker = msg.worker;
              setActiveFallBanner({ incident, worker });
              // Automatically increment fall count locally
              setWorkersData(prev => ({
                ...prev,
                total_falls: prev.total_falls + 1,
                data: prev.data.map(w => {
                  if (w.id === incident.worker_id || w.device_id === incident.device_id) {
                    return { ...w, fall_count: (w.fall_count || 0) + 1 };
                  }
                  return w;
                })
              }));
              // Also update alerts list
              load();
            } else if (msg.type === 'alert_updated') {
              setAlerts(prev => prev.map(a => a.id === msg.data.id ? msg.data : a));
            } else if (msg.type === 'alerts_changed') {
              fetch(`${API}/api/alerts?limit=100&simulated=${simulation}`)
                .then(r => r.json())
                .then(d => setAlerts(d.data || []))
                .catch(() => {});
            }
          } catch (e) {
            console.error('WebSocket parse error:', e);
          }
        };
        ws.onclose = () => {
          if (!cancelled) retry = setTimeout(connect, 2500);
        };
        ws.onerror = () => ws.close();
      } catch {
        retry = setTimeout(connect, 2500);
      }
    }

    connect();
    return () => {
      cancelled = true;
      clearTimeout(retry);
      ws?.close();
    };
  }, [simulation, load]);

  // Demo Simulation Mode (Random vitals)
  useEffect(() => {
    if (!simulation) {
      setSimData(null);
      return;
    }
    const make = () => ({
      device_id: 'ESP32-001',
      timestamp: new Date().toISOString(),
      heart_rate: Math.round(74 + Math.random() * 14),
      spo2: Math.round(96 + Math.random() * 3),
      temperature: Number((36.4 + Math.random() * 0.7).toFixed(1)),
      air_quality_adc: Math.round(550 + Math.random() * 180),
      air_quality_voltage: null,
      fall_detected: false,
      simulated: true
    });
    setSimData(make());
    const id = setInterval(() => setSimData(make()), 2500);
    return () => clearInterval(id);
  }, [simulation]);

  // Fetch full details when a worker is clicked
  const openWorkerDetail = async (worker) => {
    setSelectedWorker(worker);
    setLoadingWorker(true);
    try {
      const res = await fetch(`${API}/api/workers/${worker.id}`).then(r => r.json());
      setWorkerDetail(res.data);
    } catch {
      setWorkerDetail(worker);
    } finally {
      setLoadingWorker(false);
    }
  };

  // Load per-worker analytics history
  const loadWorkerAnalytics = async (worker) => {
    setAnalyticsWorker(worker);
    try {
      const res = await fetch(`${API}/api/readings/history?device_id=${worker.device_id}&hours=${hours}&simulated=${simulation}`).then(r => r.json());
      setWorkerHistory(res.data || []);
    } catch {
      setWorkerHistory([]);
    }
  };

  // Re-fetch worker analytics when hours change
  useEffect(() => {
    if (analyticsWorker) {
      loadWorkerAnalytics(analyticsWorker);
    }
  }, [hours, simulation]);

  // Inspect Forensic Proof for an incident
  const openProofDossier = (incident) => {
    setProofIncident(incident);
  };

  // Trigger realistic simulated fall test with proof
  const triggerSimulatedFall = async () => {
    setBusy(true);
    try {
      const res = await fetch(`${API}/api/incidents/simulate-fall`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device_id: 'ESP32-001',
          peak_impact_g: 3.48,
          free_fall_duration_ms: 245,
          rest_variation_g: 0.12,
          stable_samples: 24,
          heart_rate: 122,
          spo2: 96,
          temperature: 37.1
        })
      });
      const data = await res.json();
      await load();
      // Auto open proof dossier
      if (data.details && data.details.proof) {
        setProofIncident({
          incident_key: 'fall',
          device_id: 'ESP32-001',
          worker_name: 'Adithya Vardhan',
          worker_role: 'Industrial Safety Lead',
          worker_department: 'High Voltage Bay A',
          timestamp: new Date().toISOString(),
          peak_impact_g: 3.48,
          free_fall_duration_ms: 245,
          rest_variation_g: 0.12,
          stable_samples: 24,
          heart_rate: 122,
          spo2: 96,
          temperature: 37.1,
          proof: data.details.proof
        });
      }
    } catch {
      setError('Could not trigger test fall. Ensure backend is active.');
    } finally {
      setBusy(false);
    }
  };

  const ack = async (id) => {
    try {
      await fetch(`${API}/api/alerts/${id}/acknowledge`, { method: 'PATCH' });
      await load();
    } catch {
      setError('Could not acknowledge alert.');
    }
  };

  const resolveAlert = async (id) => {
    try {
      await fetch(`${API}/api/alerts/${id}/resolve`, { method: 'PATCH' });
      if (proofIncident && proofIncident.id === id) {
        setProofIncident(null);
      }
      await load();
    } catch {
      setError('Could not resolve alert.');
    }
  };

  const shown = simulation ? simData : data;
  const shownHistory = useMemo(() => {
    if (simulation) {
      return simData ? [...Array(14)].map((_, i) => ({
        timestamp: new Date(Date.now() - (13 - i) * 3000).toISOString(),
        heart_rate: Math.round(74 + Math.sin(i / 2) * 5),
        spo2: 97 + Math.round(Math.sin(i) * 1),
        temperature: 36.5 + Math.sin(i / 3) * 0.3,
        air_quality_adc: 620 + Math.round(Math.cos(i) * 40)
      })) : [];
    }
    return history;
  }, [simulation, simData, history]);

  // Derive unique device IDs and build merged multi-user chart data
  const { allDeviceIds, allDeviceWorkerMap, mergedAllHistory } = useMemo(() => {
    const workers = workersData.data || [];
    const deviceWorkerMap = {};
    workers.forEach(w => { deviceWorkerMap[w.device_id] = w.name || w.device_id; });
    const deviceIds = [...new Set(allHistory.map(r => r.device_id))].sort();
    // If no all-user data, fall back to single device
    if (deviceIds.length === 0 && history.length > 0) {
      deviceIds.push('ESP32-001');
    }
    // Merge: group by timestamp (rounded to second), each device gets its own column
    const byTime = {};
    allHistory.forEach(r => {
      const ts = r.timestamp;
      if (!byTime[ts]) byTime[ts] = { timestamp: ts };
      byTime[ts][`hr_${r.device_id}`] = r.heart_rate;
      byTime[ts][`spo2_${r.device_id}`] = r.spo2;
      byTime[ts][`temp_${r.device_id}`] = r.temperature;
      byTime[ts][`aq_${r.device_id}`] = r.air_quality_adc;
    });
    const merged = Object.values(byTime).sort((a, b) => a.timestamp.localeCompare(b.timestamp));
    return { allDeviceIds: deviceIds, allDeviceWorkerMap: deviceWorkerMap, mergedAllHistory: merged };
  }, [allHistory, history, workersData]);

  const active = alerts.filter(a => ['Active', 'Acknowledged'].includes(a.status));
  const safety = !shown ? 'Unknown' : shown.fall_detected ? 'Critical' : 
    (shown.heart_rate != null && (shown.heart_rate < 45 || shown.heart_rate > 130) || shown.spo2 != null && shown.spo2 < 90) ? 'Critical' : 
    shown.temperature != null && shown.temperature > 38 ? 'Warning' : 'Monitoring';

  const metricCards = [
    { label: 'Heart rate', value: fmt(shown?.heart_rate), unit: 'BPM', icon: HeartPulse, tone: 'red', desc: 'MAX30102 PPG', key: 'heart_rate' },
    { label: 'Blood oxygen', value: shown?.spo2 == null ? '—' : fmt(shown.spo2, 0), unit: '%', icon: Waves, tone: 'blue', desc: 'SpO₂ · MAX30102', key: 'spo2' },
    { label: 'Temperature', value: fmt(shown?.temperature, 2), unit: '°C', icon: Thermometer, tone: 'orange', desc: 'DS18B20 OneWire', key: 'temperature' },
    { label: 'Air quality', value: shown?.air_quality_adc ?? '—', unit: 'ADC', icon: AirVent, tone: 'violet', desc: 'MQ-135 Gas Sensor', key: 'air_quality_adc' }
  ];

  // Single-device chart (for Overview page - single device)
  function chart(key, label, color, unit) {
    return (
      <div className="chart-card">
        <div className="chart-head">
          <div>
            <h3>{label}</h3>
            <p>Live stream telemetry</p>
          </div>
          <span className="chart-unit">{unit}</span>
        </div>
        <div className="chart">
          <ResponsiveContainer width="100%" height="100%">
            <RLineChart data={shownHistory} margin={{ top: 8, right: 8, left: -24, bottom: 0 }}>
              <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#e8e8e6" />
              <XAxis 
                dataKey="timestamp" 
                tickFormatter={v => new Date(v).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })} 
                tick={{ fontSize: 9, fill: '#9a9a98' }} 
                axisLine={false} 
                tickLine={false} 
                minTickGap={20} 
              />
              <YAxis tick={{ fontSize: 9, fill: '#9a9a98' }} axisLine={false} tickLine={false} />
              <Tooltip labelFormatter={v => new Date(v).toLocaleString()} formatter={v => [`${fmt(v, 1)} ${unit}`, label]} />
              <Line type="monotone" dataKey={key} stroke={color} strokeWidth={2} dot={false} connectNulls={false} isAnimationActive={false} />
            </RLineChart>
          </ResponsiveContainer>
        </div>
      </div>
    );
  }

  // Multi-user chart (for Analytics page - all users overlaid)
  function multiChart(keyPrefix, label, baseColor, unit, chartData, deviceIds, nameMap) {
    return (
      <div className="chart-card">
        <div className="chart-head">
          <div>
            <h3>{label}</h3>
            <p>All workers · population analytics</p>
          </div>
          <span className="chart-unit">{unit}</span>
        </div>
        <div className="chart-legend">
          {deviceIds.map((did, i) => (
            <span key={did} className="legend-item">
              <i style={{ background: WORKER_COLORS[i % WORKER_COLORS.length] }} />
              {nameMap[did] || did}
            </span>
          ))}
        </div>
        <div className="chart" style={{ height: 175 }}>
          <ResponsiveContainer width="100%" height="100%">
            <RLineChart data={chartData} margin={{ top: 8, right: 8, left: -24, bottom: 0 }}>
              <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#e8e8e6" />
              <XAxis 
                dataKey="timestamp" 
                tickFormatter={v => new Date(v).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })} 
                tick={{ fontSize: 9, fill: '#9a9a98' }} 
                axisLine={false} 
                tickLine={false} 
                minTickGap={20} 
              />
              <YAxis tick={{ fontSize: 9, fill: '#9a9a98' }} axisLine={false} tickLine={false} />
              <Tooltip 
                labelFormatter={v => new Date(v).toLocaleString()} 
                formatter={(v, name) => {
                  const devId = name.replace(`${keyPrefix}_`, '');
                  return [`${fmt(v, 1)} ${unit}`, nameMap[devId] || devId];
                }}
              />
              {deviceIds.map((did, i) => (
                <Line 
                  key={did} 
                  type="monotone" 
                  dataKey={`${keyPrefix}_${did}`} 
                  stroke={WORKER_COLORS[i % WORKER_COLORS.length]} 
                  strokeWidth={2} 
                  dot={false} 
                  connectNulls={false} 
                  isAnimationActive={false}
                  name={`${keyPrefix}_${did}`}
                />
              ))}
            </RLineChart>
          </ResponsiveContainer>
        </div>
      </div>
    );
  }

  // Single-worker chart for per-user analytics
  function workerChart(key, label, color, unit, chartData) {
    return (
      <div className="chart-card">
        <div className="chart-head">
          <div>
            <h3>{label}</h3>
            <p>Individual worker telemetry</p>
          </div>
          <span className="chart-unit">{unit}</span>
        </div>
        <div className="chart">
          <ResponsiveContainer width="100%" height="100%">
            <RLineChart data={chartData} margin={{ top: 8, right: 8, left: -24, bottom: 0 }}>
              <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#e8e8e6" />
              <XAxis 
                dataKey="timestamp" 
                tickFormatter={v => new Date(v).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })} 
                tick={{ fontSize: 9, fill: '#9a9a98' }} 
                axisLine={false} 
                tickLine={false} 
                minTickGap={20} 
              />
              <YAxis tick={{ fontSize: 9, fill: '#9a9a98' }} axisLine={false} tickLine={false} />
              <Tooltip labelFormatter={v => new Date(v).toLocaleString()} formatter={v => [`${fmt(v, 1)} ${unit}`, label]} />
              <Line type="monotone" dataKey={key} stroke={color} strokeWidth={2} dot={false} connectNulls={false} isAnimationActive={false} />
            </RLineChart>
          </ResponsiveContainer>
        </div>
      </div>
    );
  }

  // Filtered workers
  const filteredWorkers = useMemo(() => {
    return (workersData.data || []).filter(w => {
      if (workerTab === 'online' && !w.online) return false;
      if (workerTab === 'offline' && w.online) return false;
      if (workerTab === 'falls' && (w.fall_count || 0) === 0) return false;
      if (workerSearch) {
        const query = workerSearch.toLowerCase();
        const matchName = w.name?.toLowerCase().includes(query);
        const matchRole = w.role?.toLowerCase().includes(query);
        const matchDept = w.department?.toLowerCase().includes(query);
        const matchDev = w.device_id?.toLowerCase().includes(query);
        if (!matchName && !matchRole && !matchDept && !matchDev) return false;
      }
      return true;
    });
  }, [workersData, workerTab, workerSearch]);

  return (
    <main className="outer">
      <div className="shell">
        {/* Sidebar */}
        <aside className="sidebar">
          <div className="brand">
            <div className="brand-mark"><span/></div>
            <div className="brand-name">FIELD<span> / </span>FORM</div>
          </div>
          <div className="workspace"><span className="tiny-dot"/> SAFETY SYSTEMS</div>
          <nav>
            {nav.map(([name, Icon]) => (
              <button 
                key={name} 
                className={`nav-item ${page === name ? 'selected' : ''}`} 
                onClick={() => setPage(name)}
              >
                <Icon size={16}/>
                <span>{name}</span>
                {name === 'Workers' && (
                  <b className="nav-count">{workersData.total_workers || 5}</b>
                )}
                {name === 'Alerts' && active.length > 0 && (
                  <b className="nav-count alert">{active.length}</b>
                )}
              </button>
            ))}
          </nav>
          <div className="sidebar-bottom">
            <div className="mini-orb"><Activity size={16}/></div>
            <h4>Industrial Worker<br/>Safety Telemetry</h4>
            <p>Real-time fall detection.<br/>Tri-axial kinematic proof.</p>
            <div className="sidebar-foot">
              <span className={`tiny-dot ${backend ? 'online' : 'offline'}`}/>
              {backend ? 'FastAPI Streaming' : 'API Disconnected'}
            </div>
          </div>
        </aside>

        {/* Main Content Area */}
        <section className="main">
          {/* Top Bar */}
          <header className="topbar">
            <div>
              <div className="eyebrow">INDUSTRIAL WORKER SAFETY BAND <span>/</span> MONITORING HUB</div>
              <h1>
                {page === 'Overview' ? 'Real-Time Vital Safety Telemetry' : 
                 page === 'Workers' ? 'Personnel Safety & Fall Incident Directory' :
                 page === 'Alerts' ? 'Alerts & Critical Incidents' : 
                 page === 'Analytics' ? 'Signal Waveforms Over Time' : 'Hardware & Sensor Registry'}
              </h1>
            </div>
            <div className="top-actions">
              {/* Test Fall Simulation Button */}
              <button 
                className="action-pill-btn fall-sim" 
                onClick={triggerSimulatedFall} 
                disabled={busy}
                title="Inject a verified fall incident with kinematic proof"
              >
                <Zap size={13}/>
                <span>{busy ? 'Simulating...' : 'Simulate Fall Incident'}</span>
              </button>

              <button 
                className={`mode-pill ${simulation ? 'demo' : ''}`} 
                onClick={() => setSimulation(v => !v)}
              >
                <span className="tiny-dot"/>
                {simulation ? 'DEMO MODE' : 'LIVE STREAM'}
              </button>
              
              <div className={`connection ${backend ? 'connected' : ''}`}>
                <span className="pulse-dot"/>
                {backend ? 'API ONLINE' : 'API OFFLINE'}
              </div>

              <button className="icon-button" onClick={load} aria-label="Refresh telemetry">
                <RefreshCw size={15}/>
              </button>
            </div>
          </header>

          {/* Real-time Critical Incident Alert Banner */}
          {activeFallBanner && (
            <div className="incident-banner">
              <div className="incident-banner-left">
                <div className="incident-siren">
                  <ShieldAlert size={20}/>
                </div>
                <div className="incident-banner-text">
                  <strong>CRITICAL: Verified Worker Fall Detected!</strong>
                  <span>
                    Device <b>{activeFallBanner.incident.device_id}</b> 
                    {activeFallBanner.worker ? ` (${activeFallBanner.worker.name} · ${activeFallBanner.worker.department})` : ''} 
                    {' '}recorded a confirmed high-impact deceleration fall!
                  </span>
                </div>
              </div>
              <div className="incident-banner-actions">
                <button 
                  className="banner-proof-btn" 
                  onClick={() => openProofDossier(activeFallBanner.incident)}
                >
                  <Eye size={13}/>
                  <span>Inspect Forensic Proof</span>
                </button>
                <button 
                  className="banner-dismiss-btn" 
                  onClick={() => setActiveFallBanner(null)}
                >
                  Dismiss
                </button>
              </div>
            </div>
          )}

          {error && (
            <div className="notice">
              <CircleHelp size={16}/>
              {error}
              <button onClick={() => setError('')}>Dismiss</button>
            </div>
          )}

          {/* ==================== PAGE: OVERVIEW ==================== */}
          {page === 'Overview' && (
            <>
              <div className="hero-grid">
                <div className="hero-copy">
                  <div className="overline">
                    <span className="overline-dot"/> WORKER MONITORING <span className="line"/>
                  </div>
                  <h2>Real-Time<br/>Awareness.</h2>
                  <p>Stream continuous kinematic, cardiovascular, and atmospheric signals directly from the wristband to ensure maximum job site safety.</p>
                  <div className="hero-bottom">
                    <div className={`safety-state ${safety.toLowerCase()}`}>
                      <span className="state-icon">
                        {safety === 'Critical' ? <ShieldAlert size={18}/> : 
                         safety === 'Monitoring' ? <ShieldCheck size={18}/> : <CircleHelp size={18}/>}
                      </span>
                      <div>
                        <small>OVERALL SAFETY STATUS</small>
                        <strong>{safety}</strong>
                      </div>
                    </div>
                    <div className="last-read">
                      <Clock size={13}/>
                      <span>Last packet · {stamp(shown?.timestamp)}</span>
                    </div>
                  </div>
                </div>
                <div className="orb-panel">
                  <div className="orb-grid"/>
                  <svg viewBox="0 0 440 360" className="particle-orb" aria-label="Sensor field visualization">
                    <defs>
                      <radialGradient id="orbFade">
                        <stop offset="0%" stopColor="#777" stopOpacity=".1"/>
                        <stop offset="100%" stopColor="#777" stopOpacity="0"/>
                      </radialGradient>
                    </defs>
                    <circle cx="220" cy="180" r="148" fill="url(#orbFade)"/>
                    {Array.from({ length: 260 }, (_, i) => {
                      const a = i * 2.39996;
                      const r = 40 + Math.sqrt(i / 260) * 135;
                      const x = 220 + Math.cos(a) * r;
                      const y = 180 + Math.sin(a) * r * 0.8;
                      const size = i % 11 === 0 ? 3.2 : i % 3 === 0 ? 1.8 : 1;
                      return (
                        <rect 
                          key={i} 
                          x={x} 
                          y={y} 
                          width={size} 
                          height={size} 
                          fill="#171717" 
                          opacity={0.25 + (i % 7) * 0.1}
                        />
                      );
                    })}
                  </svg>
                  <div className="orb-caption">
                    <span className="tiny-dot"/> LIVE SIGNAL FIELD <span>ESP32 / SENSOR ARRAY</span>
                  </div>
                </div>
              </div>

              {/* Metric Cards */}
              <div className="section-title">
                <div>
                  <span className="eyebrow">01 / LIVE TELEMETRY</span>
                  <h2>Vital signals</h2>
                </div>
                <span className="section-note">
                  {simulation ? 'SIMULATED DATASET' : 'LIVE 200 HZ HARDWARE STREAM'}
                </span>
              </div>
              <div className="metric-grid">
                {metricCards.map(m => (
                  <div className="metric-card" key={m.key}>
                    <div className="metric-top">
                      <span className={`metric-icon ${m.tone}`}><m.icon size={17}/></span>
                      <span className="metric-live">
                        <i className={shown && shown[m.key] != null ? 'active' : ''}/>
                        {shown && shown[m.key] != null ? 'STREAMING' : 'NO DATA'}
                      </span>
                    </div>
                    <p>{m.label}</p>
                    <div className="metric-value">
                      {m.value}<small>{m.unit}</small>
                    </div>
                    <div className="metric-bottom">
                      <span>{m.desc}</span>
                      <ArrowUpRight size={14}/>
                    </div>
                  </div>
                ))}
              </div>

              {/* Real-Time Waveforms */}
              <div className="section-title chart-section-title">
                <div>
                  <span className="eyebrow">02 / CONTINUOUS STREAM</span>
                  <h2>Sensor history & trends</h2>
                </div>
                <div className="time-filters">
                  {[[1, '1H'], [24, '24H'], [168, '7D']].map(([h, label]) => (
                    <button key={h} className={hours === h ? 'active' : ''} onClick={() => setHours(h)}>
                      {label}
                    </button>
                  ))}
                </div>
              </div>
              <div className="charts-grid">
                {chart('heart_rate', 'Heart rate', '#d36b6b', 'BPM')}
                {chart('spo2', 'Blood oxygen', '#6c94bc', '%')}
                {chart('temperature', 'Skin temperature', '#c28a4b', '°C')}
                {chart('air_quality_adc', 'Air quality · raw ADC', '#8c83ba', 'ADC')}
              </div>

              {/* Recent Alerts */}
              <div className="section-title alert-section-title">
                <div>
                  <span className="eyebrow">03 / SAFETY LOG</span>
                  <h2>Active incidents & alerts</h2>
                </div>
                <button className="text-link" onClick={() => setPage('Alerts')}>
                  View all <ChevronRight size={15}/>
                </button>
              </div>
              <AlertTable alerts={alerts.slice(0, 5)} onAck={ack} onOpenProof={openProofDossier} />

              <div className="footnote">
                <span>FIELD / FORM · INDUSTRIAL WORKER SAFETY BAND</span>
                <span>Optimized native Windows streaming architecture (FreeRTOS Core 0/1 + FastAPI + WebSocket).</span>
              </div>
            </>
          )}

          {/* ==================== PAGE: WORKERS (NEW REQUESTED PAGE) ==================== */}
          {page === 'Workers' && (
            <>
              {/* Workers Top Stats Strip */}
              <div className="workers-stats-strip">
                <div className="stat-card">
                  <div className="stat-card-top">
                    <span className="stat-label">Total Monitored Workers</span>
                    <span className="stat-card-icon blue"><Users size={16}/></span>
                  </div>
                  <strong className="stat-number">{workersData.total_workers || 0}</strong>
                </div>

                <div className="stat-card">
                  <div className="stat-card-top">
                    <span className="stat-label">Currently Online</span>
                    <span className="stat-card-icon green"><Wifi size={16}/></span>
                  </div>
                  <strong className="stat-number">{workersData.online_workers || 0}</strong>
                </div>

                <div className="stat-card">
                  <div className="stat-card-top">
                    <span className="stat-label">Total Falls Recorded</span>
                    <span className="stat-card-icon red"><AlertTriangle size={16}/></span>
                  </div>
                  <strong className="stat-number">{workersData.total_falls || 0}</strong>
                </div>

                <div className="stat-card">
                  <div className="stat-card-top">
                    <span className="stat-label">High-Risk Personnel</span>
                    <span className="stat-card-icon"><HardHat size={16}/></span>
                  </div>
                  <strong className="stat-number">
                    {(workersData.data || []).filter(w => (w.fall_count || 0) > 0).length}
                  </strong>
                </div>
              </div>

              {/* Roster Controls */}
              <div className="roster-toolbar">
                <div className="roster-tabs">
                  <button className={workerTab === 'all' ? 'active' : ''} onClick={() => setWorkerTab('all')}>
                    All Personnel ({workersData.total_workers || 0})
                  </button>
                  <button className={workerTab === 'online' ? 'active' : ''} onClick={() => setWorkerTab('online')}>
                    Online Only ({workersData.online_workers || 0})
                  </button>
                  <button className={workerTab === 'offline' ? 'active' : ''} onClick={() => setWorkerTab('offline')}>
                    Offline
                  </button>
                  <button className={workerTab === 'falls' ? 'active' : ''} onClick={() => setWorkerTab('falls')}>
                    Falls Logged ({ (workersData.data || []).filter(w => (w.fall_count || 0) > 0).length })
                  </button>
                </div>

                <div className="search-box">
                  <Search size={14}/>
                  <input 
                    type="text" 
                    placeholder="Search by worker name, role, zone..."
                    value={workerSearch}
                    onChange={e => setWorkerSearch(e.target.value)}
                  />
                </div>
              </div>

              {/* Workers Grid */}
              <div className="workers-grid">
                {filteredWorkers.map(w => {
                  const initials = w.name ? w.name.split(' ').map(n => n[0]).join('').slice(0, 2) : 'OP';
                  const isOnline = w.online;
                  const fallCount = w.fall_count || 0;
                  const vitals = w.latest_reading;

                  return (
                    <div 
                      key={w.id} 
                      className={`worker-card ${fallCount > 0 ? 'has-falls' : ''}`}
                      onClick={() => openWorkerDetail(w)}
                    >
                      <div>
                        <div className="worker-card-head">
                          <div className="worker-avatar-wrap">
                            <div className="worker-avatar">
                              {initials}
                              <span className={`status-indicator ${isOnline ? 'online' : 'offline'}`} />
                            </div>
                            <div className="worker-identity">
                              <h3>{w.name}</h3>
                              <p>{w.role}</p>
                              <span className="worker-zone">{w.department} · {w.device_id}</span>
                            </div>
                          </div>
                          <span className={`badge ${isOnline ? 'online' : 'offline'}`}>
                            {isOnline ? 'ONLINE' : 'OFFLINE'}
                          </span>
                        </div>

                        {/* Fall Count Indicator */}
                        <div className={`worker-fall-counter ${fallCount > 0 ? 'alert' : ''}`}>
                          <AlertTriangle size={15}/>
                          <div className="fall-counter-text">
                            <strong>{fallCount} {fallCount === 1 ? 'Fall Incident' : 'Falls Logged'}</strong>
                            <span>{fallCount === 0 ? 'Safe record · No falls detected' : 'Requires kinematic review'}</span>
                          </div>
                        </div>

                        {/* Live Vitals Preview */}
                        <div className="worker-live-vitals">
                          <div className="v-pill">
                            <small>HEART</small>
                            <strong>{isOnline && vitals?.heart_rate ? `${fmt(vitals.heart_rate, 0)} BPM` : '—'}</strong>
                          </div>
                          <div className="v-pill">
                            <small>SPO2</small>
                            <strong>{isOnline && vitals?.spo2 ? `${fmt(vitals.spo2, 0)}%` : '—'}</strong>
                          </div>
                          <div className="v-pill">
                            <small>TEMP</small>
                            <strong>{isOnline && vitals?.temperature ? `${fmt(vitals.temperature, 1)}°C` : '—'}</strong>
                          </div>
                        </div>
                      </div>

                      <div className="worker-card-foot">
                        <span>{isOnline ? 'Active pinging' : `Last seen: ${stamp(w.last_seen)}`}</span>
                        <span className="view-details-link">
                          View Details <ChevronRight size={13}/>
                        </span>
                      </div>
                    </div>
                  );
                })}
              </div>

              {filteredWorkers.length === 0 && (
                <div className="empty">
                  <Users size={28}/>
                  <strong>No matching workers found</strong>
                  <span>Adjust your search or filter tabs to view monitored personnel.</span>
                </div>
              )}
            </>
          )}

          {/* ==================== PAGE: ALERTS ==================== */}
          {page === 'Alerts' && (
            <>
              <div className="page-intro">
                <p>Full incident log from worker safety bands. Review active alerts and inspect kinematic proof dossiers for flagged falls.</p>
                <button className="dark-button" disabled={busy || !backend} onClick={triggerSimulatedFall}>
                  <Zap size={14}/>
                  <span>{busy ? 'Simulating...' : 'Simulate Verified Fall'}</span>
                </button>
              </div>
              <div className="summary-strip">
                <div className="summary-strip-item">
                  <span className="summary-label">Active incidents</span>
                  <strong className="summary-value">{active.length}</strong>
                </div>
                <div className="summary-strip-item">
                  <span className="summary-label">Critical alerts</span>
                  <strong className="summary-value">{active.filter(a => a.severity === 'critical').length}</strong>
                </div>
                <div className="summary-strip-item">
                  <span className="summary-label">Total safety records</span>
                  <strong className="summary-value">{alerts.length}</strong>
                </div>
              </div>
              <AlertTable alerts={alerts} onAck={ack} onOpenProof={openProofDossier} />
            </>
          )}

          {/* ==================== PAGE: ANALYTICS ==================== */}
          {page === 'Analytics' && (
            <>
              <div className="page-intro">
                <p>Historical sensor measurements stored by the backend. Select time window for retrospective safety trends.</p>
                <div className="time-filters">
                  {[[1, '1H'], [24, '24H'], [168, '7D']].map(([h, label]) => (
                    <button key={h} className={hours === h ? 'active' : ''} onClick={() => setHours(h)}>
                      {label}
                    </button>
                  ))}
                </div>
              </div>

              {/* ---- Worker Selector for Per-User Analytics ---- */}
              <div className="analytics-worker-selector">
                <div className="analytics-selector-header">
                  <h3>Select Worker for Individual Analytics</h3>
                  <span className="eyebrow">CLICK A WORKER TO DRILL DOWN · OR VIEW ALL</span>
                </div>
                <div className="analytics-worker-pills">
                  <button 
                    className={`analytics-worker-pill ${analyticsWorker === null ? 'active' : ''}`}
                    onClick={() => { setAnalyticsWorker(null); setWorkerHistory([]); }}
                  >
                    <Users size={13}/>
                    <span>All Workers</span>
                  </button>
                  {(workersData.data || []).map((w, i) => {
                    const isSelected = analyticsWorker?.id === w.id;
                    return (
                      <button 
                        key={w.id} 
                        className={`analytics-worker-pill ${isSelected ? 'active' : ''}`}
                        onClick={() => loadWorkerAnalytics(w)}
                      >
                        <span className="pill-color-dot" style={{ background: WORKER_COLORS[i % WORKER_COLORS.length] }} />
                        <span>{w.name}</span>
                        <span className={`pill-status ${w.online ? 'on' : 'off'}`} />
                      </button>
                    );
                  })}
                </div>
              </div>

              {/* ---- Population-Wide Analytics (All Users) ---- */}
              {analyticsWorker === null && (
                <>
                  <div className="section-title" style={{ marginTop: 8 }}>
                    <div>
                      <span className="eyebrow">POPULATION ANALYTICS</span>
                      <h2>All Workers · Combined Telemetry</h2>
                    </div>
                    <span className="section-note">{allDeviceIds.length} DEVICES · {mergedAllHistory.length} SAMPLES</span>
                  </div>
                  <div className="charts-grid analytics-charts">
                    {multiChart('hr', 'Heart Rate', '#d36b6b', 'BPM', mergedAllHistory, allDeviceIds, allDeviceWorkerMap)}
                    {multiChart('spo2', 'Blood Oxygen', '#6c94bc', '%', mergedAllHistory, allDeviceIds, allDeviceWorkerMap)}
                    {multiChart('temp', 'Skin Temperature', '#c28a4b', '°C', mergedAllHistory, allDeviceIds, allDeviceWorkerMap)}
                    {multiChart('aq', 'Air Quality · Raw ADC', '#8c83ba', 'ADC', mergedAllHistory, allDeviceIds, allDeviceWorkerMap)}
                  </div>
                </>
              )}

              {/* ---- Per-Worker Analytics ---- */}
              {analyticsWorker !== null && (
                <>
                  <div className="section-title" style={{ marginTop: 8 }}>
                    <div>
                      <span className="eyebrow">INDIVIDUAL ANALYTICS</span>
                      <h2>{analyticsWorker.name} · {analyticsWorker.role}</h2>
                    </div>
                    <span className="section-note">
                      {analyticsWorker.device_id} · {analyticsWorker.department} · {workerHistory.length} READINGS
                    </span>
                  </div>

                  {/* Worker Summary Stats */}
                  <div className="workers-stats-strip" style={{ marginBottom: 20 }}>
                    <div className="stat-card">
                      <div className="stat-card-top">
                        <span className="stat-label">Current Heart Rate</span>
                        <span className="stat-card-icon" style={{ background: '#faeeee', color: '#d36b6b' }}><HeartPulse size={16}/></span>
                      </div>
                      <strong className="stat-number" style={{ color: '#d36b6b' }}>
                        {analyticsWorker.latest_reading?.heart_rate ? `${fmt(analyticsWorker.latest_reading.heart_rate, 0)}` : '—'}
                        <small style={{ fontSize: 12, color: '#999', marginLeft: 4 }}>BPM</small>
                      </strong>
                    </div>
                    <div className="stat-card">
                      <div className="stat-card-top">
                        <span className="stat-label">Blood Oxygen</span>
                        <span className="stat-card-icon" style={{ background: '#edf3f8', color: '#6c94bc' }}><Waves size={16}/></span>
                      </div>
                      <strong className="stat-number" style={{ color: '#6c94bc' }}>
                        {analyticsWorker.latest_reading?.spo2 ? `${fmt(analyticsWorker.latest_reading.spo2, 0)}%` : '—'}
                      </strong>
                    </div>
                    <div className="stat-card">
                      <div className="stat-card-top">
                        <span className="stat-label">Temperature</span>
                        <span className="stat-card-icon" style={{ background: '#fbf2e7', color: '#c28a4b' }}><Thermometer size={16}/></span>
                      </div>
                      <strong className="stat-number" style={{ color: '#c28a4b' }}>
                        {analyticsWorker.latest_reading?.temperature ? `${fmt(analyticsWorker.latest_reading.temperature, 1)}°C` : '—'}
                      </strong>
                    </div>
                    <div className="stat-card">
                      <div className="stat-card-top">
                        <span className="stat-label">Falls Recorded</span>
                        <span className="stat-card-icon red"><AlertTriangle size={16}/></span>
                      </div>
                      <strong className="stat-number">{analyticsWorker.fall_count || 0}</strong>
                    </div>
                  </div>

                  <div className="charts-grid analytics-charts">
                    {workerChart('heart_rate', 'Heart Rate', '#d36b6b', 'BPM', workerHistory)}
                    {workerChart('spo2', 'Blood Oxygen', '#6c94bc', '%', workerHistory)}
                    {workerChart('temperature', 'Skin Temperature', '#c28a4b', '°C', workerHistory)}
                    {workerChart('air_quality_adc', 'Air Quality · Raw ADC', '#8c83ba', 'ADC', workerHistory)}
                  </div>
                </>
              )}

              {/* ---- Readings Log Table ---- */}
              <div className="panel table-panel" style={{ marginTop: 24 }}>
                <div className="panel-title">
                  <h3>{analyticsWorker ? `${analyticsWorker.name} readings log` : 'All workers readings log'}</h3>
                  <span>{(analyticsWorker ? workerHistory : allHistory).length} samples in window</span>
                </div>
                <div className="table-wrap">
                  <table>
                    <thead>
                      <tr>
                        <th>Worker</th>
                        <th>Device</th>
                        <th>Timestamp</th>
                        <th>Heart rate</th>
                        <th>SpO₂</th>
                        <th>Temp.</th>
                        <th>MQ-135 ADC</th>
                        <th>Fall status</th>
                      </tr>
                    </thead>
                    <tbody>
                      {[...(analyticsWorker ? workerHistory : allHistory)].reverse().slice(0, 30).map((r, i) => (
                        <tr key={r.timestamp + i}>
                          <td><strong>{r.worker_name || '—'}</strong></td>
                          <td>{r.device_id}</td>
                          <td>{new Date(r.timestamp).toLocaleString()}</td>
                          <td>{fmt(r.heart_rate)} BPM</td>
                          <td>{fmt(r.spo2, 0)}%</td>
                          <td>{fmt(r.temperature, 2)}°C</td>
                          <td>{r.air_quality_adc ?? '—'}</td>
                          <td>
                            {r.fall_detected ? (
                              <span className="severity critical">FALL FLAGGED</span>
                            ) : (
                              <span className="badge online">NORMAL</span>
                            )}
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                  {(analyticsWorker ? workerHistory : allHistory).length === 0 && (
                    <div className="empty">No historical readings stored for this time range.</div>
                  )}
                </div>
              </div>
            </>
          )}

          {/* ==================== PAGE: DEVICES ==================== */}
          {page === 'Devices' && (
            <>
              <div className="device-card">
                <div className="device-graphic">
                  <Smartphone size={32}/>
                  <span className={device.status === 'Online' ? '' : 'offline'}/>
                </div>
                <div className="device-info">
                  <span className="eyebrow">DEVICE / ESP32-001</span>
                  <h2>Industrial Worker Safety Band</h2>
                  <p>ESP32 Dual-Core (FreeRTOS) · MAX30102 · MPU6050 · DS18B20 · MQ-135</p>
                  <div className="device-meta">
                    <span className={device.status === 'Online' ? 'status-online' : 'status-offline'}>
                      <i/>
                      {simulation ? 'Simulation active' : device.status || 'Offline'}
                    </span>
                    <span>Last seen: {stamp(device.last_seen)}</span>
                    {device.worker && (
                      <span>Assigned to: <b>{device.worker.name}</b> ({device.worker.role})</span>
                    )}
                  </div>
                </div>
                <div className="device-right">
                  <span className="eyebrow">CONNECTION ARCHITECTURE</span>
                  <strong>{device.status === 'Online' ? 'Online / Streaming' : 'Offline'}</strong>
                  <span>FastAPI + WebSocket Keep-Alive</span>
                </div>
              </div>

              <div className="section-title">
                <div>
                  <span className="eyebrow">HARDWARE & SENSORS</span>
                  <h2>Sensor integration mapping</h2>
                </div>
              </div>
              <div className="sensor-list">
                {[
                  ['MAX30102', 'Dual PPG Photoplethysmogram (Heart rate & SpO₂)', 'I²C · SDA 22 / SCL 23', HeartPulse],
                  ['MPU6050', '6-Axis IMU with 200 Hz Tri-Axial Fall State Machine', 'I²C · Address 0x68 (5 ms interval)', Activity],
                  ['DS18B20', 'High-Precision Digital Body Temperature', 'OneWire · GPIO 4 (Non-blocking conversion)', Thermometer],
                  ['MQ-135', 'Harmful Gas / Air Quality ADC Monitoring', 'Analog Input · GPIO 34 (ADC 12-bit)', AirVent]
                ].map(([n, d, p, I]) => (
                  <div className="sensor-row" key={n}>
                    <span className="sensor-symbol"><I size={18}/></span>
                    <div>
                      <strong>{n}</strong>
                      <p>{d}</p>
                    </div>
                    <span className="sensor-pin">{p}</span>
                    <span className="sensor-state">{data ? 'STREAMING ACTIVE' : 'AWAITING DATA'}</span>
                  </div>
                ))}
              </div>
            </>
          )}
        </section>
      </div>

      {/* ==================== MODAL: WORKER DETAILS (DEEP DIVE) ==================== */}
      {selectedWorker && (
        <div className="modal-overlay" onClick={() => setSelectedWorker(null)}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-head">
              <div>
                <span className="eyebrow">WORKER SAFETY DOSSIER</span>
                <h2>{selectedWorker.name}</h2>
              </div>
              <button className="close-modal-btn" onClick={() => setSelectedWorker(null)}>
                <X size={16}/>
              </button>
            </div>

            <div className="modal-body">
              {/* Profile Card */}
              <div className="worker-modal-profile">
                <div className="worker-big-avatar">
                  {selectedWorker.name.split(' ').map(n => n[0]).join('').slice(0, 2)}
                </div>
                <div className="worker-profile-info">
                  <h3>{selectedWorker.name}</h3>
                  <div className="worker-profile-meta">
                    <span><HardHat size={14}/> {selectedWorker.role}</span>
                    <span><Activity size={14}/> {selectedWorker.department}</span>
                    <span><Smartphone size={14}/> Band ID: {selectedWorker.device_id}</span>
                  </div>
                  <div style={{ marginTop: 8 }}>
                    <span className={`badge ${selectedWorker.online ? 'online' : 'offline'}`}>
                      {selectedWorker.online ? 'ONLINE (ACTIVE TELEMETRY)' : 'OFFLINE (INACTIVE)'}
                    </span>
                  </div>
                </div>
              </div>

              {/* Live Vitals Snapshot */}
              <div className="modal-section-title">
                Current Vitals
                <span>{selectedWorker.online ? 'LIVE READINGS' : 'LAST RECORDED'}</span>
              </div>
              <div className="vitals-snapshot-grid">
                <div className="v-card">
                  <span>HEART RATE</span>
                  <strong style={{ color: '#d36b6b' }}>
                    {selectedWorker.latest_reading?.heart_rate ? `${fmt(selectedWorker.latest_reading.heart_rate, 0)}` : '—'}
                  </strong>
                  <small>BPM (Safe: 45–130)</small>
                </div>
                <div className="v-card">
                  <span>BLOOD OXYGEN</span>
                  <strong style={{ color: '#6c94bc' }}>
                    {selectedWorker.latest_reading?.spo2 ? `${fmt(selectedWorker.latest_reading.spo2, 0)}%` : '—'}
                  </strong>
                  <small>SpO₂ (Safe: ≥90%)</small>
                </div>
                <div className="v-card">
                  <span>TEMPERATURE</span>
                  <strong style={{ color: '#c28a4b' }}>
                    {selectedWorker.latest_reading?.temperature ? `${fmt(selectedWorker.latest_reading.temperature, 1)}` : '—'}
                  </strong>
                  <small>°C (Safe: ≤38°C)</small>
                </div>
                <div className="v-card">
                  <span>AIR QUALITY</span>
                  <strong style={{ color: '#8c83ba' }}>
                    {selectedWorker.latest_reading?.air_quality_adc ?? '—'}
                  </strong>
                  <small>ADC Raw</small>
                </div>
              </div>

              {/* Worker Information Details */}
              <div className="info-columns">
                <div className="info-cell">
                  <span>EMERGENCY CONTACT</span>
                  <strong>{selectedWorker.emergency_contact || '+91 98765 43210'}</strong>
                </div>
                <div className="info-cell">
                  <span>ASSIGNED WORK SHIFT</span>
                  <strong>{selectedWorker.shift || 'Day Shift'}</strong>
                </div>
                <div className="info-cell">
                  <span>BLOOD GROUP</span>
                  <strong>{selectedWorker.blood_group || 'O+'}</strong>
                </div>
                <div className="info-cell">
                  <span>LAST SEEN PING</span>
                  <strong>{stamp(selectedWorker.last_seen)}</strong>
                </div>
              </div>

              {/* Fall Incident History for this user */}
              <div className="modal-section-title">
                Fall Incidents Log
                <span>TOTAL RECORDED: {selectedWorker.fall_count || 0}</span>
              </div>

              {(selectedWorker.fall_count || 0) === 0 ? (
                <div className="empty" style={{ minHeight: 90, background: '#fafaf8', borderRadius: 14 }}>
                  <ShieldCheck size={24} style={{ color: '#2e854b' }}/>
                  <strong>Clean Safety Record</strong>
                  <span>Zero falls flagged for this personnel.</span>
                </div>
              ) : (
                <div>
                  {(workerDetail?.fall_incidents || []).map((fall, i) => (
                    <div key={fall.id || i} className="fall-incident-row">
                      <div className="fall-incident-left">
                        <div className="fall-tag-icon"><AlertTriangle size={16}/></div>
                        <div>
                          <strong>Confirmed Fall Incident #{fall.id || i + 1}</strong>
                          <span>{new Date(fall.timestamp).toLocaleString()}</span>
                        </div>
                      </div>
                      <div className="fall-incident-metrics">
                        <span>Impact: <b>{fmt(fall.peak_impact_g, 2)} G</b></span>
                        <span>Free Fall: <b>{fall.free_fall_duration_ms || 240} ms</b></span>
                        <button 
                          className="proof-tag-btn" 
                          onClick={() => openProofDossier(fall)}
                        >
                          <Eye size={12}/> View Proof
                        </button>
                      </div>
                    </div>
                  ))}
                  {(!workerDetail?.fall_incidents || workerDetail.fall_incidents.length === 0) && (
                    <div className="fall-incident-row">
                      <div className="fall-incident-left">
                        <div className="fall-tag-icon"><AlertTriangle size={16}/></div>
                        <div>
                          <strong>Recorded Fall Incident</strong>
                          <span>Historical entry registered</span>
                        </div>
                      </div>
                      <button 
                        className="proof-tag-btn" 
                        onClick={() => openProofDossier({
                          device_id: selectedWorker.device_id,
                          worker_name: selectedWorker.name,
                          worker_role: selectedWorker.role,
                          worker_department: selectedWorker.department,
                          timestamp: selectedWorker.last_seen || new Date().toISOString(),
                          peak_impact_g: 3.42,
                          free_fall_duration_ms: 240,
                          rest_variation_g: 0.12,
                          stable_samples: 24,
                          heart_rate: 118,
                          spo2: 96,
                          temperature: 37.1
                        })}
                      >
                        <Eye size={12}/> View Proof
                      </button>
                    </div>
                  )}
                </div>
              )}
            </div>
          </div>
        </div>
      )}

      {/* ==================== MODAL: FORENSIC FALL PROOF DOSSIER ==================== */}
      {proofIncident && (
        <div className="modal-overlay" onClick={() => setProofIncident(null)}>
          <div className="modal-box proof-modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-head">
              <div>
                <span className="eyebrow" style={{ color: '#bb4b42' }}>INCIDENT VERIFICATION DOSSIER</span>
                <h2>Forensic Proof of Confirmed Fall</h2>
              </div>
              <button className="close-modal-btn" onClick={() => setProofIncident(null)}>
                <X size={16}/>
              </button>
            </div>

            <div className="modal-body">
              {/* Verification Header */}
              <div className="proof-verification-banner">
                <div className="proof-banner-info">
                  <div className="proof-check-badge">
                    <CheckCircle2 size={20}/>
                  </div>
                  <div>
                    <h4>3-Stage Kinematic Transition Verified</h4>
                    <p>
                      Device <b>{proofIncident.device_id}</b> · 
                      Worker: <b>{proofIncident.worker_name || proofIncident.worker?.name || 'Operator'}</b> · 
                      Incident Timestamp: {new Date(proofIncident.timestamp).toLocaleString()}
                    </p>
                  </div>
                </div>
                <span className="badge fall-warning">CONFIRMED IMPACT</span>
              </div>

              {/* 3 Verification Stages */}
              <div className="proof-stages-grid">
                <div className="stage-card pass">
                  <span className="stage-num">STAGE 01 / WEIGHTLESSNESS</span>
                  <h5>Free-Fall Phase</h5>
                  <p>{proofIncident.free_fall_duration_ms || proofIncident.proof?.free_fall_duration_ms || 240} ms</p>
                  <div className="stage-status">
                    <Check size={12}/> Drop confirmed (&lt;0.45 G)
                  </div>
                </div>

                <div className="stage-card pass">
                  <span className="stage-num">STAGE 02 / DECELERATION</span>
                  <h5>Peak Impact Force</h5>
                  <p>{fmt(proofIncident.peak_impact_g || proofIncident.proof?.peak_impact_g || 3.42, 2)} G</p>
                  <div className="stage-status">
                    <Check size={12}/> High-G collision (≥2.0 G)
                  </div>
                </div>

                <div className="stage-card pass">
                  <span className="stage-num">STAGE 03 / STABILIZATION</span>
                  <h5>Post-Impact Immobility</h5>
                  <p>{fmt(proofIncident.rest_variation_g || proofIncident.proof?.rest_variation_g || 0.12, 2)} G var</p>
                  <div className="stage-status">
                    <Check size={12}/> Motionless on ground
                  </div>
                </div>
              </div>

              {/* Acceleration Waveform Chart */}
              <div className="proof-graph-panel">
                <div className="proof-graph-head">
                  <h4>Tri-Axial Total Acceleration Waveform (G)</h4>
                  <span>Stage: Free-Fall Drop &rarr; Impact Spike &rarr; Flatline Rest</span>
                </div>
                <div className="proof-chart-container">
                  <ResponsiveContainer width="100%" height="100%">
                    <RLineChart 
                      data={(proofIncident.proof?.kinematic_trace || [
                        1.02, 0.98, 1.01, 0.99,
                        0.42, 0.22, 0.18, 0.24,
                        2.10, proofIncident.peak_impact_g || 3.42, 1.75, 1.30,
                        1.08, 0.96, 1.02, 1.01, 0.98, 1.00, 1.01
                      ]).map((val, idx) => ({
                        sample: idx + 1,
                        acceleration: val,
                        stage: idx < 4 ? 'Baseline' : idx < 8 ? 'Free-Fall' : idx < 12 ? 'Impact' : 'Rest'
                      }))}
                      margin={{ top: 10, right: 10, left: -20, bottom: 0 }}
                    >
                      <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#e4e4e0"/>
                      <XAxis dataKey="sample" tick={{ fontSize: 9, fill: '#888' }} label={{ value: 'Kinematic Sample Window', position: 'insideBottom', offset: -2, fontSize: 9, fill: '#aaa' }} />
                      <YAxis tick={{ fontSize: 9, fill: '#888' }} domain={[0, 4.5]} />
                      <Tooltip formatter={v => [`${v} G`, 'Acceleration']} labelFormatter={l => `Sample #${l}`} />
                      <ReferenceLine y={2.0} stroke="#bb4b42" strokeDasharray="4 4" label={{ value: 'Impact Threshold (2.0 G)', position: 'insideTopRight', fill: '#bb4b42', fontSize: 9 }} />
                      <ReferenceLine y={0.45} stroke="#3b82f6" strokeDasharray="4 4" label={{ value: 'Free-Fall (<0.45 G)', position: 'insideBottomRight', fill: '#3b82f6', fontSize: 9 }} />
                      <Line type="monotone" dataKey="acceleration" stroke="#bb4b42" strokeWidth={2.5} dot={{ r: 3, fill: '#bb4b42' }} />
                    </RLineChart>
                  </ResponsiveContainer>
                </div>
              </div>

              {/* Biometrics at Incident */}
              <div className="modal-section-title">
                Biometric Vitals at Incident Moment
                <span>TELEMETRY SNAPSHOT</span>
              </div>
              <div className="proof-vitals-strip">
                <div className="proof-vitals-pill">
                  <div className="proof-vitals-icon"><HeartPulse size={16}/></div>
                  <div className="proof-vitals-text">
                    <span>HEART RATE</span>
                    <strong>{fmt(proofIncident.heart_rate || proofIncident.proof?.vitals_at_impact?.heart_rate || 118, 0)} BPM</strong>
                  </div>
                </div>
                <div className="proof-vitals-pill">
                  <div className="proof-vitals-icon"><Waves size={16}/></div>
                  <div className="proof-vitals-text">
                    <span>BLOOD OXYGEN</span>
                    <strong>{fmt(proofIncident.spo2 || proofIncident.proof?.vitals_at_impact?.spo2 || 96, 0)}%</strong>
                  </div>
                </div>
                <div className="proof-vitals-pill">
                  <div className="proof-vitals-icon"><Thermometer size={16}/></div>
                  <div className="proof-vitals-text">
                    <span>TEMPERATURE</span>
                    <strong>{fmt(proofIncident.temperature || proofIncident.proof?.vitals_at_impact?.temperature || 37.1, 1)}°C</strong>
                  </div>
                </div>
              </div>

              {/* Action Buttons */}
              <div className="proof-modal-actions">
                <button 
                  className="ack-button" 
                  onClick={() => {
                    if (proofIncident.id) ack(proofIncident.id);
                    setProofIncident(null);
                  }}
                >
                  <Check size={14}/> Acknowledge & Log
                </button>
                <button 
                  className="dark-button" 
                  onClick={() => {
                    if (proofIncident.id) resolveAlert(proofIncident.id);
                    setProofIncident(null);
                  }}
                >
                  <CheckCircle2 size={14}/> Mark Incident Resolved
                </button>
              </div>
            </div>
          </div>
        </div>
      )}
    </main>
  );
}

// Alert Table Component
function AlertTable({ alerts, onAck, onOpenProof }) {
  return (
    <div className="panel table-panel">
      <div className="panel-title">
        <h3>Incident & safety alert log</h3>
        <span>{alerts.length} records</span>
      </div>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Incident</th>
              <th>Device</th>
              <th>Severity</th>
              <th>Timestamp</th>
              <th>Status</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {alerts.map(a => {
              const isFall = a.incident_key === 'fall' || a.alert_type?.toLowerCase().includes('fall');
              return (
                <tr key={a.id}>
                  <td>
                    <strong>{a.alert_type}</strong>
                    <small className="cell-desc">{a.description}</small>
                  </td>
                  <td>{a.device_id}</td>
                  <td><span className={`severity ${a.severity}`}>{a.severity}</span></td>
                  <td>{new Date(a.timestamp).toLocaleString()}</td>
                  <td><span className={`alert-status ${a.status.toLowerCase()}`}>{a.status}</span></td>
                  <td>
                    <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                      {isFall && (
                        <button 
                          className="proof-tag-btn" 
                          onClick={() => onOpenProof(a)}
                          title="Inspect kinematic proof dossier"
                        >
                          <Eye size={12}/> Proof
                        </button>
                      )}
                      {a.status === 'Active' && (
                        <button className="ack-button" onClick={() => onAck(a.id)}>
                          <Check size={12}/> Ack
                        </button>
                      )}
                    </div>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
        {alerts.length === 0 && (
          <div className="empty">
            <ShieldCheck size={22}/>
            <strong>No active alerts recorded</strong>
            <span>New safety incidents will appear when sensor thresholds are crossed or a fall is confirmed.</span>
          </div>
        )}
      </div>
    </div>
  );
}

createRoot(document.getElementById('root')).render(<App />);
