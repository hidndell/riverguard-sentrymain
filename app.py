import os
import re
import ssl
import hmac
import json
import time
import sqlite3
import threading
from datetime import datetime, timezone
from functools import wraps
from urllib.parse import urlparse, unquote
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

try:
    import psycopg2
    import psycopg2.extras
    HAS_PSYCOPG2 = True
except ImportError:
    HAS_PSYCOPG2 = False

app = Flask(__name__)
app.secret_key = os.getenv('SECRET_KEY', 'riverguard-capstone-secret-key-2026')
CORS(app)

db_path = os.path.join(app.root_path, 'river.db')

# Must match gatewayApiKey in Main_Node_Fin03.ino / Main_Node_Fin04.ino (override via GATEWAY_API_KEY in Render)
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


_active_db_backend = 'sqlite'
_active_env_sig = None
_db_init_error = None


def _current_db_env_sig():
    return (
        os.getenv('DATABASE_URL', '').strip(),
        os.getenv('DB_URL', '').strip(),
        os.getenv('DB_HOST', '').strip(),
        os.getenv('DB_PORT', '').strip(),
        os.getenv('DB_ENGINE', '').strip(),
        os.getenv('DB_NAME', '').strip(),
    )


def detect_configured_backend():
    """
    Detects which persistent cloud database backend is configured via environment variables:
      - 'postgresql': DATABASE_URL / DB_URL starting with postgres:// or postgresql://, or DB_ENGINE=postgres
      - 'mysql': DATABASE_URL / DB_URL starting with mysql:// / mysql+pymysql://, or DB_HOST set (BriquetteTrack style)
      - 'sqlite': Local fallback when no cloud database variables are configured
    """
    db_url = (os.getenv('DATABASE_URL') or os.getenv('DB_URL') or '').strip()
    if db_url:
        lower_url = db_url.lower()
        if lower_url.startswith(('postgres://', 'postgresql://', 'postgresql+psycopg2://')):
            return 'postgresql'
        if lower_url.startswith(('mysql://', 'mysql+pymysql://', 'mariadb://')):
            return 'mysql'

    db_host = os.getenv('DB_HOST', '').strip()
    if db_host:
        engine = os.getenv('DB_ENGINE', '').strip().lower()
        port = os.getenv('DB_PORT', '').strip()
        if engine in ('postgres', 'postgresql', 'pgsql') or port == '5432':
            return 'postgresql'
        return 'mysql'

    return 'sqlite'


def get_db_backend():
    """Returns the active initialized database backend ('mysql', 'postgresql', or 'sqlite')."""
    if _active_env_sig != _current_db_env_sig():
        init_db()
    return _active_db_backend


def is_mysql_configured():
    """Backward-compatible helper indicating active Cloud MySQL / TiDB Serverless backend."""
    return get_db_backend() == 'mysql'


def _ensure_mysql_database_exists(base_kwargs, database):
    """Automatically creates the target MySQL/TiDB database if Error 1049 (Unknown database) occurs."""
    safe_db = re.sub(r'[^A-Za-z0-9_]', '', database) or 'riverguard_db'
    for fallback_db in (None, 'test', 'defaultdb'):
        try:
            tmp_kwargs = dict(base_kwargs)
            if fallback_db:
                tmp_kwargs['database'] = fallback_db
            else:
                tmp_kwargs.pop('database', None)
            tmp_conn = pymysql.connect(**tmp_kwargs)
            try:
                with tmp_conn.cursor() as cur:
                    cur.execute(f"CREATE DATABASE IF NOT EXISTS `{safe_db}`")
                tmp_conn.commit()
                return safe_db
            finally:
                tmp_conn.close()
        except Exception:
            continue
    return database


