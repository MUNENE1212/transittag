/*
 * config.h — TransitTag compile-time configuration
 *
 * All tuneable constants live here so they can be overridden at build time
 * (e.g.  cmake -DHTTP_PORT=8443) without touching individual source files.
 * This also makes it easy to strip or obfuscate business-logic parameters
 * in commercial deployments.
 */

#ifndef TRANSITTAG_CONFIG_H
#define TRANSITTAG_CONFIG_H

/* ── Network ──────────────────────────────────────────────────────── */

#ifndef BROKER_HOST
#  define BROKER_HOST        "byte-iot.net"
#endif

#ifndef BROKER_PORT
#  define BROKER_PORT        1883          /* Plain MQTT; use 8883 for TLS */
#endif

#ifndef BROKER_PORT_TLS
#  define BROKER_PORT_TLS    8883
#endif

#ifndef HTTP_PORT
#  define HTTP_PORT          8080
#endif

#define KEEP_ALIVE           60            /* MQTT keep-alive seconds */
#define CLIENT_ID_PREFIX     "tt_dash_"

/* ── WebSocket limits ─────────────────────────────────────────────── */

#define MAX_WS_CLIENTS       32
#define MAX_WS_MSG_BYTES     8192          /* Reject oversized WS frames */

/* ── MQTT ─────────────────────────────────────────────────────────── */

#define MAX_SUBSCRIPTIONS    16
#define MAX_TOPIC_LEN        256
#define DEFAULT_TOPIC        "/topic/transittag/#"

/* ── Message log ring buffer ──────────────────────────────────────── */

#define MAX_LOG_MSGS         100

/* ── Seats ────────────────────────────────────────────────────────── */

#define DEFAULT_SEAT_COUNT   14

/* ── Pricing ──────────────────────────────────────────────────────── */

#define DEFAULT_BASE_FARE_KES  50
#define DEFAULT_ROUTE          "CBD → Westlands"
#define PEAK_MULTIPLIER        1.5f
#define PEAK1_START_HOUR       7
#define PEAK1_END_HOUR         9
#define PEAK2_START_HOUR       17
#define PEAK2_END_HOUR         19

/* Distance → base fare tiers (metres → KES) */
#define FARE_TIER_0_DIST_M     2000
#define FARE_TIER_0_KES        30
#define FARE_TIER_1_DIST_M     5000
#define FARE_TIER_1_KES        50
#define FARE_TIER_2_DIST_M     10000
#define FARE_TIER_2_KES        70
#define FARE_TIER_3_DIST_M     20000
#define FARE_TIER_3_KES        100
#define FARE_TIER_MAX_KES      130

/* ── Security / Auth ──────────────────────────────────────────────── */

/* Server-side PINs — override at build time: cmake -DCONDUCTOR_PIN=9999 */
#ifndef CONDUCTOR_PIN
#  define CONDUCTOR_PIN      "1234"
#endif

#ifndef OWNER_PIN
#  define OWNER_PIN          "5678"
#endif

/* Maximum failed PIN attempts before connection is locked for LOCKOUT_SEC */
#define PIN_MAX_ATTEMPTS     5
#define PIN_LOCKOUT_SEC      30

/* Minimum seconds between PIN attempts (anti-brute-force) */
#define PIN_ATTEMPT_INTERVAL_SEC  2

/* Payment timeout before OVERDUE */
#define PAYMENT_TIMEOUT_SEC  90

/* ── Paths ────────────────────────────────────────────────────────── */

#ifndef WWW_DIR
#  define WWW_DIR            "./www"
#endif

#define STOPS_FILE           WWW_DIR "/stops.json"
#define TMP_DIR              "/tmp"

/* ── HTTP security headers (sent with every file response) ────────── */
#define HTTP_SECURITY_HEADERS \
    "X-Frame-Options: DENY\r\n" \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-XSS-Protection: 1; mode=block\r\n" \
    "Referrer-Policy: no-referrer\r\n" \
    "Content-Security-Policy: default-src 'self'; " \
        "style-src 'self' 'unsafe-inline'; " \
        "img-src 'self' data:; " \
        "connect-src 'self' ws: wss:;\r\n" \
    "Permissions-Policy: geolocation=(), camera=(), microphone=()\r\n"

#endif /* TRANSITTAG_CONFIG_H */
