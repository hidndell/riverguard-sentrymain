import os
import ssl
import time
import sqlite3
import threading
from datetime import datetime, timezone
from urllib.parse import urlparse
from flask import Flask, request, jsonify, render_template, Response
from flask_cors import CORS

try:
    import pymysql
    import pymysql.cursors
    HAS_PYMYSQL = True
except ImportError:
    HAS_PYMYSQL = False

app = Flask(__name__)
CORS(app)

db_path = os.path.join(app.root_path, 'river.db')

# =========================================================
# THREAD-SAFE IN-MEMORY CACHE FOR HIGH CONCURRENCY
# =========================================================
# Prevents database lock contention when many users view the
# live dashboard simultaneously while the ESP32 uploads data.
_cache_lock = threading.Lock()
_telemetry_cache = {
    "rows": None,
    "updated_at": 0.0,
    "total_uploads": 0,
    "last_upload_ts": None
}
CACHE_TTL_SECONDS = 2.0


def is_mysql_configured():
    """Check if Cloud MySQL / TiDB Serverless environment variables are configured."""
    if not HAS_PYMYSQL:
        return False
    return bool(os.getenv('DATABASE_URL') or os.getenv('DB_URL') or os.getenv('DB_HOST'))


def get_mysql_connection():
    """
    Establishes connection to Cloud MySQL (TiDB Cloud Serverless, Aiven, etc.)
    using the same pattern as BriquetteTrack_Project.
    """
    db_url = os.getenv('DATABASE_URL') or os.getenv('DB_URL')
    if db_url:
        parsed = urlparse(db_url)
        host = parsed.hostname or 'localhost'
        port = parsed.port or 3306
        user = parsed.username or 'root'
        password = parsed.password or ''
        database = parsed.path.lstrip('/') or 'riverguard_db'
    else:
        host = os.getenv('DB_HOST', 'localhost')
        user = os.getenv('DB_USER', 'root')
        password = os.getenv('DB_PASSWORD', '')
        database = os.getenv('DB_NAME', 'riverguard_db')
        port = int(os.getenv('DB_PORT', 3306))

    connect_kwargs = {
        'host': host,
        'user': user,
        'password': password,
        'database': database,
        'port': port,
        'cursorclass': pymysql.cursors.DictCursor,
        'connect_timeout': 10
    }

    is_remote = host not in ('localhost', '127.0.0.1', 'db')
    force_ssl = os.getenv('DB_SSL', '').lower() in ('true', '1', 'yes')
    disable_ssl = os.getenv('DB_SSL', '').lower() in ('false', '0', 'no') or os.getenv('DB_SSL_DISABLED')

    if (is_remote and not disable_ssl) or force_ssl:
        ssl_ctx = ssl.create_default_context()
        ssl_ctx.check_hostname = False
        ssl_ctx.verify_mode = ssl.CERT_NONE
        connect_kwargs['ssl'] = ssl_ctx

    return pymysql.connect(**connect_kwargs)


