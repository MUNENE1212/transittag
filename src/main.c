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

/* ── Configuration ────────────────────────────────────────────── */

#define BROKER_HOST  "byte-iot.net"
#define BROKER_PORT  1883
#define KEEP_ALIVE   60
#define CLIENT_ID_PREFIX "inv_dash_"
#define HTTP_PORT    8080
#define MAX_WS_CLIENTS 32
#define MAX_LOG_MSGS   100
#define MAX_SUBSCRIPTIONS 16
#define MAX_TOPIC_LEN 256

#ifndef WWW_DIR
#define WWW_DIR "./www"
#endif

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
 */
static void handle_ws_pwa_command(struct lws *wsi, cJSON *cmd)
{
    const char *action = cjson_get_string(cmd, "action", "");

    if (strcmp(action, "get_seats") == 0) {
        /* Send current seat state to requesting client only */
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
        const char *phone = cjson_get_string(cmd, "phone",
                            cjson_get_string(cmd, "their_phone", ""));
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
            }
        }

    } else if (strcmp(action, "pay_for_seat") == 0) {
        /* Third-party payment: payer_phone is different from seat occupant */
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
        const char *payer = cjson_get_string(cmd, "payer_phone", "");
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
            }
        }

    } else if (strcmp(action, "cash_paid") == 0) {
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
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
            }
        }

    } else if (strcmp(action, "reset_seat") == 0) {
        int seat_id = cjson_get_int(cmd, "seat_id", 0);
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
        day_summary_t ds  = seat_get_day_summary();
        char *json = day_summary_to_json(&ds,
            g_lat, g_lon, g_speed, g_battery, g_gsm);
        if (json) {
            ws_send(wsi, json, strlen(json));
            free(json);
        }

    } else if (strcmp(action, "auth") == 0) {
        /*
         * PIN authentication.  For now we accept any PIN from a connected
         * client and rely on the local JS check.  A production deployment
         * would validate against a server-side PIN store.
         * Respond with auth_ok to allow the client to enter the role.
         */
        const char *role = cjson_get_string(cmd, "role", "");
        const char *pin  = cjson_get_string(cmd, "pin",  "");
        printf("[WS] auth attempt: role=%s pin=****\n", role);
        (void)pin; /* Audit log would go here */

        const char *resp = "{\"type\":\"auth_ok\"}";
        ws_send(wsi, resp, strlen(resp));
    }
}

/* Handle incoming WebSocket command from browser */
static void handle_ws_command(struct lws *wsi, const char *data, size_t len)
{
    char *str = strndup(data, len);
    cJSON *cmd = cJSON_Parse(str);
    free(str);
    if (!cmd) return;

    const char *action = cjson_get_string(cmd, "action", NULL);
    if (!action) { cJSON_Delete(cmd); return; }

    if (strcmp(action, "subscribe") == 0) {
        const char *topic = cjson_get_string(cmd, "topic", NULL);
        if (topic && strlen(topic) > 0 && strlen(topic) < MAX_TOPIC_LEN
            && sub_count < MAX_SUBSCRIPTIONS) {
            /* Check not already subscribed */
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
        const char *topic = cjson_get_string(cmd, "topic", NULL);
        if (topic) {
            for (int i = 0; i < sub_count; i++) {
                if (strcmp(active_subs[i], topic) == 0) {
                    mosquitto_unsubscribe(g_mosq, NULL, topic);
                    printf("[MQTT] Unsubscribed: %s\n", topic);
                    /* Compact array */
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
        /* Dispatch to PWA seat/fare/payment command handler */
        handle_ws_pwa_command(wsi, cmd);
    }

    cJSON_Delete(cmd);
}

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    (void)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
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
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (pending_broadcast && pending_broadcast_len > 0)
            ws_send(wsi, pending_broadcast, pending_broadcast_len);
        break;

    case LWS_CALLBACK_RECEIVE:
        if (in && len > 0)
            handle_ws_command(wsi, (const char *)in, len);
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

        /* Path traversal protection */
        if (strstr(uri, "..")) {
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, "Forbidden");
            return -1;
        }

        snprintf(path, sizeof(path), "%s%s", WWW_DIR, uri);

        printf("[HTTP] %s -> %s\n", uri, path);

        const char *mime = get_mimetype(path);
        int n = lws_serve_http_file(wsi, path, mime, NULL, 0);
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
    { "http-only", callback_http, 0, 0 },
    { "dashboard-ws", callback_ws, 0, 4096 },
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

    int rc = mosquitto_connect(g_mosq, BROKER_HOST, BROKER_PORT, KEEP_ALIVE);
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
    for (int i = 0; i < MAX_LOG_MSGS; i++)
        free(msg_log[i].json);
    free(pending_broadcast);
    printf("Clean shutdown.\n");
    return EXIT_SUCCESS;
}