def get_mysql_connection():
    """Connects to Cloud MySQL / TiDB Serverless (same deployment pattern as BriquetteTrack)."""
    db_url = (os.getenv('DATABASE_URL') or os.getenv('DB_URL') or '').strip()
    if db_url and db_url.lower().startswith(('mysql://', 'mysql+pymysql://', 'mariadb://')):
        parsed = urlparse(db_url)
        host = parsed.hostname or 'localhost'
        port = parsed.port or int(os.getenv('DB_PORT', 3306))
        user = unquote(parsed.username) if parsed.username else 'root'
        password = unquote(parsed.password) if parsed.password else ''
        raw_path_db = unquote(parsed.path.lstrip('/')) if parsed.path else ''
        database = raw_path_db or os.getenv('DB_NAME', 'riverguard_db')
    else:
        host = os.getenv('DB_HOST', 'localhost').strip()
        user = os.getenv('DB_USER', 'root').strip()
        password = os.getenv('DB_PASSWORD', '')
        database = os.getenv('DB_NAME', 'riverguard_db').strip() or 'riverguard_db'
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

    try:
        return pymysql.connect(**connect_kwargs)
    except pymysql.err.OperationalError as exc:
        if exc.args and exc.args[0] == 1049:
            created_db = _ensure_mysql_database_exists(connect_kwargs, database)
            connect_kwargs['database'] = created_db
            return pymysql.connect(**connect_kwargs)
        raise


def get_postgres_connection():
    """Connects to Cloud PostgreSQL (Neon, Supabase, or Render PostgreSQL)."""
    db_url = (os.getenv('DATABASE_URL') or os.getenv('DB_URL') or '').strip()
    force_ssl = os.getenv('DB_SSL', '').lower() in ('true', '1', 'yes')
    disable_ssl = os.getenv('DB_SSL', '').lower() in ('false', '0', 'no') or os.getenv('DB_SSL_DISABLED')

    if db_url:
        normalized_url = re.sub(r'^postgres(\+psycopg2)?://', 'postgresql://', db_url, flags=re.IGNORECASE)
        normalized_url = re.sub(r'^postgresql\+psycopg2://', 'postgresql://', normalized_url, flags=re.IGNORECASE)
        parsed = urlparse(normalized_url)
        host = parsed.hostname or 'localhost'
        is_remote = host not in ('localhost', '127.0.0.1', 'db')

        connect_kwargs = {
            'cursor_factory': psycopg2.extras.RealDictCursor,
            'connect_timeout': 10
        }
        if ((is_remote and not disable_ssl) or force_ssl) and 'sslmode=' not in normalized_url.lower():
            connect_kwargs['sslmode'] = 'require'

        return psycopg2.connect(normalized_url, **connect_kwargs)

    host = os.getenv('DB_HOST', 'localhost').strip()
    port = int(os.getenv('DB_PORT', 5432))
    user = os.getenv('DB_USER', 'postgres').strip()
    password = os.getenv('DB_PASSWORD', '')
    database = os.getenv('DB_NAME', 'riverguard_db').strip() or 'riverguard_db'
    is_remote = host not in ('localhost', '127.0.0.1', 'db')

    connect_kwargs = {
        'host': host,
        'port': port,
        'user': user,
        'password': password,
        'dbname': database,
        'cursor_factory': psycopg2.extras.RealDictCursor,
        'connect_timeout': 10
    }
    if (is_remote and not disable_ssl) or force_ssl:
        connect_kwargs['sslmode'] = 'require'

    return psycopg2.connect(**connect_kwargs)


def get_cloud_connection(backend):
    if backend == 'postgresql':
        return get_postgres_connection()
    return get_mysql_connection()


