import os
from fastapi.testclient import TestClient
from main import app, init_db

def test_api(tmp_path, monkeypatch):
    import main
    monkeypatch.setattr(main, 'DB_PATH', tmp_path / 'test.db')
    with TestClient(app) as client:
        # Health endpoint should report architecture info
        h = client.get('/api/health').json()
        assert h['status'] == 'ok'
        assert 'mqtt_enabled' in h
        assert 'influxdb_connected' in h

        # Empty payload should be rejected
        bad = client.post('/api/readings', json={'device_id': 'ESP32-TEST'})
        assert bad.status_code == 422

        # Valid reading should trigger alert (high HR)
        good = client.post('/api/readings', json={'device_id': 'ESP32-TEST', 'heart_rate': 140, 'spo2': 98, 'simulated': False})
        assert good.status_code == 201
        assert good.json()['accepted'] is True
        assert good.json()['source'] == 'http'

        # Alert should have been created
        alerts = client.get('/api/alerts', params={'device_id': 'ESP32-TEST'}).json()['data']
        assert len(alerts) == 1 and alerts[0]['severity'] == 'critical'

        # Acknowledge alert
        ack = client.patch(f"/api/alerts/{alerts[0]['id']}/acknowledge")
        assert ack.status_code == 200 and ack.json()['data']['status'] == 'Acknowledged'

        # Workers endpoint
        workers_res = client.get('/api/workers')
        assert workers_res.status_code == 200
        wd = workers_res.json()
        assert 'total_workers' in wd
        assert len(wd['data']) >= 1

        # Fall simulation with kinematic proof
        sim = client.post('/api/incidents/simulate-fall', json={
            'device_id': 'ESP32-001', 'peak_impact_g': 3.65,
            'free_fall_duration_ms': 260, 'rest_variation_g': 0.11,
            'stable_samples': 25, 'heart_rate': 125, 'spo2': 97, 'temperature': 37.0
        })
        assert sim.status_code == 200
        assert sim.json()['status'] == 'simulated_fall_created'
        assert sim.json()['details']['fall_confirmed'] is True
        assert sim.json()['details']['proof'] is not None
        assert sim.json()['details']['proof']['verified'] is True

        # Fall incidents should be logged
        falls = client.get('/api/incidents/falls').json()['data']
        assert len(falls) >= 1
        assert falls[0]['proof'] is not None

        # Worker fall count should have incremented
        w = client.get('/api/workers/ESP32-001').json()['data']
        assert w['fall_count'] >= 1

        # History endpoint should note source
        hist = client.get('/api/readings/history', params={'device_id': 'ESP32-TEST', 'hours': 1})
        assert hist.status_code == 200
        assert 'source' in hist.json()

        # Resolve an alert
        resolve = client.patch(f"/api/alerts/{alerts[0]['id']}/resolve")
        assert resolve.status_code == 200
        assert resolve.json()['data']['status'] == 'Resolved'