def get_sqlite_connection():
    """
    Establishes a high-concurrency SQLite connection using WAL (Write-Ahead Logging)
    mode so multiple Gunicorn threads can read and write simultaneously without locking.
    """
    conn = sqlite3.connect(db_path, timeout=15.0, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    return conn


# =========================================================
# 1. INITIALIZE & MIGRATE DATABASE
# =========================================================
def init_db():
    if is_mysql_configured():
        try:
            conn = get_mysql_connection()
            with conn.cursor() as cursor:
                cursor.execute('''
                    CREATE TABLE IF NOT EXISTS sensor_logs (
                        id INT AUTO_INCREMENT PRIMARY KEY,
                        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                        global_mode INT NOT NULL DEFAULT 1,
                        node1_distance INT NOT NULL DEFAULT 999,
                        node2_distance INT DEFAULT 999,
                        node3_distance INT DEFAULT 999,
                        rssi FLOAT DEFAULT NULL,
                        snr FLOAT DEFAULT NULL,
                        source VARCHAR(32) DEFAULT 'esp32'
                    )
                ''')
                cursor.execute('''
                    CREATE TABLE IF NOT EXISTS system_settings (
                        setting_key VARCHAR(64) PRIMARY KEY,
                        setting_value TEXT NOT NULL,
                        updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
                    )
                ''')
            conn.commit()
            conn.close()
            return
        except Exception as e:
            print(f"[WARN] Cloud MySQL init failed ({e}), falling back to SQLite WAL.")

    conn = get_sqlite_connection()
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS sensor_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            global_mode INTEGER,
            node1_distance INTEGER,
            node2_distance INTEGER DEFAULT 999,
            node3_distance INTEGER DEFAULT 999,
            rssi REAL DEFAULT NULL,
            snr REAL DEFAULT NULL,
            source TEXT DEFAULT 'esp32'
        )
    ''')

    # Safely migrate existing pre-oral defense river.db if new columns are missing
    existing_cols = {
        row['name'] for row in cursor.execute("PRAGMA table_info(sensor_logs)").fetchall()
    }
    migrations = [
        ("node2_distance", "INTEGER DEFAULT 999"),
        ("node3_distance", "INTEGER DEFAULT 999"),
        ("rssi", "REAL DEFAULT NULL"),
        ("snr", "REAL DEFAULT NULL"),
        ("source", "TEXT DEFAULT 'esp32'")
    ]
    for col_name, col_def in migrations:
        if col_name not in existing_cols:
            cursor.execute(f"ALTER TABLE sensor_logs ADD COLUMN {col_name} {col_def}")

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS system_settings (
            setting_key TEXT PRIMARY KEY,
            setting_value TEXT NOT NULL,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')

    # Ensure default emergency number exists
    default_num = os.getenv('EMERGENCY_NUMBERS', '+639123456789')
    cursor.execute('''
        INSERT OR IGNORE INTO system_settings (setting_key, setting_value)
        VALUES ('emergency_numbers', ?)
    ''', (default_num,))

    conn.commit()
    conn.close()


init_db()


def compute_threat_mode(distances):
    """
    Matches ESP32 firmware updateGlobalThreatLevel():
    < 180 cm (and > 0) -> Mode 3 (CRITICAL)
    180 to 299 cm      -> Mode 2 (WARNING)
    >= 300 cm          -> Mode 1 (SAFE)
    """
    highest = 1
    for d in distances:
        if d is None:
            continue
        if 0 < d < 180:
            return 3
        elif 180 <= d < 300 and highest < 3:
            highest = 2
    return highest


def insert_sensor_reading(mode, node1, node2=999, node3=999, rssi=None, snr=None, source='esp32'):
    """Inserts a telemetry row into MySQL or SQLite and invalidates the read cache."""
    utc_now_str = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')

    if is_mysql_configured():
        conn = get_mysql_connection()
        try:
            with conn.cursor() as cursor:
                cursor.execute('''
                    INSERT INTO sensor_logs
                    (timestamp, global_mode, node1_distance, node2_distance, node3_distance, rssi, snr, source)
                    VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                ''', (utc_now_str, mode, node1, node2, node3, rssi, snr, source))
            conn.commit()
        finally:
            conn.close()
    else:
        conn = get_sqlite_connection()
        try:
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO sensor_logs
                (timestamp, global_mode, node1_distance, node2_distance, node3_distance, rssi, snr, source)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            ''', (utc_now_str, mode, node1, node2, node3, rssi, snr, source))
            conn.commit()
        finally:
            conn.close()

    with _cache_lock:
        _telemetry_cache["rows"] = None
        _telemetry_cache["updated_at"] = 0.0
        _telemetry_cache["total_uploads"] += 1
        _telemetry_cache["last_upload_ts"] = utc_now_str


