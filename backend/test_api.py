import os
from fastapi.testclient import TestClient
from main import app, init_db

def test_api(tmp_path, monkeypatch):
    import main
    monkeypatch.setattr(main, 'DB_PATH', tmp_path / 'test.db')
    with TestClient(app) as client:
        assert client.get('/api/health').status_code == 200
        bad=client.post('/api/readings',json={'device_id':'ESP32-TEST'})
        assert bad.status_code == 422
        good=client.post('/api/readings',json={'device_id':'ESP32-TEST','heart_rate':140,'spo2':98,'simulated':False})
        assert good.status_code == 201
        alerts=client.get('/api/alerts',params={'device_id':'ESP32-TEST'}).json()['data']
        assert len(alerts)==1 and alerts[0]['severity']=='critical'
        ack=client.patch(f"/api/alerts/{alerts[0]['id']}/acknowledge")
        assert ack.status_code == 200 and ack.json()['data']['status']=='Acknowledged'
