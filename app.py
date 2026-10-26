import os
import re
import ssl
import hmac
import time
import sqlite3
import threading
from datetime import datetime, timezone
from functools import wraps
from urllib.parse import urlparse
from flask import (
    Flask, request, jsonify, render_template,
    redirect, url_for, session, Response
)
from flask_cors import CORS
from werkzeug.security import generate_password_hash, check_password_hash

try:
    import pymysql
    import pymysql.cursors
    HAS_PYMYSQL = True
except ImportError:
    HAS_PYMYSQL = False

app = Flask(__name__)
app.secret_key = os.getenv('SECRET_KEY', 'riverguard-capstone-secret-key-2026')
CORS(app)

db_path = os.path.join(app.root_path, 'river.db')

# Shared API key between ESP32 Main Gateway and Flask Server
# Override GATEWAY_API_KEY in Render Environment Variables for production!
DEFAULT_GATEWAY_KEY = 'rg-gateway-2026-key'

# =========================================================
# THREAD-SAFE IN-MEMORY CACHE FOR MULTI-USER SCALABILITY
# =========================================================
_cache_lock = threading.Lock()
_telemetry_cache = {
    "rows": None,
    "updated_at": 0.0
}
CACHE_TTL_SECONDS = 2.0


def get_gateway_api_key():
    return os.getenv('GATEWAY_API_KEY', DEFAULT_GATEWAY_KEY).strip()


def gateway_key_required(f):
    """
    Protects hardware endpoints (/api/upload and /api/get_numbers) using X-API-Key
    so registered mobile numbers and sensor logs are secure from unauthorized access.
    """
    @wraps(f)
    def decorated(*args, **kwargs):
        expected_key = get_gateway_api_key()

        # Check X-API-Key header first (recommended), then Authorization: Bearer, then form/JSON/query fallback
        provided_key = request.headers.get('X-API-Key', '').strip()
        if not provided_key:
            auth_header = request.headers.get('Authorization', '').strip()
            if auth_header.lower().startswith('bearer '):
                provided_key = auth_header[7:].strip()
        if not provided_key:
            provided_key = (
                request.args.get('api_key')
                or request.form.get('api_key')
                or ((request.get_json(silent=True) or {}).get('api_key') if request.is_json else None)
                or ''
            ).strip()

        if not provided_key or not hmac.compare_digest(provided_key, expected_key):
            return jsonify({"error": "Unauthorized: Invalid or missing X-API-Key"}), 401

        return f(*args, **kwargs)
    return decorated


def is_mysql_configured():
    """Check if Cloud MySQL / TiDB Serverless environment variables are set."""
    if not HAS_PYMYSQL:
        return False
    return bool(os.getenv('DATABASE_URL') or os.getenv('DB_URL') or os.getenv('DB_HOST'))


