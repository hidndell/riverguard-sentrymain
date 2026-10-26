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
   - Validates phone format (`Invalid Phone Number Format`), prevents duplicates (`Number Already Registered`), confirms registration (`Registration Successful`), and exposes registered numbers to the SIM800L gateway via `/api/get_numbers`.
3. **Administrator Log In (`/login`)**:
   - Authenticates pre-registered administrator username/email and password against database records (`Incorrect Username or password` on invalid credentials).
4. **View & Delete Registered Numbers (`/`, Admin Directory)**:
   - Logged-in administrators can view all gathered SIM numbers, names, and optional details, and delete entries with confirmation (`DELETE /api/admin/numbers/<id>`).
5. **Administrator Log Out (`/logout`)**:
   - Immediately destroys the active session and redirects to the home dashboard.

---

## Deploying on Render (Same Workflow as BriquetteTrack)

1. In [Render Dashboard](https://dashboard.render.com/), create a **Blueprint** or **Web Service** connected to `https://github.com/hidndell/riverguard-sentrymain`.
2. Build & Start commands (configured automatically via `render.yaml` / `Procfile`):
   - **Build Command**: `pip install -r requirements.txt`
   - **Start Command**: `gunicorn app:app --bind 0.0.0.0:$PORT --workers 1 --threads 8 --timeout 60`
3. **Environment Variables**:
   - `ADMIN_USERNAME` (default: `admin`)
   - `ADMIN_EMAIL` (default: `admin@riverguard.com`)
   - `ADMIN_PASSWORD` (default: `riverguard2026` — change in Render Environment Variables for production)
   - `DATABASE_URL` *(Optional)*: Connect a TiDB Cloud Serverless or MySQL database (`mysql://...`) for persistent storage across container restarts; otherwise runs automatically on high-concurrency SQLite WAL.
