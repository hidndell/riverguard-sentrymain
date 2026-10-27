-- ============================================================================
-- RiverGuard Sentry System — Relational Database Schema (MySQL 8.0+ / TiDB)
-- Can be imported directly into MySQL Workbench, phpMyAdmin, draw.io, or dbdiagram.io
-- ============================================================================

CREATE TABLE IF NOT EXISTS admins (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(80) NOT NULL UNIQUE,
    email VARCHAR(120) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

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
        ON DELETE SET NULL
        ON UPDATE CASCADE
);

CREATE TABLE IF NOT EXISTS node_states (
    node_id INT PRIMARY KEY,
    distance INT NOT NULL DEFAULT 999,
    battery INT NOT NULL DEFAULT 100,
    rssi INT DEFAULT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'ONLINE',
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS sensor_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    node_id INT DEFAULT NULL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    global_mode INT NOT NULL DEFAULT 1,
    node1_distance INT NOT NULL DEFAULT 999,
    nodes_json TEXT DEFAULT NULL,
    CONSTRAINT fk_sensor_logs_node
        FOREIGN KEY (node_id) REFERENCES node_states(node_id)
        ON DELETE SET NULL
        ON UPDATE CASCADE
);