def get_mysql_connection():
    """Connects to Cloud MySQL / TiDB Serverless (same deployment pattern as BriquetteTrack)."""
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
    """High-concurrency SQLite connection with WAL mode enabled."""
    conn = sqlite3.connect(db_path, timeout=15.0, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    return conn


# =========================================================
# 1. INITIALIZE DATABASE (Sensor Logs, Registered Numbers, Admins)
# =========================================================
def init_db():
    default_admin_user = os.getenv('ADMIN_USERNAME', 'admin')
    default_admin_email = os.getenv('ADMIN_EMAIL', 'admin@riverguard.com')
    default_admin_pass = os.getenv('ADMIN_PASSWORD', 'riverguard2026')
    password_hash = generate_password_hash(default_admin_pass)

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
                        node3_distance INT DEFAULT 999
                    )
                ''')
                cursor.execute('''
                    CREATE TABLE IF NOT EXISTS registered_numbers (
                        id INT AUTO_INCREMENT PRIMARY KEY,
                        mobile_number VARCHAR(20) NOT NULL UNIQUE,
                        full_name VARCHAR(120) DEFAULT '',
                        location VARCHAR(160) DEFAULT '',
                        additional_details VARCHAR(255) DEFAULT '',
                        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
                    )
                ''')
                cursor.execute('''
                    CREATE TABLE IF NOT EXISTS admins (
                        id INT AUTO_INCREMENT PRIMARY KEY,
                        username VARCHAR(80) NOT NULL UNIQUE,
                        email VARCHAR(120) NOT NULL UNIQUE,
                        password_hash VARCHAR(255) NOT NULL,
                        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
                    )
                ''')
                cursor.execute("SELECT COUNT(*) AS cnt FROM admins")
                if cursor.fetchone()['cnt'] == 0:
                    cursor.execute('''
                        INSERT INTO admins (username, email, password_hash)
                        VALUES (%s, %s, %s)
                    ''', (default_admin_user, default_admin_email, password_hash))
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
            node3_distance INTEGER DEFAULT 999
        )
    ''')

    existing_cols = {
        row['name'] for row in cursor.execute("PRAGMA table_info(sensor_logs)").fetchall()
    }
    if 'node2_distance' not in existing_cols:
        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN node2_distance INTEGER DEFAULT 999")
    if 'node3_distance' not in existing_cols:
        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN node3_distance INTEGER DEFAULT 999")

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS registered_numbers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            mobile_number TEXT NOT NULL UNIQUE,
            full_name TEXT DEFAULT '',
            location TEXT DEFAULT '',
            additional_details TEXT DEFAULT '',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS admins (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            email TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')

    admin_count = cursor.execute("SELECT COUNT(*) FROM admins").fetchone()[0]
    if admin_count == 0:
        cursor.execute('''
            INSERT INTO admins (username, email, password_hash)
            VALUES (?, ?, ?)
        ''', (default_admin_user, default_admin_email, password_hash))

    conn.commit()
    conn.close()


init_db()


def admin_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not session.get('admin_logged_in'):
            return jsonify({"error": "Unauthorized"}), 401
        return f(*args, **kwargs)
    return decorated


def compute_threat_mode(distances):
    """Auto-computes threat level if ESP32 omits explicit mode/threat field."""
    highest = 1
    for d in distances:
        if d is None:
            continue
        if 0 < d < 180:
            return 3
        elif 180 <= d < 300 and highest < 3:
            highest = 2
    return highest


def validate_and_normalize_phone(raw_number):
    """
    Validates mobile number according to Table 3 Use Case Narrative:
    - Rejects text, spaces, or incomplete numbers ("Invalid Phone Number Format")
    - Accepts Philippine SIM format 09XXXXXXXXX (11 digits) or +639XXXXXXXXX (13 chars)
    - Normalizes to +639XXXXXXXXX for SIM800L GSM SMS Gateway compatibility
    """
    if raw_number is None or not isinstance(raw_number, str) or raw_number == '':
        return None
    if any(ch.isspace() for ch in raw_number):
        return None

    if re.fullmatch(r'09\d{9}', raw_number):
        return '+63' + raw_number[1:]
    if re.fullmatch(r'\+639\d{9}', raw_number):
        return raw_number
    return None


# =========================================================
# 2. ESP32 MAIN GATEWAY UPLOAD ROUTE (/api/upload) — API KEY PROTECTED
# =========================================================
@app.route('/api/upload', methods=['POST'])
@gateway_key_required
def receive_hardware_data():
    """
    Receives real sensor telemetry from the ESP32 Main Gateway (protected by X-API-Key).
    Compatible with both JSON and form-urlencoded payloads and 1-3 soldier nodes:
      - JSON: {"node_1": 250, "node_2": 310, "node_3": 999, "threat": 2}
      - Form: mode=2&node1=250&node2=310
    """
    try:
        payload = {}
        if request.is_json:
            payload = request.get_json(silent=True) or {}
        elif request.form:
            payload = request.form.to_dict()
        else:
            payload = request.get_json(force=True, silent=True) or {}

        raw_mode = payload.get('mode', payload.get('threat', payload.get('global_mode')))
        raw_n1 = payload.get('node1', payload.get('node_1', payload.get('distance')))
        raw_n2 = payload.get('node2', payload.get('node_2', 999))
        raw_n3 = payload.get('node3', payload.get('node_3', 999))

        if raw_n1 is None and raw_mode is None:
            return jsonify({"error": "Missing sensor node or threat mode data"}), 400

        node1_dist = int(float(raw_n1)) if raw_n1 is not None else 999
        node2_dist = int(float(raw_n2)) if raw_n2 is not None else 999
        node3_dist = int(float(raw_n3)) if raw_n3 is not None else 999

        if raw_mode is not None:
            system_mode = int(raw_mode)
        else:
            system_mode = compute_threat_mode([node1_dist, node2_dist, node3_dist])

        utc_now_str = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')

        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        INSERT INTO sensor_logs
                        (timestamp, global_mode, node1_distance, node2_distance, node3_distance)
                        VALUES (%s, %s, %s, %s, %s)
                    ''', (utc_now_str, system_mode, node1_dist, node2_dist, node3_dist))
                conn.commit()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                conn.execute('''
                    INSERT INTO sensor_logs
                    (timestamp, global_mode, node1_distance, node2_distance, node3_distance)
                    VALUES (?, ?, ?, ?, ?)
                ''', (utc_now_str, system_mode, node1_dist, node2_dist, node3_dist))
                conn.commit()
            finally:
                conn.close()

        with _cache_lock:
            _telemetry_cache["rows"] = None
            _telemetry_cache["updated_at"] = 0.0

        return "OK", 200

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 3. FETCH LATEST SENSOR READINGS (/api/get_latest)
# =========================================================
@app.route('/api/get_latest', methods=['GET'])
def get_latest_data():
    """Returns latest 20 real gateway readings (empty list if gateway hasn't sent data yet)."""
    try:
        now = time.time()
        with _cache_lock:
            if _telemetry_cache["rows"] is not None and (now - _telemetry_cache["updated_at"]) < CACHE_TTL_SECONDS:
                return jsonify(_telemetry_cache["rows"]), 200

        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        SELECT timestamp, global_mode, node1_distance, node2_distance, node3_distance
                        FROM sensor_logs
                        ORDER BY id DESC LIMIT 20
                    ''')
                    rows = cursor.fetchall()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                cursor = conn.cursor()
                cursor.execute('''
                    SELECT timestamp, global_mode, node1_distance, node2_distance, node3_distance
                    FROM sensor_logs
                    ORDER BY id DESC LIMIT 20
                ''')
                rows = [dict(r) for r in cursor.fetchall()]
            finally:
                conn.close()

        data = []
        for r in rows:
            ts_val = r['timestamp']
            ts_str = ts_val.strftime('%Y-%m-%d %H:%M:%S') if isinstance(ts_val, datetime) else str(ts_val)
            n1 = int(r['node1_distance']) if r['node1_distance'] is not None else 999
            n2 = int(r['node2_distance']) if r.get('node2_distance') is not None else 999
            n3 = int(r['node3_distance']) if r.get('node3_distance') is not None else 999
            data.append({
                "time": ts_str,
                "mode": int(r['global_mode']),
                "distance": n1,
                "node1": n1,
                "node2": n2,
                "node3": n3
            })

        with _cache_lock:
            _telemetry_cache["rows"] = data
            _telemetry_cache["updated_at"] = now

        return jsonify(data), 200

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 4. SMS GATEWAY FETCH NUMBERS ROUTE (/api/get_numbers) — API KEY PROTECTED
# =========================================================
@app.route('/api/get_numbers', methods=['GET'])
@gateway_key_required
def get_numbers():
    """
    Returns active registered mobile numbers for the ESP32 SIM800L module (protected by X-API-Key).
    - Default response: plain-text comma-separated (+639171111111,+639182222222)
    - Optional ?format=json response: {"numbers": ["+639171111111", ...], "count": N}
    """
    try:
        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute("SELECT mobile_number FROM registered_numbers ORDER BY id ASC")
                    nums = [row['mobile_number'] for row in cursor.fetchall()]
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                rows = conn.execute("SELECT mobile_number FROM registered_numbers ORDER BY id ASC").fetchall()
                nums = [row['mobile_number'] for row in rows]
            finally:
                conn.close()

        if request.args.get('format', '').lower() == 'json':
            return jsonify({"numbers": nums, "count": len(nums)}), 200

        return Response(",".join(nums), mimetype='text/plain', status=200)
    except Exception as e:
        if request.args.get('format', '').lower() == 'json':
            return jsonify({"error": str(e), "numbers": []}), 500
        return Response("", mimetype='text/plain', status=200)


