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

## Key Constants

All tuneable constants live in `src/config.h` (not `main.c`). Override at build time:
```bash
cmake -B build -DCONDUCTOR_PIN=9876 -DOWNER_PIN=5432 -DHTTP_PORT=8443
```

Key values: `BROKER_HOST/PORT`, `HTTP_PORT=8080`, `MAX_WS_CLIENTS=32`, `MAX_WS_MSG_BYTES=8192` (oversized frames are rejected), `MAX_LOG_MSGS=100`, `PIN_MAX_ATTEMPTS=5`, `PIN_LOCKOUT_SEC=30`

## Security Architecture

- **Auth module** (`src/auth.c/h`): per-WebSocket-connection state (`ws_auth_t`), stored as libwebsockets per-session data. PINs validated server-side with constant-time comparison. 5 failures → 30s lockout. Client-side PIN check **removed** — all auth is server-enforced.
- **Role hierarchy**: `OWNER > CONDUCTOR > DRIVER = PASSENGER`. `REQUIRE_ROLE()` macro guards privileged actions. Passenger/Driver need no PIN; Conductor/Owner require PIN.
- **Input validation**: `VALIDATE_SEAT_ID()` and `VALIDATE_PHONE()` macros check all WS command parameters before processing. Oversized WS frames (>8 KB) close the connection.
- **HTTP security headers**: `X-Frame-Options: DENY`, `X-Content-Type-Options`, `CSP`, `Permissions-Policy` appended to all file responses via `g_sec_headers`.
- **MQTT TLS**: set `MQTT_CA_CERT=/path/to/ca.crt` env var to enable TLS on port 8883. Mutual TLS via `MQTT_CLIENT_CERT`/`MQTT_CLIENT_KEY`.
- **Path traversal**: URI checked for `..`, `//`, `\r`, `\n` before file serving.
- **MQTT topic ACL**: subscribe action only allows `/topic/transittag/` prefix.

## MQTT Topic Structure

```
/topic/transittag/wifi/<imei>        — WiFi AP status + connected clients
/topic/transittag/heartbeat/<imei>   — GPS, battery, GSM, speed, movement
/topic/transittag/rfid/<imei>        — RFID card scans
/topic/transittag/login/<imei>       — Device online/offline events
```

## Dependencies

System libraries (apt): `libmosquitto-dev`, `libwebsockets-dev`, `libcurl4-openssl-dev` (optional, for InfluxDB + Google Maps)
Vendored: `cJSON` (`src/cJSON.c`), `qrcodegen` (`src/qrcodegen.c`) — MIT licensed