def query_latest_readings(limit=50):
    """Fetches latest readings using the thread-safe cache when limit <= 50."""
    now = time.time()
    with _cache_lock:
        if (
            limit <= 50
            and _telemetry_cache["rows"] is not None
            and (now - _telemetry_cache["updated_at"]) < CACHE_TTL_SECONDS
        ):
            return _telemetry_cache["rows"][:limit]

    rows_formatted = []
    if is_mysql_configured():
        conn = get_mysql_connection()
        try:
            with conn.cursor() as cursor:
                cursor.execute('''
                    SELECT id, timestamp, global_mode, node1_distance,
                           node2_distance, node3_distance, rssi, snr, source
                    FROM sensor_logs
                    ORDER BY id DESC LIMIT %s
                ''', (max(limit, 50),))
                rows = cursor.fetchall()
        finally:
            conn.close()
    else:
        conn = get_sqlite_connection()
        try:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT id, timestamp, global_mode, node1_distance,
                       node2_distance, node3_distance, rssi, snr, source
                FROM sensor_logs
                ORDER BY id DESC LIMIT ?
            ''', (max(limit, 50),))
            rows = [dict(r) for r in cursor.fetchall()]
        finally:
            conn.close()

    for r in rows:
        ts_val = r['timestamp']
        if isinstance(ts_val, datetime):
            ts_str = ts_val.strftime('%Y-%m-%d %H:%M:%S')
        else:
            ts_str = str(ts_val)

        n1 = int(r['node1_distance']) if r['node1_distance'] is not None else 999
        n2 = int(r['node2_distance']) if r.get('node2_distance') is not None else 999
        n3 = int(r['node3_distance']) if r.get('node3_distance') is not None else 999
        mode = int(r['global_mode']) if r['global_mode'] is not None else 1

        rows_formatted.append({
            "id": r['id'],
            "time": ts_str,
            "mode": mode,
            "distance": n1,       # Backward-compatible field for Pre-Oral Defense chart
            "node1": n1,
            "node2": n2,
            "node3": n3,
            "rssi": r.get('rssi'),
            "snr": r.get('snr'),
            "source": r.get('source') or 'esp32'
        })

    with _cache_lock:
        if limit <= 50:
            _telemetry_cache["rows"] = rows_formatted
            _telemetry_cache["updated_at"] = now

    return rows_formatted[:limit]


# =========================================================
# 2. ESP32 UPLOAD ROUTE (Supports BOTH Form & JSON Payloads)
# =========================================================
@app.route('/api/upload', methods=['POST'])
def receive_hardware_data():
    """
    Accepts telemetry from ESP32 Gateway:
    - Form data (Pre-Oral Defense): mode=1&node1=250[&node2=...&node3=...]
    - JSON data (Final Defense):    {"node_1": 250, "node_2": 310, "threat": 1}
    """
    try:
        payload = {}
        if request.is_json:
            payload = request.get_json(silent=True) or {}
        elif request.form:
            payload = request.form.to_dict()
        else:
            # Fallback if ESP32 sent JSON without application/json header
            payload = request.get_json(force=True, silent=True) or {}

        raw_mode = payload.get('mode', payload.get('threat'))
        raw_n1 = payload.get('node1', payload.get('node_1', payload.get('distance')))
        raw_n2 = payload.get('node2', payload.get('node_2', 999))
        raw_n3 = payload.get('node3', payload.get('node_3', 999))
        raw_rssi = payload.get('rssi')
        raw_snr = payload.get('snr')
        source = payload.get('source', 'esp32')

        if raw_n1 is None and raw_mode is None:
            return jsonify({"error": "Missing mode/threat or node1/node_1 data"}), 400

        node1_dist = int(float(raw_n1)) if raw_n1 is not None else 999
        node2_dist = int(float(raw_n2)) if raw_n2 is not None else 999
        node3_dist = int(float(raw_n3)) if raw_n3 is not None else 999

        if raw_mode is not None:
            system_mode = int(raw_mode)
        else:
            system_mode = compute_threat_mode([node1_dist, node2_dist, node3_dist])

        rssi_val = float(raw_rssi) if raw_rssi not in (None, '') else None
        snr_val = float(raw_snr) if raw_snr not in (None, '') else None

        insert_sensor_reading(
            mode=system_mode,
            node1=node1_dist,
            node2=node2_dist,
            node3=node3_dist,
            rssi=rssi_val,
            snr=snr_val,
            source=source
        )

        # Keep "OK" plain-text response for ESP32 firmware compatibility
        return "OK", 200

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 3. FETCH DATA ROUTES (For Live Dashboard)
# =========================================================
@app.route('/api/get_latest', methods=['GET'])
def get_latest_data():
    try:
        limit = request.args.get('limit', default=20, type=int)
        limit = max(1, min(limit, 200))
        data = query_latest_readings(limit=limit)
        return jsonify(data), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route('/api/health', methods=['GET'])
@app.route('/api/status', methods=['GET'])
def system_health():
    """Returns server status, storage engine, and latest reading summary."""
    try:
        latest = query_latest_readings(limit=1)
        return jsonify({
            "status": "online",
            "service": "RiverGuard Sentry API",
            "version": "2.0.0",
            "database_engine": "Cloud MySQL" if is_mysql_configured() else "SQLite WAL (High-Concurrency)",
            "latest_reading": latest[0] if latest else None,
            "server_time_utc": datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')
        }), 200
    except Exception as e:
        return jsonify({"status": "error", "error": str(e)}), 500


# =========================================================
# 4. EMERGENCY SMS SUBSCRIBER NUMBERS ROUTE
# =========================================================
def get_stored_emergency_numbers():
    default_num = os.getenv('EMERGENCY_NUMBERS', '+639123456789')
    try:
        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute(
                        "SELECT setting_value FROM system_settings WHERE setting_key = 'emergency_numbers'"
                    )
                    row = cursor.fetchone()
                    return row['setting_value'] if row else default_num
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                row = conn.execute(
                    "SELECT setting_value FROM system_settings WHERE setting_key = 'emergency_numbers'"
                ).fetchone()
                return row['setting_value'] if row else default_num
            finally:
                conn.close()
    except Exception:
        return default_num


@app.route('/api/get_numbers', methods=['GET'])
def get_numbers():
    """Plain-text endpoint consumed directly by ESP32 SIM800L HTTPClient."""
    numbers = get_stored_emergency_numbers()
    return Response(numbers.strip(), mimetype='text/plain', status=200)


@app.route('/api/numbers', methods=['GET', 'POST'])
def manage_numbers():
    """JSON endpoint for the RiverGuard Web Dashboard to view or update SMS alert numbers."""
    if request.method == 'GET':
        raw = get_stored_emergency_numbers()
        num_list = [n.strip() for n in raw.split(',') if n.strip()]
        return jsonify({"primary": num_list[0] if num_list else "+639123456789", "numbers": num_list, "raw": raw}), 200

    try:
        body = request.get_json(silent=True) or {}
        new_numbers = str(body.get('numbers', '')).strip()
        if not new_numbers:
            return jsonify({"error": "Phone number cannot be empty"}), 400

        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        INSERT INTO system_settings (setting_key, setting_value)
                        VALUES ('emergency_numbers', %s)
                        ON DUPLICATE KEY UPDATE setting_value = VALUES(setting_value)
                    ''', (new_numbers,))
                conn.commit()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                conn.execute('''
                    INSERT INTO system_settings (setting_key, setting_value, updated_at)
                    VALUES ('emergency_numbers', ?, CURRENT_TIMESTAMP)
                    ON CONFLICT(setting_key) DO UPDATE SET
                        setting_value = excluded.setting_value,
                        updated_at = CURRENT_TIMESTAMP
                ''', (new_numbers,))
                conn.commit()
            finally:
                conn.close()

        return jsonify({"status": "updated", "raw": new_numbers}), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 5. SIMULATION & DEMO SEED ROUTE (For Defense / Testing)
