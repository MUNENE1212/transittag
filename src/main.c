/*
 * mqtt_dashboard — Unified MQTT client + HTTP/WebSocket server
 *
 * Receives JSON from byte-iot.net via MQTT, serves a web dashboard
 * on :8080, and pushes live updates to browsers over WebSocket.
 * Optionally writes time-series data to InfluxDB for Grafana.
 *
 * Dependencies: libmosquitto, libwebsockets, cJSON (vendored), libcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <mosquitto.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "influxdb.h"
#include "seat_manager.h"
#include "pricing.h"
#include "routes.h"
#include "qr_svg.h"
#include "auth.h"
#include "config.h"

/* ── Globals ──────────────────────────────────────────────────── */

static volatile int running = 1;
static struct lws_context *g_lws_ctx = NULL;
static struct mosquitto *g_mosq = NULL;

/* Active MQTT subscriptions */
static char active_subs[MAX_SUBSCRIPTIONS][MAX_TOPIC_LEN];
static int sub_count = 0;

/* Ring of recent MQTT messages */
typedef struct {
    char *json;
    size_t len;
} log_entry_t;

static log_entry_t msg_log[MAX_LOG_MSGS];
static int log_head = 0;
static int log_count = 0;

/* WebSocket client tracking */
static struct lws *ws_clients[MAX_WS_CLIENTS];
static int ws_client_count = 0;

/* Pending broadcast */
static char *pending_broadcast = NULL;
static size_t pending_broadcast_len = 0;

/* Last known vehicle telemetry (cached from heartbeat) */
static double g_lat     = 0.0;
static double g_lon     = 0.0;
static double g_speed   = 0.0;
static int    g_battery = 0;
static int    g_gsm     = 0;

/* Security headers appended to every HTTP file response */
static const char g_sec_headers[] = HTTP_SECURITY_HEADERS;
static const int  g_sec_headers_len = sizeof(HTTP_SECURITY_HEADERS) - 1;

static void store_log_entry(const char *json, size_t len);
static void schedule_ws_broadcast(const char *json, size_t len);

/* ── Helpers ──────────────────────────────────────────────────── */

static void get_iso_timestamp(char *buf, size_t buflen)
{
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", tm);
}

static const char *get_topic_type(const char *topic)
{
    if (strstr(topic, "/heartbeat/")) return "heartbeat";
    if (strstr(topic, "/wifi/"))      return "wifi";
    if (strstr(topic, "/rfid/"))      return "rfid";
    if (strstr(topic, "/login/"))     return "login";
    if (strstr(topic, "/seat/"))      return "seat";
    return "unknown";
}

static double cjson_get_double(cJSON *obj, const char *key, double def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return item->valuedouble;
    return def;
}

static int cjson_get_int(cJSON *obj, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return (int)item->valuedouble;
    return def;
}

static const char *cjson_get_string(cJSON *obj, const char *key, const char *def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return def;
}

static int cjson_get_bool(cJSON *obj, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valuedouble != 0;
    return def;
}

/* ── MQTT callbacks ───────────────────────────────────────────── */

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc == 0) {
        printf("[MQTT] Connected to %s:%d\n", BROKER_HOST, BROKER_PORT);
        /* Resubscribe to all active topics */
        for (int i = 0; i < sub_count; i++) {
            mosquitto_subscribe(mosq, NULL, active_subs[i], 0);
            printf("[MQTT] Subscribed: %s\n", active_subs[i]);
        }
    } else {
        fprintf(stderr, "[MQTT] Connection failed: %s\n",
                mosquitto_connack_string(rc));
    }
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq; (void)userdata;
    printf("[MQTT] Disconnected (rc=%d)\n", rc);
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    (void)mosq; (void)userdata;
    if (!msg->payload || msg->payloadlen == 0) return;

    printf("[MQTT] %s (%d bytes)\n", msg->topic, msg->payloadlen);

    char ts[32];
    get_iso_timestamp(ts, sizeof(ts));
    char *payload_str = strndup((char *)msg->payload, msg->payloadlen);

    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "topic", msg->topic);
    cJSON_AddStringToObject(envelope, "payload", payload_str);
    cJSON_AddStringToObject(envelope, "receivedAt", ts);

    char *json_out = cJSON_PrintUnformatted(envelope);
    size_t json_len = strlen(json_out);

    store_log_entry(json_out, json_len);
    schedule_ws_broadcast(json_out, json_len);

    /* ── Write to InfluxDB ── */
    const char *ttype = get_topic_type(msg->topic);
    cJSON *payload_json = cJSON_Parse(payload_str);
    if (payload_json) {
        const char *imei = cjson_get_string(payload_json, "imei", NULL);

        if (strcmp(ttype, "heartbeat") == 0) {
            /* Cache latest vehicle telemetry for owner dashboard */
            g_lat     = cjson_get_double(payload_json, "latitude",  g_lat);
            g_lon     = cjson_get_double(payload_json, "longitude", g_lon);
            g_speed   = cjson_get_double(payload_json, "speed",     g_speed);
            g_battery = cjson_get_int(payload_json,    "battery",   g_battery);
            g_gsm     = cjson_get_int(payload_json,    "gsm",       g_gsm);

            influx_write_heartbeat(imei,
                cjson_get_double(payload_json, "battery", 0),
                cjson_get_int(payload_json, "gsm", 0),
                cjson_get_double(payload_json, "latitude", 0),
                cjson_get_double(payload_json, "longitude", 0),
                cjson_get_double(payload_json, "speed", 0),
                cjson_get_int(payload_json, "satelites", 0),
                cjson_get_string(payload_json, "acc", "OFF"),
                cjson_get_bool(payload_json, "move", 0),
                cjson_get_bool(payload_json, "alarm", 0));
        } else if (strcmp(ttype, "seat") == 0) {
            /*
             * Seat sensor update from the IoT device.
             * Expected payload: {"seats":[{"id":1,"weight":12.3},…]}
             * or a single seat: {"id":1,"weight":12.3}
             */
            cJSON *seats_arr = cJSON_GetObjectItem(payload_json, "seats");
            if (seats_arr && cJSON_IsArray(seats_arr)) {
                cJSON *item = NULL;
                cJSON_ArrayForEach(item, seats_arr) {
                    int    sid    = cjson_get_int(item, "id", 0);
                    double weight = cjson_get_double(item, "weight", 0.0);
                    if (sid > 0) {
                        seat_set_weight(sid, weight, pricing_get_fare());
                    }
                }
            } else {
                /* Single-seat payload */
                int    sid    = cjson_get_int(payload_json, "id", 0);
                double weight = cjson_get_double(payload_json, "weight", 0.0);
                if (sid > 0) {
                    seat_set_weight(sid, weight, pricing_get_fare());
                }
            }
            /* Broadcast updated seat state to all connected clients */
            {
                char *sj = seats_state_to_json(
                    pricing_get_fare(),
                    pricing_get_route(),
                    pricing_is_peak());
                if (sj) {
                    schedule_ws_broadcast(sj, strlen(sj));
                    free(sj);
                }
            }
        } else if (strcmp(ttype, "wifi") == 0) {
            cJSON *cfg = cJSON_GetObjectItem(payload_json, "config");
            const char *ssid = cfg ? cjson_get_string(cfg, "ssid", "unknown") : "unknown";
            influx_write_wifi(imei, ssid,
                cjson_get_int(payload_json, "clients_num", 0));
        } else if (strcmp(ttype, "rfid") == 0) {
            influx_write_rfid(imei,
                cjson_get_string(payload_json, "userID", "unknown"),
                cjson_get_string(payload_json, "stationID", "unknown"),
                cjson_get_int(payload_json, "status", -1));
        } else if (strcmp(ttype, "login") == 0) {
            influx_write_login(imei);
        }

        cJSON_Delete(payload_json);
    }

    free(json_out);
    free(payload_str);
    cJSON_Delete(envelope);
}

