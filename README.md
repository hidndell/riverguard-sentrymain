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

## Persistent Web Database Setup (Prevents Data Loss on Render Redeploys)

Render Free Web Services wipe local container files (`river.db`) whenever you redeploy or when the container restarts. `app.py` automatically connects to a **persistent Cloud Web Database** (and auto-creates all tables + admin account on startup) as soon as you add **either** of these 100% Free cloud database options in **Render Dashboard → Your Web Service → Environment**:

### Option A: TiDB Cloud Starter — Free Serverless MySQL (Same as BriquetteTrack, $0 / No Card)
1. Go to [tidbcloud.com](https://tidbcloud.com/) → sign in with GitHub/Google → create a **Starter (Free)** cluster.
2. Click **Connect** → choose **General** or **PyMySQL** → generate password.
3. In **Render Dashboard → riverguard-sentrymain → Environment**, add either:
   - Single URL variable:
     - `DATABASE_URL` = `mysql://YOUR_USER:YOUR_PASSWORD@YOUR_TIDB_HOST:4000/riverguard_db`
   - **OR** BriquetteTrack-style separate variables:
     - `DB_HOST` = `gateway01.<region>.prod.aws.tidbcloud.com`
     - `DB_PORT` = `4000`
     - `DB_USER` = `<prefix>.root`
     - `DB_PASSWORD` = `<your_generated_password>`
     - `DB_NAME` = `riverguard_db` *(created automatically if it does not exist yet)*
4. Click **Save Changes** in Render.

### Option B: Neon.tech or Render PostgreSQL — Free Cloud PostgreSQL ($0 / No Card)
1. Go to [neon.tech](https://neon.tech/) (Free Forever tier, no credit card) or Render PostgreSQL → create a project.
2. Copy the connection string (`postgresql://...`).
3. In **Render Dashboard → riverguard-sentrymain → Environment**, add:
   - `DATABASE_URL` = `postgresql://YOUR_USER:YOUR_PASSWORD@YOUR_NEON_HOST/neondb?sslmode=require`
4. Click **Save Changes** in Render.

### Verify Persistent Web Database is Active
Open `https://riverguard-sentrymain.onrender.com/api/health` in your browser. When connected to your cloud database, it returns:
```json
{
  "configured_backend": "mysql",
  "db_backend": "mysql",
  "persistent": true,
  "status": "ok"
}
```

---

## Firmware Version History
- **Main Gateway (`ESP32 + SX1262 + SIM800L + SSD1306`)**:
  - `Main_Node_Fin03/Main_Node_Fin03.ino` — Preserved baseline with SIM800L SMS fix and Render cloud sync.
  - `Main_Node_Fin04/Main_Node_Fin04.ino` — Latest version: adds visible non-blocking **3× Blue LED blink (`Pin D2`)** whenever LoRa telemetry is received from any Soldier/Sensor Node.
- **Soldier / Sensor Node (`ESP32 + SX1262 + JSN-SR04T + MOSFET`)**:
  - `Soldier_Node_Fin02/Soldier_Node_Fin02.ino` — Preserved baseline.
  - `Soldier_Node_Fin03/Soldier_Node_Fin03.ino` — Latest version: fixes JSN-SR04T reading fluctuations by filtering valid ultrasonic echoes (`20–450 cm`) before median sorting and preventing `999` out-of-range timeouts from poisoning the Exponential Moving Average (`emaDistance`), restoring smooth incremental readings.

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
   - `DATABASE_URL` *(or `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD`, `DB_NAME` — see Persistent Web Database Setup above)*
5. Click **Deploy Web Service**.