# =========================================================
@app.route('/api/simulate', methods=['POST'])
def simulate_reading():
    """
    Allows testing the dashboard or demonstrating Safe / Warning / Critical
    scenarios during oral/final defense without waiting for physical flood water.
    """
    try:
        body = request.get_json(silent=True) or {}
        scenario = body.get('scenario')

        if scenario == 'safe':
            n1, n2, n3, rssi, snr = 345, 360, 355, -68.0, 9.5
        elif scenario == 'warning':
            n1, n2, n3, rssi, snr = 225, 260, 290, -74.0, 7.8
        elif scenario == 'critical':
            n1, n2, n3, rssi, snr = 142, 175, 210, -81.0, 5.2
        else:
            n1 = int(body.get('node1', 320))
            n2 = int(body.get('node2', 999))
            n3 = int(body.get('node3', 999))
            rssi = float(body.get('rssi', -70.0)) if body.get('rssi') is not None else -70.0
            snr = float(body.get('snr', 8.5)) if body.get('snr') is not None else 8.5

        active_nodes = [d for d in (n1, n2, n3) if 0 < d < 999]
        mode = body.get('mode')
        if mode is not None:
            mode = int(mode)
        else:
            mode = compute_threat_mode(active_nodes or [n1])

        insert_sensor_reading(
            mode=mode,
            node1=n1,
            node2=n2,
            node3=n3,
            rssi=rssi,
            snr=snr,
            source='simulation'
        )
        return jsonify({
            "status": "simulated",
            "mode": mode,
            "node1": n1,
            "node2": n2,
            "node3": n3
        }), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 6. FRONTEND DASHBOARD ROUTES
# =========================================================
@app.route('/')
@app.route('/dashboard')
@app.route('/app')
def index():
    return render_template('index.html')


if __name__ == '__main__':
    port = int(os.getenv('PORT', 5000))
    app.run(host='0.0.0.0', port=port, debug=True)
