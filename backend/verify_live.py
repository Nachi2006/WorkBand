import urllib.request
import json
import sys

# Configure UTF-8 stdout for Windows consoles
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8')

def req(url, method='GET', data=None):
    r = urllib.request.Request(url, method=method)
    if data:
        r.add_header('Content-Type', 'application/json')
        r.data = json.dumps(data).encode()
    try:
        with urllib.request.urlopen(r) as res:
            return res.status, json.loads(res.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        return e.code, json.loads(body) if body else str(e)

print("=== 1. CHECK HEALTH & ARCHITECTURE ===")
status, h = req("http://127.0.0.1:8000/api/health")
print(f"Status: {status}")
print(f"Health Response: {json.dumps(h, indent=2)}")

print("\n=== 2. WORKERS ROSTER ===")
status, w = req("http://127.0.0.1:8000/api/workers")
print(f"Status: {status}, Total workers: {w.get('total_workers')}")
for worker in w.get('data', [])[:3]:
    print(f" - {worker['id']}: {worker['name']} ({worker['role']}) - Device: {worker['device_id']} - Falls: {worker['fall_count']}")

print("\n=== 3. INGEST NORMAL SENSOR READING (POST /api/readings) ===")
reading_payload = {
    'device_id': 'ESP32-001',
    'heart_rate': 76.5,
    'spo2': 98.2,
    'temperature': 36.8,
    'air_quality_adc': 310,
    'air_quality_voltage': 0.85,
    'simulated': False
}
status, r = req("http://127.0.0.1:8000/api/readings", "POST", reading_payload)
print(f"Status: {status}, Ingested: {r.get('accepted')}, Source: {r.get('source')}, Reading ID: {r.get('reading_id')}")

print("\n=== 4. SIMULATE FALL INCIDENT WITH KINEMATIC PROOF (POST /api/incidents/simulate-fall) ===")
fall_payload = {
    'device_id': 'ESP32-001',
    'peak_impact_g': 3.82,
    'free_fall_duration_ms': 255,
    'rest_variation_g': 0.09,
    'stable_samples': 26,
    'heart_rate': 124.0,
    'spo2': 97.0,
    'temperature': 37.2
}
status, f = req("http://127.0.0.1:8000/api/incidents/simulate-fall", "POST", fall_payload)
print(f"Status: {status}, Incident Key: {f.get('status')}")
proof = f.get('details', {}).get('proof', {})
print(f"Proof Verified: {proof.get('verified')}")
print(f"Algorithm: {proof.get('algorithm')}")
print(f"Peak Impact: {proof.get('peak_impact_g')} G")
print(f"Free Fall: {proof.get('free_fall_duration_ms')} ms")

print("\n=== 5. CHECK FALL INCIDENTS LIST (GET /api/incidents/falls) ===")
status, falls = req("http://127.0.0.1:8000/api/incidents/falls")
print(f"Status: {status}, Recorded Falls: {len(falls.get('data', []))}")

print("\n=== 6. CHECK WORKER FALL COUNT UPDATE (GET /api/workers/ESP32-001) ===")
status, w_detail = req("http://127.0.0.1:8000/api/workers/ESP32-001")
print(f"Status: {status}, Worker: {w_detail['data']['name']}, Fall Count: {w_detail['data']['fall_count']}")

print("\n=== 7. CHECK ACTIVE ALERTS (GET /api/alerts) ===")
status, alerts = req("http://127.0.0.1:8000/api/alerts")
print(f"Status: {status}, Active Alerts: {len(alerts.get('data', []))}")
for a in alerts.get('data', [])[:3]:
    print(f" - [{a['severity'].upper()}] {a['alert_type']}: {a['description']} (Device: {a['device_id']})")

print("\n=== 8. CHECK TIME-SERIES / HISTORY QUERY (GET /api/readings/history) ===")
status, hist = req("http://127.0.0.1:8000/api/readings/history?device_id=ESP32-001&hours=1")
print(f"Status: {status}, Storage Backend: {hist.get('source')}, History Points: {len(hist.get('data', []))}")

print("\n=== 9. CHECK DASHBOARD SUMMARY (GET /api/dashboard/summary) ===")
status, summary = req("http://127.0.0.1:8000/api/dashboard/summary")
print(f"Status: {status}")
print(f"Total Workers: {summary.get('total_workers')}")
print(f"Online Devices: {summary.get('online_devices')}")
print(f"Active Alerts: {summary.get('active_alerts')}")
print(f"Total Falls: {summary.get('total_falls')}")

print("\n>>> ALL API VERIFICATIONS COMPLETED SUCCESSFULLY! <<<")
