# RiverGuard Sentry System — Web Application

Web dashboard and SMS registry for the **RiverGuard Autonomous LoRa Mesh Flood Detection Network**, aligned strictly with the system Use Case Diagram and Use Case Narratives.

## System Use Cases
1. **View Water Monitoring (`/`)**:
   - Accessible immediately to General Users and Administrators.
   - Displays live threat level and graphical water-distance readings transmitted by the ESP32 Main Gateway (`/api/upload`), refreshing every 5 seconds (`/api/get_latest`).
   - Uses zero mock data — shows `WAITING FOR GATEWAY` until actual hardware transmissions are received.
2. **Register Mobile Number (`/register`)**:
   - Accessible via the **Register** button next to **Admin Login**.
   - Displays the Capstone Final Defense **Data Privacy Policy** and collects a required Philippine SIM card number (`09XXXXXXXXX` or `+639XXXXXXXXX`) plus optional Name, Location/Barangay, and Additional Details.
   - Validates phone format (`Invalid Phone Number Format`), prevents duplicates (`Number Already Registered`), and confirms registration (`Registration Successful`).
3. **Administrator Log In (`/login`)**:
   - Authenticates pre-registered administrator username/email and password against database records (`Incorrect Username or password` on invalid credentials).
4. **View & Delete Registered Numbers (`/`, Admin Directory)**:
   - Logged-in administrators can view all gathered SIM numbers, names, and optional details, and delete entries with confirmation (`DELETE /api/admin/numbers/<id>`).
5. **Administrator Log Out (`/logout`)**:
   - Immediately destroys the active session and redirects to the home dashboard.

---

## Secure ESP32 Main Gateway API Specification
Both hardware endpoints require the **`X-API-Key`** HTTP header matching `GATEWAY_API_KEY` (default: `rg-gateway-2026-key`):
- **`POST /api/upload`** (Header: `X-API-Key: <GATEWAY_API_KEY>`):
  - Accepts JSON `{"node_1": 250, "node_2": 310, "node_3": 999, "threat": 2}` OR form data `mode=2&node1=250`.
- **`GET /api/get_numbers`** (Header: `X-API-Key: <GATEWAY_API_KEY>`):
  - Returns comma-separated `+639XXXXXXXXX,+639YYYYYYYYY` in plain text (or JSON `{"numbers": [...], "count": N}` with `?format=json`).

---

## Deploying for FREE on Render (Without Blueprints)

Render asks for a payment card if you click "Blueprint", so use **New → Web Service** (100% Free, same method as BriquetteTrack):

1. Open [Render Dashboard](https://dashboard.render.com/), click **New +** → **Web Service** (do **not** click Blueprint).
2. Connect your GitHub repository: **`hidndell/riverguard-sentrymain`**.
3. Fill in the Web Service settings:
   - **Language / Runtime**: `Python 3`
   - **Branch**: `main`
   - **Build Command**: `pip install -r requirements.txt`
   - **Start Command**: `gunicorn app:app --bind 0.0.0.0:$PORT --workers 1 --threads 8 --timeout 60`
   - **Instance Type**: Select **Free ($0/month)**
4. Under **Environment Variables**, add:
   - `GATEWAY_API_KEY` = `rg-gateway-2026-key` *(or your own secret key — use the exact same key in your ESP32 sketch)*
   - `ADMIN_USERNAME` = `admin`
   - `ADMIN_PASSWORD` = `riverguard2026` *(change as desired)*
   - `DATABASE_URL` = *(Optional: paste your free TiDB Cloud Serverless MySQL URL `mysql://...` so registered numbers & logs persist even when Render's free container sleeps)*
5. Click **Deploy Web Service**.