/* ── Log ring buffer ──────────────────────────────────────────── */

static void store_log_entry(const char *json, size_t len)
{
    free(msg_log[log_head].json);
    msg_log[log_head].json = strndup(json, len);
    msg_log[log_head].len = len;
    log_head = (log_head + 1) % MAX_LOG_MSGS;
    if (log_count < MAX_LOG_MSGS) log_count++;
}

/* ── WebSocket ────────────────────────────────────────────────── */

static void ws_send(struct lws *wsi, const char *data, size_t len)
{
    unsigned char *buf = malloc(LWS_PRE + len);
    if (!buf) return;
    memcpy(buf + LWS_PRE, data, len);
    lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
    free(buf);
}

static void schedule_ws_broadcast(const char *json, size_t len)
{
    free(pending_broadcast);
    pending_broadcast = strndup(json, len);
    pending_broadcast_len = len;
    for (int i = 0; i < ws_client_count; i++)
        if (ws_clients[i])
            lws_callback_on_writable(ws_clients[i]);
}

static void send_backlog(struct lws *wsi)
{
    if (log_count == 0) return;
    int start = (log_count < MAX_LOG_MSGS) ? 0 : log_head;
    for (int i = 0; i < log_count; i++) {
        int idx = (start + i) % MAX_LOG_MSGS;
        if (msg_log[idx].json)
            ws_send(wsi, msg_log[idx].json, msg_log[idx].len);
    }
}

/* Build and send the subscription list to one client */
static void send_sub_list(struct lws *wsi)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "sub_list");
    cJSON *arr = cJSON_AddArrayToObject(resp, "active_subs");
    for (int i = 0; i < sub_count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(active_subs[i]));

    char *json = cJSON_PrintUnformatted(resp);
    ws_send(wsi, json, strlen(json));
    free(json);
    cJSON_Delete(resp);
}

/* Broadcast subscription list to ALL clients */
static void broadcast_sub_list(void)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "sub_list");
    cJSON *arr = cJSON_AddArrayToObject(resp, "active_subs");
    for (int i = 0; i < sub_count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(active_subs[i]));

    char *json = cJSON_PrintUnformatted(resp);
    size_t len = strlen(json);
    for (int i = 0; i < ws_client_count; i++)
        if (ws_clients[i])
            ws_send(ws_clients[i], json, len);
    free(json);
    cJSON_Delete(resp);
}

/* ── PWA WebSocket command handler ──────────────────────────────── */
/*
 * Handles actions from the TransitTag PWA (seat management, payments,
 * fare configuration).  Called by handle_ws_command for unrecognised
 * action strings.
 *
 * Role enforcement:
 *   Passenger / Driver  — get_seats, select_stop, get_stops, get_dropoffs,
 *                         pay_self, pay_push_them, pay_for_seat
 *   Conductor+          — cash_paid, reset_seat, set_fare, get_summary
 *   Owner+              — set_stops
 */
