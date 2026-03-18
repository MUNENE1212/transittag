#include "influxdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <curl/curl.h>

/*
 * safe_tag — sanitise a string for use as an InfluxDB line-protocol tag value.
 * Commas, spaces, equals, quotes, and backslashes are InfluxDB delimiters;
 * they must be escaped or stripped to prevent line-protocol injection from
 * attacker-controlled MQTT payloads (crafted IMEI, SSID, user_id, etc.).
 */
static void safe_tag(const char *src, char *dst, size_t n)
{
    if (!src || !*src) { snprintf(dst, n, "unknown"); return; }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ',' || c == ' ' || c == '=' || c == '"' || c == '\\') {
            dst[j++] = '\\';   /* escape in-place */
            dst[j++] = (char)c;
        } else if (c == '\n' || c == '\r' || c < 0x20 || c == 0x7f) {
            /* strip non-printable / newlines — they terminate the line protocol */
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
    if (j == 0) snprintf(dst, n, "unknown");
}

static CURL *g_curl = NULL;
static char g_write_url[512];
static char g_auth_header[256];
static struct curl_slist *g_headers = NULL;
static int g_enabled = 0;

/* Discard response body */
static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
    (void)ptr; (void)data;
    return size * nmemb;
}

int influx_init(const char *url, const char *org, const char *bucket, const char *token)
{
    if (!url || !org || !bucket || !token) {
        printf("[InfluxDB] Disabled — missing config\n");
        return -1;
    }

    g_curl = curl_easy_init();
    if (!g_curl) {
        fprintf(stderr, "[InfluxDB] curl_easy_init failed\n");
        return -1;
    }

    snprintf(g_write_url, sizeof(g_write_url),
             "%s/api/v2/write?org=%s&bucket=%s&precision=ns", url, org, bucket);
    snprintf(g_auth_header, sizeof(g_auth_header), "Authorization: Token %s", token);

    g_headers = curl_slist_append(NULL, g_auth_header);
    g_headers = curl_slist_append(g_headers, "Content-Type: text/plain");

    curl_easy_setopt(g_curl, CURLOPT_URL, g_write_url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, g_headers);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT_MS, 500L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT_MS, 300L);

    g_enabled = 1;
    printf("[InfluxDB] Initialized → %s (bucket: %s)\n", url, bucket);
    return 0;
}

static int influx_post(const char *line)
{
    if (!g_enabled || !g_curl) return -1;

    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, line);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE, (long)strlen(line));

    CURLcode res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        /* Rate-limit error logging — only print occasionally */
        static time_t last_err = 0;
        time_t now = time(NULL);
        if (now - last_err > 10) {
            fprintf(stderr, "[InfluxDB] Write failed: %s\n", curl_easy_strerror(res));
            last_err = now;
        }
        return -1;
    }

    long code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 204) {
        static time_t last_err = 0;
        time_t now = time(NULL);
        if (now - last_err > 10) {
            fprintf(stderr, "[InfluxDB] HTTP %ld on write\n", code);
            last_err = now;
        }
        return -1;
    }
    return 0;
}

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int influx_write_heartbeat(const char *imei, double battery, int gsm,
    double lat, double lon, double speed, int sats,
    const char *acc, int moving, int alarm)
{
    if (!g_enabled) return -1;
    char s_imei[64], s_acc[16];
    safe_tag(imei, s_imei, sizeof(s_imei));
    safe_tag(acc,  s_acc,  sizeof(s_acc));
    char line[512];
    snprintf(line, sizeof(line),
        "heartbeat,imei=%s battery=%.1f,gsm=%di,latitude=%.6f,longitude=%.6f,"
        "speed=%.1f,satellites=%di,acc=\"%s\",moving=%s,alarm=%s %lld",
        s_imei, battery, gsm, lat, lon, speed, sats, s_acc,
        moving ? "true" : "false",
        alarm  ? "true" : "false",
        now_ns());
    return influx_post(line);
}

int influx_write_wifi(const char *imei, const char *ssid, int client_count)
{
    if (!g_enabled) return -1;
    char s_imei[64], s_ssid[128];
    safe_tag(imei, s_imei, sizeof(s_imei));
    safe_tag(ssid, s_ssid, sizeof(s_ssid));
    char line[512];
    snprintf(line, sizeof(line),
        "wifi,imei=%s,ssid=%s clients=%di %lld",
        s_imei, s_ssid, client_count, now_ns());
    return influx_post(line);
}

int influx_write_rfid(const char *imei, const char *user_id,
    const char *station_id, int status)
{
    if (!g_enabled) return -1;
    char s_imei[64], s_uid[64], s_sid[64];
    safe_tag(imei,       s_imei, sizeof(s_imei));
    safe_tag(user_id,    s_uid,  sizeof(s_uid));
    safe_tag(station_id, s_sid,  sizeof(s_sid));
    char line[512];
    snprintf(line, sizeof(line),
        "rfid,imei=%s,user_id=%s,station_id=%s status=%di %lld",
        s_imei, s_uid, s_sid, status, now_ns());
    return influx_post(line);
}

int influx_write_login(const char *imei)
{
    if (!g_enabled) return -1;
    char s_imei[64];
    safe_tag(imei, s_imei, sizeof(s_imei));
    char line[256];
    snprintf(line, sizeof(line),
        "login,imei=%s online=1i %lld", s_imei, now_ns());
    return influx_post(line);
}

void influx_cleanup(void)
{
    if (g_headers) curl_slist_free_all(g_headers);
    if (g_curl) curl_easy_cleanup(g_curl);
    g_headers = NULL;
    g_curl = NULL;
    g_enabled = 0;
}
