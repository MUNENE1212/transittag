#include "influxdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

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
    char line[512];
    snprintf(line, sizeof(line),
        "heartbeat,imei=%s battery=%.1f,gsm=%di,latitude=%.6f,longitude=%.6f,"
        "speed=%.1f,satellites=%di,acc=\"%s\",moving=%s,alarm=%s %lld",
        imei ? imei : "unknown",
        battery, gsm, lat, lon, speed, sats,
        acc ? acc : "OFF",
        moving ? "true" : "false",
        alarm ? "true" : "false",
        now_ns());
    return influx_post(line);
}

int influx_write_wifi(const char *imei, const char *ssid, int client_count)
{
    if (!g_enabled) return -1;
    char line[512];
    snprintf(line, sizeof(line),
        "wifi,imei=%s,ssid=%s clients=%di %lld",
        imei ? imei : "unknown",
        ssid ? ssid : "unknown",
        client_count, now_ns());
    return influx_post(line);
}

int influx_write_rfid(const char *imei, const char *user_id,
    const char *station_id, int status)
{
    if (!g_enabled) return -1;
    char line[512];
    snprintf(line, sizeof(line),
        "rfid,imei=%s,user_id=%s,station_id=%s status=%di %lld",
        imei ? imei : "unknown",
        user_id ? user_id : "unknown",
        station_id ? station_id : "unknown",
        status, now_ns());
    return influx_post(line);
}

int influx_write_login(const char *imei)
{
    if (!g_enabled) return -1;
    char line[256];
    snprintf(line, sizeof(line),
        "login,imei=%s online=1i %lld",
        imei ? imei : "unknown", now_ns());
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