def get_sqlite_connection():
    """High-concurrency SQLite connection with WAL mode enabled."""
    conn = sqlite3.connect(db_path, timeout=15.0, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute("PRAGMA foreign_keys=ON;")
    return conn


# =========================================================
# 1. INITIALIZE DATABASE (PERSISTENT CLOUD MYSQL / POSTGRES + SQLITE FALLBACK)
# =========================================================
def init_db():
    global _active_db_backend, _active_env_sig, _db_init_error
    _active_env_sig = _current_db_env_sig()
    _db_init_error = None

    default_admin_user = os.getenv('ADMIN_USERNAME', 'admin')
    default_admin_email = os.getenv('ADMIN_EMAIL', 'admin@riverguard.com')
    default_admin_pass = os.getenv('ADMIN_PASSWORD', 'riverguard2026')
    password_hash = generate_password_hash(default_admin_pass)

    configured = detect_configured_backend()

    if configured == 'postgresql':
        if not HAS_PSYCOPG2:
            _db_init_error = "psycopg2-binary not installed; falling back to SQLite"
            print(f"[WARN] {_db_init_error}")
        else:
            try:
                conn = get_postgres_connection()
                try:
                    with conn.cursor() as cursor:
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS admins (
                                id SERIAL PRIMARY KEY,
                                username VARCHAR(80) NOT NULL UNIQUE,
                                email VARCHAR(120) NOT NULL UNIQUE,
                                password_hash VARCHAR(255) NOT NULL,
                                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                            )
                        ''')
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS registered_numbers (
                                id SERIAL PRIMARY KEY,
                                mobile_number VARCHAR(20) NOT NULL UNIQUE,
                                full_name VARCHAR(120) DEFAULT '',
                                location VARCHAR(160) DEFAULT '',
                                additional_details VARCHAR(255) DEFAULT '',
                                added_by_admin_id INTEGER DEFAULT NULL
                                    REFERENCES admins(id) ON DELETE SET NULL ON UPDATE CASCADE,
                                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                            )
                        ''')
                        cursor.execute(
                            "ALTER TABLE registered_numbers ADD COLUMN IF NOT EXISTS added_by_admin_id INTEGER DEFAULT NULL"
                        )
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS node_states (
                                node_id INTEGER PRIMARY KEY,
                                distance INTEGER NOT NULL DEFAULT 999,
                                battery INTEGER NOT NULL DEFAULT 100,
                                rssi INTEGER DEFAULT NULL,
                                status VARCHAR(16) NOT NULL DEFAULT 'ONLINE',
                                updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                            )
                        ''')
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS sensor_logs (
                                id SERIAL PRIMARY KEY,
                                node_id INTEGER DEFAULT NULL
                                    REFERENCES node_states(node_id) ON DELETE SET NULL ON UPDATE CASCADE,
                                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                                global_mode INTEGER NOT NULL DEFAULT 1,
                                node1_distance INTEGER NOT NULL DEFAULT 999,
                                nodes_json TEXT DEFAULT NULL
                            )
                        ''')
                        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN IF NOT EXISTS nodes_json TEXT DEFAULT NULL")
                        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN IF NOT EXISTS node_id INTEGER DEFAULT NULL")

                        cursor.execute("SELECT COUNT(*) AS cnt FROM admins")
                        if int(cursor.fetchone()['cnt']) == 0:
                            cursor.execute('''
                                INSERT INTO admins (username, email, password_hash)
                                VALUES (%s, %s, %s)
                            ''', (default_admin_user, default_admin_email, password_hash))
                    conn.commit()
                finally:
                    conn.close()
                _active_db_backend = 'postgresql'
                return
            except Exception as e:
                _db_init_error = f"{type(e).__name__}: Cloud PostgreSQL connection failed"
                print(f"[WARN] Cloud PostgreSQL init failed ({e}), falling back to SQLite WAL.")

    elif configured == 'mysql':
        if not HAS_PYMYSQL:
            _db_init_error = "PyMySQL not installed; falling back to SQLite"
            print(f"[WARN] {_db_init_error}")
        else:
            try:
                conn = get_mysql_connection()
                try:
                    with conn.cursor() as cursor:
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS admins (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                username VARCHAR(80) NOT NULL UNIQUE,
                                email VARCHAR(120) NOT NULL UNIQUE,
                                password_hash VARCHAR(255) NOT NULL,
                                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
                            )
                        ''')
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS registered_numbers (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                mobile_number VARCHAR(20) NOT NULL UNIQUE,
                                full_name VARCHAR(120) DEFAULT '',
                                location VARCHAR(160) DEFAULT '',
                                additional_details VARCHAR(255) DEFAULT '',
                                added_by_admin_id INT DEFAULT NULL,
                                created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                                CONSTRAINT fk_registered_numbers_admin
                                    FOREIGN KEY (added_by_admin_id) REFERENCES admins(id)
                                    ON DELETE SET NULL ON UPDATE CASCADE
                            )
                        ''')
                        cursor.execute("SHOW COLUMNS FROM registered_numbers LIKE 'added_by_admin_id'")
                        if not cursor.fetchone():
                            cursor.execute("ALTER TABLE registered_numbers ADD COLUMN added_by_admin_id INT DEFAULT NULL")

                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS node_states (
                                node_id INT PRIMARY KEY,
                                distance INT NOT NULL DEFAULT 999,
                                battery INT NOT NULL DEFAULT 100,
                                rssi INT DEFAULT NULL,
                                status VARCHAR(16) NOT NULL DEFAULT 'ONLINE',
                                updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
                            )
                        ''')
                        cursor.execute('''
                            CREATE TABLE IF NOT EXISTS sensor_logs (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                node_id INT DEFAULT NULL,
                                timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                                global_mode INT NOT NULL DEFAULT 1,
                                node1_distance INT NOT NULL DEFAULT 999,
                                nodes_json TEXT DEFAULT NULL,
                                CONSTRAINT fk_sensor_logs_node
                                    FOREIGN KEY (node_id) REFERENCES node_states(node_id)
                                    ON DELETE SET NULL ON UPDATE CASCADE
                            )
                        ''')
                        cursor.execute("SHOW COLUMNS FROM sensor_logs LIKE 'nodes_json'")
                        if not cursor.fetchone():
                            cursor.execute("ALTER TABLE sensor_logs ADD COLUMN nodes_json TEXT DEFAULT NULL")
                        cursor.execute("SHOW COLUMNS FROM sensor_logs LIKE 'node_id'")
                        if not cursor.fetchone():
                            cursor.execute("ALTER TABLE sensor_logs ADD COLUMN node_id INT DEFAULT NULL")
                        cursor.execute("SELECT COUNT(*) AS cnt FROM admins")
                        if int(cursor.fetchone()['cnt']) == 0:
                            cursor.execute('''
                                INSERT INTO admins (username, email, password_hash)
                                VALUES (%s, %s, %s)
                            ''', (default_admin_user, default_admin_email, password_hash))
                    conn.commit()
                finally:
                    conn.close()
                _active_db_backend = 'mysql'
                return
            except Exception as e:
                _db_init_error = f"{type(e).__name__}: Cloud MySQL connection failed"
                print(f"[WARN] Cloud MySQL init failed ({e}), falling back to SQLite WAL.")

    conn = get_sqlite_connection()
    cursor = conn.cursor()

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS admins (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            email TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS registered_numbers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            mobile_number TEXT NOT NULL UNIQUE,
            full_name TEXT DEFAULT '',
            location TEXT DEFAULT '',
            additional_details TEXT DEFAULT '',
            added_by_admin_id INTEGER DEFAULT NULL REFERENCES admins(id) ON DELETE SET NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    reg_cols = {
        row['name'] for row in cursor.execute("PRAGMA table_info(registered_numbers)").fetchall()
    }
    if 'added_by_admin_id' not in reg_cols:
        cursor.execute("ALTER TABLE registered_numbers ADD COLUMN added_by_admin_id INTEGER DEFAULT NULL")

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS node_states (
            node_id INTEGER PRIMARY KEY,
            distance INTEGER NOT NULL DEFAULT 999,
            battery INTEGER NOT NULL DEFAULT 100,
            rssi INTEGER DEFAULT NULL,
            status TEXT NOT NULL DEFAULT 'ONLINE',
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')

    cursor.execute('''
        CREATE TABLE IF NOT EXISTS sensor_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id INTEGER DEFAULT NULL REFERENCES node_states(node_id) ON DELETE SET NULL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            global_mode INTEGER,
            node1_distance INTEGER,
            nodes_json TEXT DEFAULT NULL
        )
    ''')

    existing_cols = {
        row['name'] for row in cursor.execute("PRAGMA table_info(sensor_logs)").fetchall()
    }
    if 'nodes_json' not in existing_cols:
        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN nodes_json TEXT DEFAULT NULL")
    if 'node_id' not in existing_cols:
        cursor.execute("ALTER TABLE sensor_logs ADD COLUMN node_id INTEGER DEFAULT NULL")

    admin_count = cursor.execute("SELECT COUNT(*) FROM admins").fetchone()[0]
    if admin_count == 0:
        cursor.execute('''
            INSERT INTO admins (username, email, password_hash)
            VALUES (?, ?, ?)
        ''', (default_admin_user, default_admin_email, password_hash))

    conn.commit()
    conn.close()
    _active_db_backend = 'sqlite'


init_db()


def admin_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not session.get('admin_logged_in'):
            return jsonify({"error": "Unauthorized"}), 401
        return f(*args, **kwargs)
    return decorated


def compute_threat_mode(nodes_list):
    """
    Matches Main_Node_Fin03.ino updateGlobalThreatLevel():
    - Only considers ONLINE nodes
    - 1..179 cm   -> Mode 3 (CRITICAL)
    - 180..300 cm -> Mode 2 (WARNING)
    - > 300 cm    -> Mode 1 (SAFE)
    """
    highest = 1
    for n in nodes_list:
        if str(n.get('status', 'ONLINE')).upper() == 'OFFLINE':
            continue
        d = int(n.get('distance', 999))
        if 0 < d <= 179:
            return 3
        elif 180 <= d <= 300 and highest < 3:
            highest = 2
    return highest


def select_primary_node(all_nodes):
    """
    Selects the active online Soldier Node with the closest water reading (< 999 cm)
    so historical distance logs always reflect real Soldier Node water levels.
    """
    online_valid = [
        x for x in all_nodes
        if str(x.get('status', 'ONLINE')).upper() != 'OFFLINE' and 0 < int(x.get('distance', 999)) < 999
    ]
    if online_valid:
        return min(online_valid, key=lambda x: int(x['distance']))
    online_any = [
        x for x in all_nodes
        if str(x.get('status', 'ONLINE')).upper() != 'OFFLINE'
    ]
    return online_any[0] if online_any else all_nodes[0]


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
    Supports:
      1. Main_Node_Fin03 combined JSON:
         {"mode": 1, "nodes": [{"id": 1, "distance": 320, "battery": 95, "rssi": -68, "status": "ONLINE"}, ...]}
      2. Main_Node_Fin02 per-node form POST:
         mode=1&node_id=1&distance=320&battery=95&status=ONLINE
      3. Legacy form/JSON:
         mode=1&node1=320
    """
    try:
        payload = {}
        if request.is_json:
            payload = request.get_json(silent=True) or {}
        elif request.form:
            payload = request.form.to_dict()
        else:
            payload = request.get_json(force=True, silent=True) or {}

        incoming_nodes = []
        raw_mode = payload.get('mode', payload.get('threat'))

        if isinstance(payload.get('nodes'), list):
            for item in payload['nodes']:
                nid = int(item.get('id', item.get('node_id', 1)))
                if 1 <= nid <= 9:
                    incoming_nodes.append({
                        "id": nid,
                        "distance": int(float(item.get('distance', 999))),
                        "battery": int(float(item.get('battery', 100))),
                        "rssi": int(float(item['rssi'])) if item.get('rssi') is not None else None,
                        "status": str(item.get('status', 'ONLINE')).upper()
                    })
        elif 'node_id' in payload:
            nid = int(payload['node_id'])
            incoming_nodes.append({
                "id": nid,
                "distance": int(float(payload.get('distance', 999))),
                "battery": int(float(payload.get('battery', 100))),
                "rssi": int(float(payload['rssi'])) if payload.get('rssi') is not None else None,
                "status": str(payload.get('status', 'ONLINE')).upper()
            })
        elif 'node1' in payload or 'node_1' in payload or 'distance' in payload:
            d1 = int(float(payload.get('node1', payload.get('node_1', payload.get('distance', 999)))))
            incoming_nodes.append({
                "id": 1,
                "distance": d1,
                "battery": 100,
                "rssi": None,
                "status": "ONLINE"
            })
            if payload.get('node2') or payload.get('node_2'):
                d2 = int(float(payload.get('node2', payload.get('node_2', 999))))
                if d2 < 999:
                    incoming_nodes.append({
                        "id": 2,
                        "distance": d2,
                        "battery": 100,
                        "rssi": None,
                        "status": "ONLINE"
                    })
            if payload.get('node3') or payload.get('node_3'):
                d3 = int(float(payload.get('node3', payload.get('node_3', 999))))
                if d3 < 999:
                    incoming_nodes.append({
                        "id": 3,
                        "distance": d3,
                        "battery": 100,
                        "rssi": None,
                        "status": "ONLINE"
                    })

        if not incoming_nodes:
            return jsonify({"error": "No valid node telemetry provided"}), 400

        utc_now_str = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')
        backend = get_db_backend()

        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
            try:
                with conn.cursor() as cursor:
                    for n in incoming_nodes:
                        if backend == 'postgresql':
                            cursor.execute('''
                                INSERT INTO node_states (node_id, distance, battery, rssi, status, updated_at)
                                VALUES (%s, %s, %s, %s, %s, %s)
                                ON CONFLICT (node_id) DO UPDATE SET
                                    distance = EXCLUDED.distance,
                                    battery = EXCLUDED.battery,
                                    rssi = EXCLUDED.rssi,
                                    status = EXCLUDED.status,
                                    updated_at = EXCLUDED.updated_at
                            ''', (n['id'], n['distance'], n['battery'], n['rssi'], n['status'], utc_now_str))
                        else:
                            cursor.execute('''
                                INSERT INTO node_states (node_id, distance, battery, rssi, status, updated_at)
                                VALUES (%s, %s, %s, %s, %s, %s)
                                ON DUPLICATE KEY UPDATE
                                    distance = VALUES(distance),
                                    battery = VALUES(battery),
                                    rssi = VALUES(rssi),
                                    status = VALUES(status),
                                    updated_at = VALUES(updated_at)
                            ''', (n['id'], n['distance'], n['battery'], n['rssi'], n['status'], utc_now_str))

                    cursor.execute('''
                        SELECT node_id AS id, distance, battery, rssi, status
                        FROM node_states ORDER BY node_id ASC
                    ''')
                    all_nodes = [dict(r) for r in cursor.fetchall()]

                    system_mode = int(raw_mode) if raw_mode is not None else compute_threat_mode(all_nodes)
                    primary_node_obj = select_primary_node(all_nodes)
                    primary_dist = int(primary_node_obj['distance'])
                    primary_nid = int(primary_node_obj['id'])

                    cursor.execute('''
                        INSERT INTO sensor_logs (node_id, timestamp, global_mode, node1_distance, nodes_json)
                        VALUES (%s, %s, %s, %s, %s)
                    ''', (primary_nid, utc_now_str, system_mode, primary_dist, json.dumps(all_nodes)))
                conn.commit()
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                cursor = conn.cursor()
                for n in incoming_nodes:
                    cursor.execute('''
                        INSERT INTO node_states (node_id, distance, battery, rssi, status, updated_at)
                        VALUES (?, ?, ?, ?, ?, ?)
                        ON CONFLICT(node_id) DO UPDATE SET
                            distance = excluded.distance,
                            battery = excluded.battery,
                            rssi = excluded.rssi,
                            status = excluded.status,
                            updated_at = excluded.updated_at
                    ''', (n['id'], n['distance'], n['battery'], n['rssi'], n['status'], utc_now_str))

                rows = cursor.execute('''
                    SELECT node_id AS id, distance, battery, rssi, status
                    FROM node_states ORDER BY node_id ASC
                ''').fetchall()
                all_nodes = [dict(r) for r in rows]

                system_mode = int(raw_mode) if raw_mode is not None else compute_threat_mode(all_nodes)
                primary_node_obj = select_primary_node(all_nodes)
                primary_dist = int(primary_node_obj['distance'])
                primary_nid = int(primary_node_obj['id'])

                cursor.execute('''
                    INSERT INTO sensor_logs (node_id, timestamp, global_mode, node1_distance, nodes_json)
                    VALUES (?, ?, ?, ?, ?)
                ''', (primary_nid, utc_now_str, system_mode, primary_dist, json.dumps(all_nodes)))
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
    """Returns latest 20 real gateway snapshots (empty list if gateway hasn't sent data yet)."""
    try:
        now = time.time()
        with _cache_lock:
            if _telemetry_cache["rows"] is not None and (now - _telemetry_cache["updated_at"]) < CACHE_TTL_SECONDS:
                resp = jsonify(_telemetry_cache["rows"])
                resp.headers['Cache-Control'] = 'no-store'
                return resp, 200

        backend = get_db_backend()
        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        SELECT timestamp, global_mode, node1_distance, nodes_json
                        FROM sensor_logs
                        ORDER BY id DESC LIMIT 20
                    ''')
                    rows = [dict(r) for r in cursor.fetchall()]
            finally:
                conn.close()
        else:
            conn = get_sqlite_connection()
            try:
                cursor = conn.cursor()
                cursor.execute('''
                    SELECT timestamp, global_mode, node1_distance, nodes_json
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

            parsed_nodes = []
            if r.get('nodes_json'):
                try:
                    parsed_nodes = json.loads(r['nodes_json'])
                except Exception:
                    parsed_nodes = []

            if not parsed_nodes:
                parsed_nodes = [{"id": 1, "distance": n1, "battery": 100, "rssi": None, "status": "ONLINE"}]

            data.append({
                "time": ts_str,
                "mode": int(r['global_mode']),
                "distance": n1,
                "nodes": parsed_nodes
            })

        with _cache_lock:
            _telemetry_cache["rows"] = data
            _telemetry_cache["updated_at"] = now

        resp = jsonify(data)
        resp.headers['Cache-Control'] = 'no-store'
        return resp, 200

    except Exception as e:
        return jsonify({"error": str(e)}), 500


# =========================================================
# 4. SMS GATEWAY FETCH NUMBERS ROUTE (/api/get_numbers) — API KEY PROTECTED
# =========================================================
@app.route('/api/get_numbers', methods=['GET'])
@gateway_key_required
def get_numbers():
    """
    Returns active registered mobile numbers for Main_Node_Fin03 / Main_Node_Fin04 SIM800L (protected by X-API-Key).
    Plain-text comma-separated: +639171111111,+639182222222 (or empty string if none).
    """
    try:
        backend = get_db_backend()
        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
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
            resp = jsonify({"numbers": nums, "count": len(nums)})
            resp.headers['Cache-Control'] = 'no-store'
            return resp, 200

        return Response(",".join(nums), mimetype='text/plain', status=200, headers={'Cache-Control': 'no-store'})
    except Exception as e:
        if request.args.get('format', '').lower() == 'json':
            return jsonify({"error": str(e), "numbers": []}), 500
        return Response("", mimetype='text/plain', status=200, headers={'Cache-Control': 'no-store'})


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
        admin_id_fk = session.get('admin_id') if session.get('admin_logged_in') else None
        backend = get_db_backend()

        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
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
                        (mobile_number, full_name, location, additional_details, added_by_admin_id, created_at)
                        VALUES (%s, %s, %s, %s, %s, %s)
                    ''', (normalized_phone, full_name, location, additional_details, admin_id_fk, utc_now_str))
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
                    (mobile_number, full_name, location, additional_details, added_by_admin_id, created_at)
                    VALUES (?, ?, ?, ?, ?, ?)
                ''', (normalized_phone, full_name, location, additional_details, admin_id_fk, utc_now_str))
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
        backend = get_db_backend()
        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
            try:
                with conn.cursor() as cursor:
                    cursor.execute(
                        "SELECT id, username, email, password_hash FROM admins WHERE username = %s OR email = %s",
                        (identifier, identifier)
                    )
                    row = cursor.fetchone()
                    admin_row = dict(row) if row else None
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
        backend = get_db_backend()
        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
            try:
                with conn.cursor() as cursor:
                    cursor.execute('''
                        SELECT id, mobile_number, full_name, location, additional_details, created_at
                        FROM registered_numbers
                        ORDER BY id DESC
                    ''')
                    rows = [dict(r) for r in cursor.fetchall()]
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
        backend = get_db_backend()
        if backend in ('mysql', 'postgresql'):
            conn = get_cloud_connection(backend)
            try:
                with conn.cursor() as cursor:
                    cursor.execute(
                        "DELETE FROM registered_numbers WHERE id = %s",
                        (record_id,)
                    )
                    deleted = cursor.rowcount
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


# =========================================================
# 9. DATABASE & SERVICE HEALTH CHECK (/api/health)
# =========================================================
@app.route('/api/health', methods=['GET'])
def health_check():
    backend = get_db_backend()
    configured = detect_configured_backend()
    payload = {
        "status": "ok",
        "db_backend": backend,
        "configured_backend": configured,
        "persistent": backend in ('mysql', 'postgresql')
    }
    if _db_init_error:
        payload["db_warning"] = _db_init_error
    resp = jsonify(payload)
    resp.headers['Cache-Control'] = 'no-store'
    return resp, 200


if __name__ == '__main__':
    port = int(os.getenv('PORT', 5000))
    app.run(host='0.0.0.0', port=port, debug=True)