# =========================================================
# 5. USE CASE: VIEW WATER MONITORING (Home Dashboard)
# =========================================================
@app.route('/')
def index():
    return render_template(
        'index.html',
        is_admin=bool(session.get('admin_logged_in')),
        admin_username=session.get('admin_username', '')
    )


# =========================================================
# 6. USE CASE: REGISTER MOBILE NUMBER (/register & /api/register)
# =========================================================
@app.route('/register', methods=['GET'])
def register_page():
    return render_template(
        'register.html',
        is_admin=bool(session.get('admin_logged_in'))
    )


@app.route('/api/register', methods=['POST'])
def register_mobile_number():
    try:
        body = request.get_json(silent=True) or request.form.to_dict()
        raw_phone = body.get('mobile_number', '')
        full_name = str(body.get('full_name', '')).strip()[:120]
        location = str(body.get('location', '')).strip()[:160]
        additional_details = str(body.get('additional_details', '')).strip()[:255]
        privacy_agreed = body.get('privacy_consent')

        if not privacy_agreed:
            return jsonify({"error": "Please agree to the Data Privacy Policy before registering."}), 400

        normalized_phone = validate_and_normalize_phone(raw_phone)
        if not normalized_phone:
            return jsonify({"error": "Invalid Phone Number Format"}), 400

        utc_now_str = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')

        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute(
                        "SELECT id FROM registered_numbers WHERE mobile_number = %s",
                        (normalized_phone,)
                    )
                    if cursor.fetchone():
                        return jsonify({"error": "Number Already Registered"}), 409

                    cursor.execute('''
                        INSERT INTO registered_numbers
                        (mobile_number, full_name, location, additional_details, created_at)
                        VALUES (%s, %s, %s, %s, %s)
                    ''', (normalized_phone, full_name, location, additional_details, utc_now_str))
                conn.commit()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                existing = conn.execute(
                    "SELECT id FROM registered_numbers WHERE mobile_number = ?",
                    (normalized_phone,)
                ).fetchone()
                if existing:
                    return jsonify({"error": "Number Already Registered"}), 409

                conn.execute('''
                    INSERT INTO registered_numbers
                    (mobile_number, full_name, location, additional_details, created_at)
                    VALUES (?, ?, ?, ?, ?)
                ''', (normalized_phone, full_name, location, additional_details, utc_now_str))
                conn.commit()
            finally:
                conn.close()

        return jsonify({
            "message": "Registration Successful",
            "mobile_number": normalized_phone
        }), 201

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 7. USE CASE: ADMIN LOG IN & LOG OUT (/login, /logout)
# =========================================================
@app.route('/login', methods=['GET', 'POST'])
def admin_login():
    if session.get('admin_logged_in'):
        return redirect(url_for('index'))

    error_msg = None
    if request.method == 'POST':
        identifier = request.form.get('identifier', '').strip()
        password = request.form.get('password', '')

        admin_row = None
        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute(
                        "SELECT id, username, email, password_hash FROM admins WHERE username = %s OR email = %s",
                        (identifier, identifier)
                    )
                    admin_row = cursor.fetchone()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                row = conn.execute(
                    "SELECT id, username, email, password_hash FROM admins WHERE username = ? OR email = ?",
                    (identifier, identifier)
                ).fetchone()
                admin_row = dict(row) if row else None
            finally:
                conn.close()

        if admin_row and check_password_hash(admin_row['password_hash'], password):
            session['admin_logged_in'] = True
            session['admin_id'] = admin_row['id']
            session['admin_username'] = admin_row['username']
            return redirect(url_for('index'))
        else:
            error_msg = "Incorrect Username or password"

    return render_template('login.html', error=error_msg)


