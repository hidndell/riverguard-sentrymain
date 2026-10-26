# RiverGuard Sentry System — Live Cloud Dashboard & Gateway API

Autonomous **LoRa 915 MHz Mesh Flood Detection & Early Warning System** powered by ESP32, SX1262, JSN-SR04T ultrasonic sensors, and SIM800L GSM dispatch.

## Features
- **High-Concurrency Flask + Gunicorn Backend**: Thread-safe in-memory telemetry cache with **Cloud MySQL / TiDB Serverless** support (`DATABASE_URL`) and automatic **SQLite WAL** fallback so dozens of simultaneous users can view live flood telemetry without database lock contention.
- **Dual ESP32 Firmware Compatibility (`/api/upload`)**:
  - **Form-Urlencoded (`Pre_Oral_defense`)**: `mode=1&node1=320`
  - **JSON Multi-Node (`Final_Defense`)**: `{"node_1": 320, "node_2": 340, "threat": 1}`
- **Live Command-Center Dashboard (`/`)**:
  - Global Threat Level Banner (`SAFE ≥ 300 cm`, `WARNING 180–299 cm`, `CRITICAL < 180 cm`)
  - Hardware Relay Indicators (Strobe Pin 26, Siren Pin 27, SIM800L GSM)
  - Live River Cross-Section Clearance Gauge & Rate-of-Rise Delta
  - Real-Time Chart.js Trend with Threshold Markers & CSV Export
  - Dynamic Emergency SMS Number Manager (`/api/get_numbers` & `/api/numbers`)
  - Built-in **Defense Simulator** (`/api/simulate`) for live testing without hardware

---

## Deploying on Render (Same Workflow as BriquetteTrack)

1. Go to [Render Dashboard](https://dashboard.render.com/) and click **New +** -> **Blueprint** (or **Web Service**).
2. Connect your GitHub repository: `https://github.com/hidndell/riverguard-sentrymain`.
3. Render automatically detects `render.yaml` / `Procfile`:
   - **Build Command**: `pip install -r requirements.txt`
   - **Start Command**: `gunicorn app:app --bind 0.0.0.0:$PORT --workers 1 --threads 8 --timeout 60`
4. *(Optional — For Persistent Cloud Database across redeploys)*:
   - Add `DATABASE_URL` in Render **Environment Variables** pointing to your **TiDB Cloud Serverless** or **Aiven MySQL** connection string (`mysql://user:pass@host:4000/riverguard_db`).
   - If `DATABASE_URL` is omitted, the app automatically runs on high-concurrency **SQLite WAL**.
5. Update your ESP32 sketch (`databaseURL` / `serverURL` and `fetchNumbersURL`) to use your live Render HTTPS URL:
   - `https://<your-render-service>.onrender.com/api/upload`
   - `https://<your-render-service>.onrender.com/api/get_numbers`