static void handle_ws_pwa_command(struct lws *wsi, ws_auth_t *auth, cJSON *cmd)
{
    const char *action = cjson_get_string(cmd, "action", "");

/* Helper macro: deny and return if role requirement not met */
#define REQUIRE_ROLE(min_role) \
    do { \
        if (!auth || !auth_require_role(auth, (min_role))) { \
            const char *_d = "{\"type\":\"error\",\"reason\":\"unauthorized\"}"; \
            ws_send(wsi, _d, strlen(_d)); \
            return; \
        } \
    } while (0)

/* Helper macro: validate seat_id is in the legal range [1..MAX_SEATS] */
#define VALIDATE_SEAT_ID(sid) \
    do { \
        if ((sid) < 1 || (sid) > MAX_SEATS) { \
            const char *_e = "{\"type\":\"error\",\"reason\":\"invalid_seat_id\"}"; \
            ws_send(wsi, _e, strlen(_e)); \
            return; \
        } \
    } while (0)

/* Helper macro: validate phone number format (7-15 digits) */
#define VALIDATE_PHONE(ph) \
    do { \
        if (!(ph) || strlen(ph) < 7 || strlen(ph) > 15) { \
            const char *_e = "{\"type\":\"error\",\"reason\":\"invalid_phone\"}"; \
            ws_send(wsi, _e, strlen(_e)); \
            return; \
        } \
        for (size_t _i = 0; (ph)[_i]; _i++) { \
            if (!((ph)[_i] >= '0' && (ph)[_i] <= '9') && (ph)[_i] != '+') { \
                const char *_e = "{\"type\":\"error\",\"reason\":\"invalid_phone\"}"; \
                ws_send(wsi, _e, strlen(_e)); \
                return; \
            } \
        } \
    } while (0)

    if (strcmp(action, "get_seats") == 0) {
        /* Public — any connected client may request seat state */
        char *json = seats_state_to_json(
            pricing_get_fare(),
            pricing_get_route(),
            pricing_is_peak());
        if (json) {
            ws_send(wsi, json, strlen(json));
            free(json);
        }

    } else if (strcmp(action, "pay_self") == 0 ||
               strcmp(action, "pay_push_them") == 0) {
        /*
         * Both flows initiate an STK Push; the difference is who entered
         * the phone number.  The server triggers the same M-Pesa call.
         * For now: transition seat to PAYING and broadcast the update.
         */
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
        VALIDATE_SEAT_ID(seat_id);
        const char *phone = cjson_get_string(cmd, "phone",
                            cjson_get_string(cmd, "their_phone", ""));
        VALIDATE_PHONE(phone);
        if (seat_id > 0) {
            seat_set_paying(seat_id, phone);
            seat_t *s = seat_get(seat_id);
            if (s) {
                char *sj = seat_to_json(s);
                if (sj) {
                    /* Build seat_update message */
                    size_t msglen = strlen(sj) + 32;
                    char  *msg   = malloc(msglen);
                    if (msg) {
                        snprintf(msg, msglen,
                            "{\"type\":\"seat_update\",\"seat\":%s}", sj);
                        schedule_ws_broadcast(msg, strlen(msg));
                        free(msg);
                    }
                    free(sj);
                }
                /* Broadcast updated dropoffs */
                {
                    int sc2 = 0; seat_t *all2 = seat_get_all(&sc2);
                    char *dj = routes_dropoffs_to_json(all2, sc2);
                    if (dj) { schedule_ws_broadcast(dj, strlen(dj)); free(dj); }
                }
            }
        }

    } else if (strcmp(action, "pay_for_seat") == 0) {
        /* Third-party payment: payer_phone is different from seat occupant */
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
        VALIDATE_SEAT_ID(seat_id);
        const char *payer = cjson_get_string(cmd, "payer_phone", "");
        VALIDATE_PHONE(payer);
        if (seat_id > 0) {
            seat_set_paying(seat_id, payer);
            seat_t *s = seat_get(seat_id);
            if (s) {
                /* Store payer phone in the seat record */
                strncpy(s->payer_phone, payer, sizeof(s->payer_phone) - 1);
                char *sj = seat_to_json(s);
                if (sj) {
                    size_t msglen = strlen(sj) + 32;
                    char  *msg   = malloc(msglen);
                    if (msg) {
                        snprintf(msg, msglen,
                            "{\"type\":\"seat_update\",\"seat\":%s}", sj);
                        schedule_ws_broadcast(msg, strlen(msg));
                        free(msg);
                    }
                    free(sj);
                }
                /* Broadcast updated dropoffs */
                {
                    int sc2 = 0; seat_t *all2 = seat_get_all(&sc2);
                    char *dj = routes_dropoffs_to_json(all2, sc2);
                    if (dj) { schedule_ws_broadcast(dj, strlen(dj)); free(dj); }
                }
            }
        }

    } else if (strcmp(action, "cash_paid") == 0) {
        REQUIRE_ROLE(ROLE_CONDUCTOR);  /* Only conductor or owner can mark cash */
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
        VALIDATE_SEAT_ID(seat_id);
        if (seat_id > 0) {
            seat_set_paid_cash(seat_id);
            seat_t *s = seat_get(seat_id);
            if (s) {
                char *sj = seat_to_json(s);
                if (sj) {
                    size_t msglen = strlen(sj) + 32;
                    char  *msg   = malloc(msglen);
                    if (msg) {
                        snprintf(msg, msglen,
                            "{\"type\":\"seat_update\",\"seat\":%s}", sj);
                        schedule_ws_broadcast(msg, strlen(msg));
                        free(msg);
                    }
                    free(sj);
                }
                /* Broadcast updated dropoffs */
                {
                    int sc2 = 0; seat_t *all2 = seat_get_all(&sc2);
                    char *dj = routes_dropoffs_to_json(all2, sc2);
                    if (dj) { schedule_ws_broadcast(dj, strlen(dj)); free(dj); }
                }
            }
        }

    } else if (strcmp(action, "reset_seat") == 0) {
        REQUIRE_ROLE(ROLE_CONDUCTOR);  /* Only conductor or owner can reset */
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
        VALIDATE_SEAT_ID(seat_id);
        if (seat_id > 0) {
            seat_reset(seat_id);
            seat_t *s = seat_get(seat_id);
            if (s) {
                char *sj = seat_to_json(s);
                if (sj) {
                    size_t msglen = strlen(sj) + 32;
                    char  *msg   = malloc(msglen);
                    if (msg) {
                        snprintf(msg, msglen,
                            "{\"type\":\"seat_update\",\"seat\":%s}", sj);
                        schedule_ws_broadcast(msg, strlen(msg));
                        free(msg);
                    }
                    free(sj);
                }
            }
        }

    } else if (strcmp(action, "set_fare") == 0) {
        REQUIRE_ROLE(ROLE_CONDUCTOR);  /* Only conductor or owner can set fare */
        const char *route = cjson_get_string(cmd, "route", NULL);
        int         fare  = cjson_get_int(cmd, "fare", 0);
        pricing_set(route, fare);
        /* Broadcast updated seats_state so all PWA clients refresh */
        char *json = seats_state_to_json(
            pricing_get_fare(),
            pricing_get_route(),
            pricing_is_peak());
        if (json) {
            schedule_ws_broadcast(json, strlen(json));
            free(json);
        }

    } else if (strcmp(action, "get_summary") == 0) {
        REQUIRE_ROLE(ROLE_CONDUCTOR);  /* Revenue data: conductor and owner only */
        day_summary_t ds  = seat_get_day_summary();
        char *json = day_summary_to_json(&ds,
            g_lat, g_lon, g_speed, g_battery, g_gsm);
        if (json) {
            ws_send(wsi, json, strlen(json));
            free(json);
        }

    } else if (strcmp(action, "auth") == 0) {
        const char *role = cjson_get_string(cmd, "role", "");
        const char *pin  = cjson_get_string(cmd, "pin",  "");
        printf("[WS] auth attempt: role=%s\n", role);

        if (!auth) {
            /* Should never happen — per-session data always allocated */
            const char *fail = "{\"type\":\"auth_fail\",\"reason\":\"internal\"}";
            ws_send(wsi, fail, strlen(fail));
        } else {
            int lockout = auth_lockout_remaining(auth);
            if (lockout > 0) {
                /* Session locked — tell client how long to wait */
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"auth_fail\",\"reason\":\"locked\",\"wait\":%d}",
                    lockout);
                ws_send(wsi, buf, strlen(buf));
            } else {
                int result = auth_attempt(auth, role, pin);
                if (result == 1) {
                    char buf[80];
                    snprintf(buf, sizeof(buf),
                        "{\"type\":\"auth_ok\",\"role\":\"%s\"}",
                        auth_role_str(auth->role));
                    ws_send(wsi, buf, strlen(buf));
                } else {
                    char buf[96];
                    int attempts_left = PIN_MAX_ATTEMPTS - auth->pin_attempts;
                    if (attempts_left < 0) attempts_left = 0;
                    snprintf(buf, sizeof(buf),
                        "{\"type\":\"auth_fail\",\"reason\":\"wrong_pin\","
                        "\"attempts_left\":%d}", attempts_left);
                    ws_send(wsi, buf, strlen(buf));
                }
            }
        }

    } else if (strcmp(action, "select_stop") == 0) {
        int stop_id = cjson_get_int(cmd, "stop_id", -1);
        int seat_id = cjson_get_int(cmd, "seat_id", -1);
        if (stop_id >= 0) {
            /* Record the passenger's destination on the seat record */
            if (seat_id > 0) {
                seat_t *dest_seat = seat_get(seat_id);
                if (dest_seat) dest_seat->dest_stop_id = stop_id;
            }

            int dist = routes_get_distance_m(g_lat, g_lon, stop_id);
            cJSON *resp = cJSON_CreateObject();
            if (dist > 0) {
                /* find stop name */
                int sc = 0;
                const route_stop_t *stops = routes_get_stops(&sc);
                const char *sname = "Unknown";
                for (int i = 0; i < sc; i++)
                    if (stops[i].id == stop_id) { sname = stops[i].name; break; }

                pricing_set_distance(stop_id, dist, sname);
                int fare = pricing_get_fare();

                cJSON_AddStringToObject(resp, "type", "fare_quote");
                cJSON_AddNumberToObject(resp, "seat_id", seat_id);
                cJSON_AddNumberToObject(resp, "stop_id", stop_id);
                cJSON_AddStringToObject(resp, "stop_name", sname);
                cJSON_AddNumberToObject(resp, "distance_m", dist);
                cJSON_AddNumberToObject(resp, "base_fare", pricing_get_config()->base_fare);
                cJSON_AddBoolToObject(resp,   "peak", pricing_is_peak());
                cJSON_AddNumberToObject(resp, "effective_fare", fare);
            } else {
                cJSON_AddStringToObject(resp, "type", "fare_error");
                cJSON_AddNumberToObject(resp, "seat_id", seat_id);
                cJSON_AddStringToObject(resp, "reason", "distance_lookup_failed");
            }
            char *js = cJSON_PrintUnformatted(resp);
            /* broadcast fare_quote; send fare_error only to requestor */
            if (dist > 0) schedule_ws_broadcast(js, strlen(js));
            else ws_send(wsi, js, strlen(js));
            free(js);
            cJSON_Delete(resp);

            /* also broadcast updated seats_state with new fare */
            if (dist > 0) {
                char *ss = seats_state_to_json(pricing_get_fare(),
                                               pricing_get_route(),
                                               pricing_is_peak());
                schedule_ws_broadcast(ss, strlen(ss));
                free(ss);
            }

            /* Broadcast updated dropoffs now that a destination was set */
            {
                int sc2 = 0; seat_t *all2 = seat_get_all(&sc2);
                char *dj = routes_dropoffs_to_json(all2, sc2);
                if (dj) { schedule_ws_broadcast(dj, strlen(dj)); free(dj); }
            }
        }

    } else if (strcmp(action, "get_stops") == 0) {
        char *sj = routes_stops_to_json();
        if (sj) { ws_send(wsi, sj, strlen(sj)); free(sj); }

    } else if (strcmp(action, "set_stops") == 0) {
        REQUIRE_ROLE(ROLE_CONDUCTOR);  /* Conductor or owner can set route stops */
        /* Payload: { action:"set_stops", route:"CBD → Westlands", stops:[{id,name,lat,lon},...] } */
        const char *route_name = cjson_get_string(cmd, "route", "Unknown Route");
        cJSON *stops_arr = cJSON_GetObjectItem(cmd, "stops");
        if (stops_arr && cJSON_IsArray(stops_arr)) {
            int n = cJSON_GetArraySize(stops_arr);
            if (n > 0 && n <= MAX_STOPS) {
                route_stop_t new_stops[MAX_STOPS];
                for (int i = 0; i < n; i++) {
                    cJSON *s = cJSON_GetArrayItem(stops_arr, i);
                    new_stops[i].id  = cjson_get_int(s, "id", i + 1);
                    snprintf(new_stops[i].name, STOP_NAME_LEN, "%s",
                             cjson_get_string(s, "name", "Stop"));
                    new_stops[i].lat = cjson_get_double(s, "lat", 0.0);
                    new_stops[i].lon = cjson_get_double(s, "lon", 0.0);
                }
                if (routes_set_stops(route_name, new_stops, n) == 0) {
                    /* Broadcast updated stops list to all clients */
                    char *sj = routes_stops_to_json();
                    if (sj) { schedule_ws_broadcast(sj, strlen(sj)); free(sj); }
                    /* Also update pricing route name */
                    pricing_set(route_name, pricing_get_config()->base_fare);
                }
            }
        }

    } else if (strcmp(action, "get_dropoffs") == 0) {
        int sc = 0;
        seat_t *all = seat_get_all(&sc);
        char *dj = routes_dropoffs_to_json(all, sc);
        if (dj) { ws_send(wsi, dj, strlen(dj)); free(dj); }

    }
}