@app.route('/logout', methods=['GET', 'POST'])
def admin_logout():
    session.clear()
    return redirect(url_for('index'))


# =========================================================
# 8. USE CASE: ADMIN CONTACT DIRECTORY & DELETE NUMBERS
# =========================================================
@app.route('/api/admin/numbers', methods=['GET'])
@admin_required
def list_registered_numbers():
    try:
        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        SELECT id, mobile_number, full_name, location, additional_details, created_at
                        FROM registered_numbers
                        ORDER BY id DESC
                    ''')
                    rows = cursor.fetchall()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                cursor = conn.cursor()
                cursor.execute('''
                    SELECT id, mobile_number, full_name, location, additional_details, created_at
                    FROM registered_numbers
                    ORDER BY id DESC
                ''')
                rows = [dict(r) for r in cursor.fetchall()]
            finally:
                conn.close()

        result = []
        for r in rows:
            ts = r['created_at']
            ts_str = ts.strftime('%Y-%m-%d %H:%M:%S') if isinstance(ts, datetime) else str(ts)
            result.append({
                "id": r['id'],
                "mobile_number": r['mobile_number'],
                "full_name": r['full_name'] or '',
                "location": r['location'] or '',
                "additional_details": r['additional_details'] or '',
                "created_at": ts_str
            })
        return jsonify(result), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route('/api/admin/numbers/<int:record_id>', methods=['DELETE'])
@admin_required
def delete_registered_number(record_id):
    try:
        deleted = 0
        if is_mysql_configured():
            conn = get_mysql_connection()
            try:
                with conn.cursor() as cursor:
                    deleted = cursor.execute(
                        "DELETE FROM registered_numbers WHERE id = %s",
                        (record_id,)
                    )
                conn.commit()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                cur = conn.execute(
                    "DELETE FROM registered_numbers WHERE id = ?",
                    (record_id,)
                )
                deleted = cur.rowcount
                conn.commit()
            finally:
                conn.close()

        if deleted == 0:
            return jsonify({"error": "Delete Operation Failed. Please Try Again"}), 404

        return jsonify({"message": "Number deleted successfully"}), 200
    except Exception:
        return jsonify({"error": "Delete Operation Failed. Please Try Again"}), 500


if __name__ == '__main__':
    port = int(os.getenv('PORT', 5000))
    app.run(host='0.0.0.0', port=port, debug=True)
