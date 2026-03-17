# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TransitTag IoT Dashboard — a C server that bridges MQTT messages from a TransitTag IoT device (via `byte-iot.net:1883`) to a browser dashboard over WebSocket. Optionally writes time-series data to InfluxDB for Grafana visualization.

## Build & Run

```bash
# Build (requires libmosquitto-dev, libwebsockets-dev, cJSON is vendored)
cmake -B build && cmake --build build

# Run (MQTT broker credentials required)
./build/mqtt_dashboard <username> <password>

# Dashboard available at http://localhost:8080

# Optional: enable InfluxDB (requires libcurl4-openssl-dev at build time)
export INFLUX_URL=http://localhost:8086
export INFLUX_TOKEN=transittag-dev-token
export INFLUX_ORG=transittag
export INFLUX_BUCKET=iot_data

# Start InfluxDB + Grafana (Grafana at :3000)
docker-compose up -d
```

## Architecture

The server (`src/main.c`) runs a single-threaded event loop interleaving two libraries:
- **libmosquitto** — subscribes to MQTT topics on the broker, receives device JSON
- **libwebsockets** — serves static files from `www/` on `:8080` and pushes MQTT messages to browsers via WebSocket at `/ws`

Message flow: Device → MQTT broker → C server wraps in `{topic, payload, receivedAt}` envelope → broadcasts to all WebSocket clients → browser JS (`www/app.js`) routes by topic type (heartbeat, wifi, rfid, login) to update DOM panels.

The browser can also send commands back (subscribe/unsubscribe to MQTT topics) via WebSocket JSON messages with an `action` field.

**InfluxDB integration** is compile-time optional. CMake detects libcurl: if found, `src/influxdb.c` is compiled; otherwise `src/influxdb_stub.c` provides no-op stubs. InfluxDB is configured at runtime via environment variables, not command-line args.

## Key Constants (in main.c)

- `BROKER_HOST` / `BROKER_PORT` — MQTT broker address (`byte-iot.net:1883`)
- `HTTP_PORT` — dashboard port (`8080`)
- `MAX_WS_CLIENTS` — 32 concurrent browser connections
- `MAX_LOG_MSGS` — 100-message ring buffer for backlog replay on new WS connections
- `MAX_SUBSCRIPTIONS` — 16 active MQTT subscriptions
- Default subscription: `/topic/transittag/#`

## MQTT Topic Structure

```
/topic/transittag/wifi/<imei>        — WiFi AP status + connected clients
/topic/transittag/heartbeat/<imei>   — GPS, battery, GSM, speed, movement
/topic/transittag/rfid/<imei>        — RFID card scans
/topic/transittag/login/<imei>       — Device online/offline events
```

## Dependencies

System libraries (apt): `libmosquitto-dev`, `libwebsockets-dev`, `libcurl4-openssl-dev` (optional, for InfluxDB)
Vendored: `cJSON` (`src/cJSON.c` / `src/cJSON.h`)
