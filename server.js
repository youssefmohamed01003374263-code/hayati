/**
 * Smart Irrigation System - Backend Server
 *
 * Features:
 *   - Subscribes to MQTT topics from ESP32
 *   - Stores sensor data in SQLite database
 *   - REST API for historical data and system control
 *   - WebSocket server for real-time dashboard updates
 *   - Publishes commands back to ESP32 via MQTT
 */

require('dotenv').config();
const express = require('express');
const cors = require('cors');
const http = require('http');
const { WebSocketServer } = require('ws');
const mqtt = require('mqtt');
const path = require('path');
const fs = require('fs');
const Database = require('better-sqlite3');

// ==================== CONFIGURATION ====================
const config = {
  port: process.env.PORT || 3000,
  host: process.env.HOST || '0.0.0.0',
  mqttUrl: process.env.MQTT_BROKER_URL || 'mqtt://localhost:1883',
  mqttUser: process.env.MQTT_USERNAME || '',
  mqttPass: process.env.MQTT_PASSWORD || '',
  dbPath: process.env.DB_PATH || './data/irrigation.db',
};

// ==================== DATABASE SETUP ====================
const dataDir = path.dirname(config.dbPath);
if (!fs.existsSync(dataDir)) {
  fs.mkdirSync(dataDir, { recursive: true });
}

const db = new Database(config.dbPath);
db.pragma('journal_mode = WAL');

// Create tables
db.exec(`
  CREATE TABLE IF NOT EXISTS sensor_readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    temperature REAL,
    humidity REAL,
    soil_moisture REAL,
    light_level INTEGER,
    valve_open INTEGER,
    timestamp DATETIME DEFAULT (datetime('now'))
  );

  CREATE TABLE IF NOT EXISTS valve_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    action TEXT NOT NULL,
    source TEXT DEFAULT 'auto',
    timestamp DATETIME DEFAULT (datetime('now'))
  );

  CREATE TABLE IF NOT EXISTS system_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    message TEXT NOT NULL,
    timestamp DATETIME DEFAULT (datetime('now'))
  );

  CREATE INDEX IF NOT EXISTS idx_readings_timestamp ON sensor_readings(timestamp);
  CREATE INDEX IF NOT EXISTS idx_readings_device ON sensor_readings(device_id);
  CREATE INDEX IF NOT EXISTS idx_valve_timestamp ON valve_events(timestamp);
  CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON system_logs(timestamp);
`);

// Prepared statements for performance
const insertReading = db.prepare(`
  INSERT INTO sensor_readings (device_id, temperature, humidity, soil_moisture, light_level, valve_open)
  VALUES (?, ?, ?, ?, ?, ?)
`);

const insertValveEvent = db.prepare(`
  INSERT INTO valve_events (device_id, action, source)
  VALUES (?, ?, ?)
`);

const insertLog = db.prepare(`
  INSERT INTO system_logs (device_id, message)
  VALUES (?, ?)
`);

console.log('[DB] SQLite database initialized.');

// ==================== EXPRESS APP ====================
const app = express();
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../dashboard')));

const server = http.createServer(app);

// ==================== WEBSOCKET SERVER ====================
const wss = new WebSocketServer({ server });
const wsClients = new Set();

wss.on('connection', (ws) => {
  console.log('[WS] Client connected');
  wsClients.add(ws);

  // Send current state on connect
  if (latestSensorData) {
    ws.send(JSON.stringify({ type: 'sensor_data', data: latestSensorData }));
  }
  if (latestValveStatus) {
    ws.send(JSON.stringify({ type: 'valve_status', data: latestValveStatus }));
  }

  ws.on('close', () => {
    wsClients.delete(ws);
    console.log('[WS] Client disconnected');
  });
});

function broadcastToClients(type, data) {
  const message = JSON.stringify({ type, data });
  for (const client of wsClients) {
    if (client.readyState === 1) { // OPEN
      client.send(message);
    }
  }
}

// ==================== MQTT CLIENT ====================
let latestSensorData = null;
let latestValveStatus = null;

const mqttOptions = {
  clientId: 'irrigation_server_' + Math.random().toString(16).substring(2, 8),
  clean: true,
  reconnectPeriod: 5000,
};
if (config.mqttUser) {
  mqttOptions.username = config.mqttUser;
  mqttOptions.password = config.mqttPass;
}

const mqttClient = mqtt.connect(config.mqttUrl, mqttOptions);

mqttClient.on('connect', () => {
  console.log('[MQTT] Connected to broker at', config.mqttUrl);
  mqttClient.subscribe([
    'irrigation/sensors',
    'irrigation/valve/status',
    'irrigation/system/status',
    'irrigation/log',
  ]);
});

mqttClient.on('message', (topic, message) => {
  try {
    const data = JSON.parse(message.toString());

    switch (topic) {
      case 'irrigation/sensors':
        latestSensorData = { ...data, server_timestamp: new Date().toISOString() };
        // Store in database (throttle to every 30 seconds)
        storeSensorData(data);
        // Broadcast to web clients
        broadcastToClients('sensor_data', latestSensorData);
        break;

      case 'irrigation/valve/status':
        latestValveStatus = { ...data, server_timestamp: new Date().toISOString() };
        broadcastToClients('valve_status', latestValveStatus);
        break;

      case 'irrigation/system/status':
        broadcastToClients('system_status', data);
        console.log(`[MQTT] Device ${data.device_id} is ${data.status} (IP: ${data.ip})`);
        break;

      case 'irrigation/log':
        insertLog.run(data.device_id || 'unknown', data.message || '');
        broadcastToClients('log', { ...data, server_timestamp: new Date().toISOString() });
        console.log(`[LOG] ${data.device_id}: ${data.message}`);
        break;
    }
  } catch (err) {
    console.error('[MQTT] Error processing message:', err.message);
  }
});

