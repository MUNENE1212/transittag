# TransitTag IoT Dashboard

Real-time IoT dashboard that bridges MQTT messages from TransitTag devices to a browser via WebSocket, with optional InfluxDB + Grafana time-series visualization.

```
TransitTag Device  →  MQTT Broker  →  C Server (bridge + HTTP)  →  Browser Dashboard
                                           ↓
                                      InfluxDB  →  Grafana
```

## Features

- **Live dashboard** — device status, GPS, WiFi clients, RFID scans, message log
- **MQTT → WebSocket bridge** — single C binary, no runtime dependencies
- **Dynamic subscriptions** — subscribe/unsubscribe to MQTT topics from the browser
- **Time-series storage** — optional InfluxDB integration for historical data
- **Grafana dashboards** — pre-provisioned dashboards for device metrics

## Prerequisites

```bash
# Ubuntu/Debian
sudo apt install libmosquitto-dev libwebsockets-dev cmake build-essential

# Optional: for InfluxDB support
sudo apt install libcurl4-openssl-dev

# Optional: for Grafana visualization
docker and docker compose
```

## Quick Start

```bash
# Build
cmake -B build && cmake --build build

# Configure
cp .env.example .env
# Edit .env with your credentials

# Run dashboard
./build/mqtt_dashboard <mqtt_username> <mqtt_password>

# Open http://localhost:8080
```

## InfluxDB + Grafana (optional)

```bash
# Source your .env for the dashboard process
export $(grep -v '^#' .env | xargs)

# Start InfluxDB and Grafana
docker compose up -d

# Run dashboard with InfluxDB enabled
./build/mqtt_dashboard <mqtt_username> <mqtt_password>

# Grafana: http://localhost:3000
# InfluxDB: http://localhost:8086
```

## Architecture

The server (`src/main.c`) runs a single-threaded event loop combining:

- **libmosquitto** — MQTT client subscribing to the broker
- **libwebsockets** — HTTP file server (`www/`) + WebSocket push to browsers
- **libcurl** (optional) — writes measurements to InfluxDB v2 line protocol

The browser client (`www/app.js`) connects via WebSocket to `/ws`, receives MQTT messages wrapped in `{topic, payload, receivedAt}` envelopes, and routes them by topic type (heartbeat, wifi, rfid, login) to update the dashboard panels.

## MQTT Topics

```
/topic/transittag/heartbeat/<imei>   — GPS, battery, GSM, speed, movement
/topic/transittag/wifi/<imei>        — WiFi AP status + connected clients
/topic/transittag/rfid/<imei>        — RFID card scans
/topic/transittag/login/<imei>       — Device online/offline events
```

## License

MIT