/* Handle incoming WebSocket command from browser */
static void handle_ws_command(struct lws *wsi, ws_auth_t *auth,
                               const char *data, size_t len)
{
    char *str = strndup(data, len);
    cJSON *cmd = cJSON_Parse(str);
    free(str);
    if (!cmd) return;

    const char *action = cjson_get_string(cmd, "action", NULL);
    if (!action) { cJSON_Delete(cmd); return; }

    if (strcmp(action, "subscribe") == 0) {
        /* Only conductor or owner can add subscriptions */
        if (auth && !auth_require_role(auth, ROLE_CONDUCTOR)) {
            const char *deny = "{\"type\":\"error\",\"reason\":\"unauthorized\"}";
            ws_send(wsi, deny, strlen(deny));
            cJSON_Delete(cmd);
            return;
        }
        const char *topic = cjson_get_string(cmd, "topic", NULL);
        if (topic && strlen(topic) > 0 && strlen(topic) < MAX_TOPIC_LEN
            && sub_count < MAX_SUBSCRIPTIONS) {
            /* Validate: only allow TransitTag topic namespace */
            if (strncmp(topic, "/topic/transittag/", 18) != 0 &&
                strncmp(topic, "/topic/transittag", 17) != 0) {
                const char *deny = "{\"type\":\"error\",\"reason\":\"topic_not_allowed\"}";
                ws_send(wsi, deny, strlen(deny));
                cJSON_Delete(cmd);
                return;
            }
            int found = 0;
            for (int i = 0; i < sub_count; i++)
                if (strcmp(active_subs[i], topic) == 0) { found = 1; break; }
            if (!found) {
                mosquitto_subscribe(g_mosq, NULL, topic, 0);
                strncpy(active_subs[sub_count], topic, MAX_TOPIC_LEN - 1);
                active_subs[sub_count][MAX_TOPIC_LEN - 1] = '\0';
                sub_count++;
                printf("[MQTT] Subscribed: %s\n", topic);
            }
            broadcast_sub_list();
        }
    } else if (strcmp(action, "unsubscribe") == 0) {
        if (auth && !auth_require_role(auth, ROLE_CONDUCTOR)) {
            const char *deny = "{\"type\":\"error\",\"reason\":\"unauthorized\"}";
            ws_send(wsi, deny, strlen(deny));
            cJSON_Delete(cmd);
            return;
        }
        const char *topic = cjson_get_string(cmd, "topic", NULL);
        if (topic) {
            for (int i = 0; i < sub_count; i++) {
                if (strcmp(active_subs[i], topic) == 0) {
                    mosquitto_unsubscribe(g_mosq, NULL, topic);
                    printf("[MQTT] Unsubscribed: %s\n", topic);
                    memmove(&active_subs[i], &active_subs[i + 1],
                            (sub_count - i - 1) * MAX_TOPIC_LEN);
                    sub_count--;
                    break;
                }
            }
            broadcast_sub_list();
        }
    } else if (strcmp(action, "list_subscriptions") == 0) {
        send_sub_list(wsi);
    } else {
        handle_ws_pwa_command(wsi, auth, cmd);
    }

    cJSON_Delete(cmd);
}

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    ws_auth_t *auth = (ws_auth_t *)user;

    switch (reason) {

    /*
     * Origin validation — reject WS upgrades from unexpected origins.
     * This prevents cross-site WebSocket hijacking (CSWSH) attacks where
     * a page on a different origin initiates a WS connection via a
     * victim's browser while they are connected to the vehicle hotspot.
     *
     * We accept: no Origin header (native WS clients), and any http://
     * origin on the same host (hotspot IP or hostname).  HTTPS origins
     * will be added if/when TLS is deployed on the server.
     */
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
        char origin[256] = {0};
        int olen = lws_hdr_copy(wsi, origin, sizeof(origin) - 1,
                                WSI_TOKEN_ORIGIN);
        if (olen > 0) {
            /* Build expected origin from the Host header */
            char host[128] = {0};
            lws_hdr_copy(wsi, host, sizeof(host) - 1, WSI_TOKEN_HOST);
            /* Accept http://<host> or https://<host> matching this server */
            int ok = 0;
            char expected_http[256], expected_https[256];
            snprintf(expected_http,  sizeof(expected_http),  "http://%s",  host);
            snprintf(expected_https, sizeof(expected_https), "https://%s", host);
            if (strcmp(origin, expected_http)  == 0 ||
                strcmp(origin, expected_https) == 0)
                ok = 1;
            if (!ok) {
                printf("[WS] Rejected origin: '%s' (host: '%s')\n", origin, host);
                return -1;
            }
        }
        /* No Origin header — allow (native client or same-origin fetch) */
        break;
    }

    case LWS_CALLBACK_ESTABLISHED:
        if (auth) auth_init(auth);
        printf("[WS] Client connected (%d total)\n", ws_client_count + 1);
        if (ws_client_count < MAX_WS_CLIENTS) {
            ws_clients[ws_client_count] = wsi;
            ws_client_count++;
        }
        send_backlog(wsi);
        send_sub_list(wsi);
        /* Send current seat state so PWA clients populate immediately */
        {
            char *sj = seats_state_to_json(
                pricing_get_fare(),
                pricing_get_route(),
                pricing_is_peak());
            if (sj) {
                ws_send(wsi, sj, strlen(sj));
                free(sj);
            }
        }
        /* send route stops to new client */
        {
            char *sj = routes_stops_to_json();
            if (sj) { ws_send(wsi, sj, strlen(sj)); free(sj); }
        }
        /* send current dropoffs to new client */
        {
            int sc2 = 0; seat_t *all2 = seat_get_all(&sc2);
            char *dj = routes_dropoffs_to_json(all2, sc2);
            if (dj) { ws_send(wsi, dj, strlen(dj)); free(dj); }
        }
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (pending_broadcast && pending_broadcast_len > 0)
            ws_send(wsi, pending_broadcast, pending_broadcast_len);
        break;

    case LWS_CALLBACK_RECEIVE:
        if (in && len > 0) {
            /* Hard cap on frame size */
            if (len > MAX_WS_MSG_BYTES) {
                printf("[WS] Oversized message (%zu bytes) — closing\n", len);
                return -1;
            }
            /* Per-session rate limiting: max RATE_MAX_MSGS per RATE_WINDOW_SEC */
            if (auth) {
                time_t now = time(NULL);
                if (now - auth->rate_window_start >= RATE_WINDOW_SEC) {
                    auth->rate_window_start = now;
                    auth->rate_msg_count    = 0;
                }
                if (++auth->rate_msg_count > RATE_MAX_MSGS) {
                    /* Silent drop — avoids giving attacker timing feedback */
                    break;
                }
            }
            handle_ws_command(wsi, auth, (const char *)in, len);
        }
        break;

    case LWS_CALLBACK_CLOSED:
        printf("[WS] Client disconnected\n");
        for (int i = 0; i < ws_client_count; i++) {
            if (ws_clients[i] == wsi) {
                ws_clients[i] = ws_clients[ws_client_count - 1];
                ws_client_count--;
                break;
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ── HTTP file serving ────────────────────────────────────────── */

static void url_decode(const char *src, char *dst, size_t dstlen) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dstlen; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static const char *get_mimetype(const char *path)
{
    size_t n = strlen(path);
    if (n > 5 && strcmp(path + n - 5, ".html") == 0) return "text/html";
    if (n > 4 && strcmp(path + n - 4, ".css") == 0)  return "text/css";
    if (n > 3 && strcmp(path + n - 3, ".js") == 0)   return "application/javascript";
    if (n > 5 && strcmp(path + n - 5, ".json") == 0) return "application/json";
    if (n > 4 && strcmp(path + n - 4, ".svg") == 0)  return "image/svg+xml";
    if (n > 4 && strcmp(path + n - 4, ".png") == 0)  return "image/png";
    if (n > 4 && strcmp(path + n - 4, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len)
{
    (void)user; (void)len;

    switch (reason) {
    case LWS_CALLBACK_HTTP: {
        const char *uri = (const char *)in;
        char path[512];

        if (!uri || strcmp(uri, "/") == 0)
            uri = "/index.html";

        /* /api/qr?data=ENCODED_URL — returns SVG for any data string */
        if (strncmp(uri, "/api/qr", 7) == 0) {
            const char *q = strchr(uri, '?');
            char decoded[2048] = {0};
            if (q) {
                const char *dp = strstr(q, "data=");
                if (dp) {
                    url_decode(dp + 5, decoded, sizeof(decoded));
                }
            }
            /* Basic input validation: reject oversized or obviously malicious data */
            if (strlen(decoded) > 1024) {
                lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, "Bad Request");
                return -1;
            }
            char *svg = NULL;
            if (decoded[0]) {
                svg = qr_svg_generate(decoded, 4);
            }
            /* Use mkstemps to avoid predictable temp file paths (symlink attack) */
            char tmppath[64];
            snprintf(tmppath, sizeof(tmppath), TMP_DIR "/tt_qrXXXXXX.svg");
            int tfd = mkstemps(tmppath, 4);  /* 4 = strlen(".svg") suffix */
            if (tfd >= 0) {
                FILE *fsvg = fdopen(tfd, "w");
                if (fsvg) {
                    if (svg) fputs(svg, fsvg);
                    else fputs("<svg xmlns=\"http://www.w3.org/2000/svg\"/>", fsvg);
                    fclose(fsvg);
                } else {
                    close(tfd);
                }
            }
            free(svg);
            int n = lws_serve_http_file(wsi, tmppath, "image/svg+xml",
                                        g_sec_headers, g_sec_headers_len);
            if (n < 0) return -1;
            break;
        }

        /* /print-qrs — printable seat label page with QR codes for all 14 seats */
        if (strncmp(uri, "/print-qrs", 10) == 0) {
            char tmppath[68];
            snprintf(tmppath, sizeof(tmppath), TMP_DIR "/tt_printXXXXXX.html");
            int ptfd = mkstemps(tmppath, 5);  /* 5 = strlen(".html") */
            FILE *fhtml = (ptfd >= 0) ? fdopen(ptfd, "w") : NULL;
            if (!fhtml && ptfd >= 0) close(ptfd);
            if (fhtml) {
                fputs("<!DOCTYPE html>\n"
                      "<html>\n"
                      "<head>\n"
                      "<title>TransitTag \xe2\x80\x94 Seat QR Codes</title>\n"
                      "<style>\n"
                      "  body { font-family: sans-serif; background: #fff; margin: 0; padding: 16px; }\n"
                      "  h1 { font-size: 18px; margin-bottom: 16px; text-align: center; }\n"
                      "  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }\n"
                      "  .label { border: 2px solid #000; border-radius: 8px; padding: 12px;\n"
                      "           text-align: center; page-break-inside: avoid; }\n"
                      "  .seat-num { font-size: 28px; font-weight: bold; margin-bottom: 8px; }\n"
                      "  .qr-img { width: 140px; height: 140px; display: block; margin: 0 auto; }\n"
                      "  .scan-text { font-size: 11px; color: #444; margin-top: 8px; }\n"
                      "  @media print { body { padding: 0; } }\n"
                      "</style>\n"
                      "</head>\n"
                      "<body>\n"
                      "<h1>TransitTag \xe2\x80\x94 Seat QR Codes</h1>\n"
                      "<div class=\"grid\" id=\"grid\"></div>\n"
                      "<script>\n"
                      "var base = location.origin + '/pwa/?seat=';\n"
                      "var grid = document.getElementById('grid');\n"
                      "for (var i = 1; i <= 14; i++) {\n"
                      "  var url = base + i;\n"
                      "  var d = document.createElement('div');\n"
                      "  d.className = 'label';\n"
                      "  d.innerHTML = '<div class=\"seat-num\">SEAT ' + i + '</div>'\n"
                      "    + '<img class=\"qr-img\" src=\"/api/qr?data=' + encodeURIComponent(url) + '\" alt=\"QR\">'\n"
                      "    + '<div class=\"scan-text\">Scan to pay &bull; TransitTag</div>';\n"
                      "  grid.appendChild(d);\n"
                      "}\n"
                      "</script>\n"
                      "</body>\n"
                      "</html>\n", fhtml);
                fclose(fhtml);
            }
            int n = lws_serve_http_file(wsi, tmppath, "text/html",
                                        g_sec_headers, g_sec_headers_len);
            if (n < 0) return -1;
            break;
        }

        /* Path traversal and injection protection */
        if (strstr(uri, "..") || strstr(uri, "//") ||
            strstr(uri, "\r")  || strstr(uri, "\n")) {
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, "Forbidden");
            return -1;
        }

        /* /api/fare?stop_id=N&lat=L&lon=O */
        if (strncmp(uri, "/api/fare", 9) == 0) {
            const char *p = strchr(uri, '?');
            int stop_id = -1;
            double qlat = g_lat, qlon = g_lon;
            if (p) {
                const char *sp = strstr(p, "stop_id=");
                if (sp) stop_id = atoi(sp + 8);
            }
            cJSON *jr = cJSON_CreateObject();
            if (stop_id >= 0) {
                int dist = routes_get_distance_m(qlat, qlon, stop_id);
                int sc2 = 0;
                const route_stop_t *stops2 = routes_get_stops(&sc2);
                const char *sname2 = "Unknown";
                for (int i = 0; i < sc2; i++)
                    if (stops2[i].id == stop_id) { sname2 = stops2[i].name; break; }
                int bfare = routes_distance_to_base_fare(dist > 0 ? dist : 0);
                int efare = pricing_is_peak() ? (int)(bfare * 1.5) : bfare;
                cJSON_AddNumberToObject(jr, "stop_id",        stop_id);
                cJSON_AddStringToObject(jr, "stop_name",      sname2);
                cJSON_AddNumberToObject(jr, "distance_m",     dist > 0 ? dist : 0);
                cJSON_AddNumberToObject(jr, "base_fare",      bfare);
                cJSON_AddBoolToObject(jr,   "peak",           pricing_is_peak());
                cJSON_AddNumberToObject(jr, "effective_fare", efare);
                cJSON_AddNumberToObject(jr, "peak_multiplier", pricing_is_peak() ? 1.5 : 1.0);
            } else {
                cJSON_AddStringToObject(jr, "error", "missing stop_id");
            }
            char *jrstr = cJSON_PrintUnformatted(jr);
            cJSON_Delete(jr);
            /* Write JSON to a temp file and serve it (mkstemp avoids symlink attack) */
            char tmppath[68];
            snprintf(tmppath, sizeof(tmppath), TMP_DIR "/tt_fareXXXXXX.json");
            int jfd = mkstemps(tmppath, 5);   /* 5 = strlen(".json") */
            FILE *ftmp = (jfd >= 0) ? fdopen(jfd, "w") : NULL;
            if (!ftmp && jfd >= 0) close(jfd);
            if (ftmp) {
                fputs(jrstr, ftmp);
                fclose(ftmp);
            }
            free(jrstr);
            int n = lws_serve_http_file(wsi, tmppath,
                                        "application/json",
                                        g_sec_headers, g_sec_headers_len);
            if (n < 0) return -1;
            break;
        }

        snprintf(path, sizeof(path), "%s%s", WWW_DIR, uri);

        printf("[HTTP] %s -> %s\n", uri, path);

        const char *mime = get_mimetype(path);
        int n = lws_serve_http_file(wsi, path, mime,
                                    g_sec_headers, g_sec_headers_len);
        if (n < 0) return -1;
        break;
    }

    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
        if (lws_http_transaction_completed(wsi))
            return -1;
        break;

    default:
        break;
    }
    return 0;
}

/* ── LWS setup ────────────────────────────────────────────────── */

static struct lws_protocols protocols[] = {
    { "http-only",    callback_http, 0,                0,    0, NULL, 0 },
    { "dashboard-ws", callback_ws,  sizeof(ws_auth_t), 4096, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

/* Mount: /ws -> WebSocket callback, / -> file serving callback */
static const struct lws_http_mount mount_ws = {
    .mount_next       = NULL,
    .mountpoint       = "/ws",
    .mountpoint_len   = 3,
    .origin           = "dashboard-ws",
    .origin_protocol  = LWSMPRO_CALLBACK,
    .protocol         = "dashboard-ws",
};

/* For file serving we use the origin as root dir */
static const struct lws_http_mount mount_root = {
    .mount_next       = &mount_ws,
    .mountpoint       = "/",
    .mountpoint_len   = 1,
    .origin           = WWW_DIR,
    .def              = "index.html",
    .origin_protocol  = LWSMPRO_FILE,
};

/* ── Signal handler ───────────────────────────────────────────── */

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <username> <password>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *username = argv[1];
    const char *password = argv[2];

    setbuf(stdout, NULL);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* ── Seat manager + pricing ── */
    seat_manager_init(14);
    pricing_init();
    routes_init();
    printf("[Seats] Initialised 14 seats, base fare KES %d, route: %s\n",
           pricing_get_fare(), pricing_get_route());

    /* ── Default subscription ── */
    strncpy(active_subs[0], "/topic/transittag/#", MAX_TOPIC_LEN - 1);
    sub_count = 1;

    /* ── InfluxDB (optional — env vars) ── */
    {
        const char *influx_url    = getenv("INFLUX_URL");
        const char *influx_token  = getenv("INFLUX_TOKEN");
        const char *influx_org    = getenv("INFLUX_ORG");
        const char *influx_bucket = getenv("INFLUX_BUCKET");

        if (influx_url && influx_token) {
            influx_init(
                influx_url,
                influx_org    ? influx_org    : "transittag",
                influx_bucket ? influx_bucket : "iot_data",
                influx_token);
        } else {
            printf("[InfluxDB] Disabled — set INFLUX_URL and INFLUX_TOKEN to enable\n");
        }
    }

    /* ── MQTT ── */
    mosquitto_lib_init();

    /* Unique client ID per session to avoid stale session conflicts */
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "%s%d", CLIENT_ID_PREFIX, (int)getpid());

    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "Failed to create mosquitto instance\n");
        return EXIT_FAILURE;
    }

    mosquitto_username_pw_set(g_mosq, username, password);
    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_message_callback_set(g_mosq, on_message);

    /*
     * MQTT TLS — enabled when MQTT_CA_CERT env var is set.
     * Set MQTT_CA_CERT=/path/to/ca.crt to enable TLS on port 8883.
     * Optionally set MQTT_CLIENT_CERT and MQTT_CLIENT_KEY for mutual TLS.
     */
    int mqtt_port = BROKER_PORT;
    {
        const char *ca_cert     = getenv("MQTT_CA_CERT");
        const char *client_cert = getenv("MQTT_CLIENT_CERT");
        const char *client_key  = getenv("MQTT_CLIENT_KEY");
        if (ca_cert) {
            int tls_rc = mosquitto_tls_set(g_mosq,
                ca_cert, NULL, client_cert, client_key, NULL);
            if (tls_rc == MOSQ_ERR_SUCCESS) {
                mqtt_port = BROKER_PORT_TLS;
                printf("[MQTT] TLS enabled — connecting on port %d\n", mqtt_port);
            } else {
                fprintf(stderr, "[MQTT] TLS setup failed: %s — "
                        "falling back to plain TCP\n",
                        mosquitto_strerror(tls_rc));
            }
        }
    }

    int rc = mosquitto_connect(g_mosq, BROKER_HOST, mqtt_port, KEEP_ALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Connect error: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }
    printf("[MQTT] Connecting to %s:%d ...\n", BROKER_HOST, BROKER_PORT);

    /* ── libwebsockets ── */
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = HTTP_PORT;
    info.protocols = protocols;
    info.mounts = &mount_root;
    info.vhost_name = "dashboard";

    g_lws_ctx = lws_create_context(&info);
    if (!g_lws_ctx) {
        fprintf(stderr, "[LWS] Failed to create context\n");
        mosquitto_disconnect(g_mosq);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }
    printf("[HTTP] Dashboard: http://localhost:%d\n", HTTP_PORT);

    /* ── Event loop ── */
    /*
     * timeout_ticks: increments every lws_service call (~50 ms each).
     * 200 ticks ≈ 10 seconds — used to trigger seat_check_timeouts().
     */
    int timeout_ticks = 0;

    while (running) {
        rc = mosquitto_loop(g_mosq, 0, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            printf("[MQTT] Loop error: %s — reconnecting...\n",
                   mosquitto_strerror(rc));
            mosquitto_reconnect(g_mosq);
        }
        lws_service(g_lws_ctx, 50);

        /* Check seat payment timeouts every ~10 seconds */
        if (++timeout_ticks >= 200) {
            timeout_ticks = 0;
            seat_check_timeouts();
        }
    }

    /* ── Cleanup ── */
    printf("\nShutting down...\n");
    lws_context_destroy(g_lws_ctx);
    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    influx_cleanup();
    routes_cleanup();
    for (int i = 0; i < MAX_LOG_MSGS; i++)
        free(msg_log[i].json);
    free(pending_broadcast);
    printf("Clean shutdown.\n");
    return EXIT_SUCCESS;
}