mqttClient.on('error', (err) => {
  console.error('[MQTT] Connection error:', err.message);
});

// Throttle sensor storage to avoid flooding the database
let lastStoredTime = 0;
const STORAGE_INTERVAL = 30000; // 30 seconds

function storeSensorData(data) {
  const now = Date.now();
  if (now - lastStoredTime >= STORAGE_INTERVAL) {
    lastStoredTime = now;
    insertReading.run(
      data.device_id || 'unknown',
      data.temperature,
      data.humidity,
      data.soil_moisture,
      data.light_level,
      data.valve_open ? 1 : 0
    );
  }
}

// ==================== REST API ====================

// GET /api/current — Latest sensor readings
app.get('/api/current', (req, res) => {
  res.json({
    success: true,
    data: latestSensorData,
    valve: latestValveStatus,
  });
});

// GET /api/history — Historical sensor data
app.get('/api/history', (req, res) => {
  const hours = parseInt(req.query.hours) || 24;
  const limit = Math.min(parseInt(req.query.limit) || 1000, 5000);

  const rows = db.prepare(`
    SELECT * FROM sensor_readings
    WHERE timestamp >= datetime('now', ?)
    ORDER BY timestamp DESC
    LIMIT ?
  `).all(`-${hours} hours`, limit);

  res.json({ success: true, count: rows.length, data: rows });
});

// GET /api/history/hourly — Aggregated hourly averages
app.get('/api/history/hourly', (req, res) => {
  const hours = parseInt(req.query.hours) || 48;

  const rows = db.prepare(`
    SELECT
      strftime('%Y-%m-%d %H:00', timestamp) as hour,
      ROUND(AVG(temperature), 1) as avg_temperature,
      ROUND(AVG(humidity), 1) as avg_humidity,
      ROUND(AVG(soil_moisture), 1) as avg_soil_moisture,
      ROUND(AVG(light_level), 0) as avg_light_level,
      SUM(CASE WHEN valve_open = 1 THEN 1 ELSE 0 END) as valve_open_readings,
      COUNT(*) as total_readings
    FROM sensor_readings
    WHERE timestamp >= datetime('now', ?)
    GROUP BY hour
    ORDER BY hour ASC
  `).all(`-${hours} hours`);

  res.json({ success: true, count: rows.length, data: rows });
});

// GET /api/valve/events — Valve activity log
app.get('/api/valve/events', (req, res) => {
  const limit = Math.min(parseInt(req.query.limit) || 50, 200);

  const rows = db.prepare(`
    SELECT * FROM valve_events
    ORDER BY timestamp DESC
    LIMIT ?
  `).all(limit);

  res.json({ success: true, count: rows.length, data: rows });
});

// GET /api/logs — System logs
app.get('/api/logs', (req, res) => {
  const limit = Math.min(parseInt(req.query.limit) || 100, 500);

  const rows = db.prepare(`
    SELECT * FROM system_logs
    ORDER BY timestamp DESC
    LIMIT ?
  `).all(limit);

  res.json({ success: true, count: rows.length, data: rows });
});

// POST /api/valve/command — Send valve command to ESP32
app.post('/api/valve/command', (req, res) => {
  const { command } = req.body;
  const validCommands = ['open', 'close', 'auto', 'enable', 'disable'];

  if (!validCommands.includes(command)) {
    return res.status(400).json({
      success: false,
      error: `Invalid command. Valid commands: ${validCommands.join(', ')}`,
    });
  }

  const payload = JSON.stringify({ command });
  mqttClient.publish('irrigation/valve/command', payload);

  // Log the event
  insertValveEvent.run('server', command, 'manual');
  insertLog.run('server', `Manual command sent: ${command}`);

  console.log(`[API] Valve command sent: ${command}`);
  res.json({ success: true, command });
});

// POST /api/thresholds — Update irrigation thresholds
app.post('/api/thresholds', (req, res) => {
  const thresholds = req.body;
  const payload = JSON.stringify(thresholds);
  mqttClient.publish('irrigation/thresholds', payload, { retain: true });

  insertLog.run('server', `Thresholds updated: ${payload}`);
  console.log('[API] Thresholds updated:', thresholds);
  res.json({ success: true, thresholds });
});

// GET /api/stats — Dashboard statistics
app.get('/api/stats', (req, res) => {
  const today = db.prepare(`
    SELECT
      COUNT(*) as readings_today,
      ROUND(AVG(temperature), 1) as avg_temp,
      ROUND(AVG(humidity), 1) as avg_humidity,
      ROUND(AVG(soil_moisture), 1) as avg_soil_moisture,
      MIN(soil_moisture) as min_soil_moisture,
      MAX(soil_moisture) as max_soil_moisture
    FROM sensor_readings
    WHERE timestamp >= datetime('now', '-24 hours')
  `).get();

  const wateringEvents = db.prepare(`
    SELECT COUNT(*) as count FROM valve_events
    WHERE timestamp >= datetime('now', '-24 hours')
    AND action = 'open'
  `).get();

  res.json({
    success: true,
    data: {
      ...today,
      watering_events_today: wateringEvents.count,
    },
  });
});

// ==================== START SERVER ====================
server.listen(config.port, config.host, () => {
  console.log(`\n========================================`);
  console.log(`  Smart Irrigation Server`);
  console.log(`  http://${config.host}:${config.port}`);
  console.log(`  MQTT: ${config.mqttUrl}`);
  console.log(`========================================\n`);
});
