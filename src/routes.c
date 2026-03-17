/*
 * routes.c — Google Maps Distance Matrix integration for TransitTag
 *
 * Loads route stops from stops.json, queries the Google Distance Matrix
 * API to compute road distances, and converts distances to fare tiers.
 */

#include "routes.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* ── Static state ─────────────────────────────────────────────── */

static CURL          *g_curl          = NULL;
static char           g_api_key[128]  = {0};
static route_stop_t   g_stops[MAX_STOPS];
static int            g_stop_count    = 0;
static int            g_enabled       = 0;

/* Response accumulation buffer */
static char   g_response_buf[8192];
static size_t g_response_len = 0;

/* ── cURL write callback ───────────────────────────────────────── */

static size_t routes_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)userdata;
    size_t incoming = size * nmemb;
    size_t space    = sizeof(g_response_buf) - g_response_len - 1;
    if (incoming > space) incoming = space;
    memcpy(g_response_buf + g_response_len, ptr, incoming);
    g_response_len += incoming;
    g_response_buf[g_response_len] = '\0';
    return size * nmemb; /* always claim full consumption */
}

/* ── Init ─────────────────────────────────────────────────────── */

int routes_init(void)
{
    const char *key = getenv("GMAPS_API_KEY");
    if (!key || key[0] == '\0') {
        printf("[Routes] GMAPS_API_KEY not set — distance pricing disabled, using flat-rate\n");
        return -1;
    }
    strncpy(g_api_key, key, sizeof(g_api_key) - 1);
    g_api_key[sizeof(g_api_key) - 1] = '\0';

    /* Load stops from stops.json */
    char path[512];
    snprintf(path, sizeof(path), "%s/stops.json", WWW_DIR);

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[Routes] Cannot open %s — distance pricing disabled, using flat-rate\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize <= 0 || fsize > 65536) {
        printf("[Routes] stops.json too large or empty\n");
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        printf("[Routes] Failed to parse stops.json\n");
        return -1;
    }

    cJSON *stops_arr = cJSON_GetObjectItem(root, "stops");
    if (!stops_arr || !cJSON_IsArray(stops_arr)) {
        printf("[Routes] stops.json missing 'stops' array\n");
        cJSON_Delete(root);
        return -1;
    }

    g_stop_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, stops_arr) {
        if (g_stop_count >= MAX_STOPS) break;

        cJSON *jid   = cJSON_GetObjectItem(item, "id");
        cJSON *jname = cJSON_GetObjectItem(item, "name");
        cJSON *jlat  = cJSON_GetObjectItem(item, "lat");
        cJSON *jlon  = cJSON_GetObjectItem(item, "lon");

        if (!jid || !jname || !jlat || !jlon) continue;

        g_stops[g_stop_count].id  = (int)jid->valuedouble;
        strncpy(g_stops[g_stop_count].name, jname->valuestring,
                STOP_NAME_LEN - 1);
        g_stops[g_stop_count].name[STOP_NAME_LEN - 1] = '\0';
        g_stops[g_stop_count].lat = jlat->valuedouble;
        g_stops[g_stop_count].lon = jlon->valuedouble;
        g_stop_count++;
    }
    cJSON_Delete(root);

    if (g_stop_count == 0) {
        printf("[Routes] No stops loaded from stops.json\n");
        return -1;
    }

    /* Initialise curl */
    g_curl = curl_easy_init();
    if (!g_curl) {
        fprintf(stderr, "[Routes] curl_easy_init failed\n");
        return -1;
    }

    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, routes_write_cb);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT_MS,    2000L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);

    g_enabled = 1;
    printf("[Routes] Initialized — %d stops loaded, Google Maps distance pricing enabled\n",
           g_stop_count);
    return 0;
}

/* ── Getters ──────────────────────────────────────────────────── */

const route_stop_t *routes_get_stops(int *count)
{
    if (count) *count = g_stop_count;
    return g_stops;
}

/* ── Distance lookup ──────────────────────────────────────────── */

int routes_get_distance_m(double origin_lat, double origin_lon, int stop_id)
{
    if (!g_enabled || !g_curl) return -1;

    /* Find destination stop */
    const route_stop_t *dest = NULL;
    for (int i = 0; i < g_stop_count; i++) {
        if (g_stops[i].id == stop_id) {
            dest = &g_stops[i];
            break;
        }
    }
    if (!dest) return -1;

    /* Build URL */
    char url[512];
    snprintf(url, sizeof(url),
        "https://maps.googleapis.com/maps/api/distancematrix/json"
        "?origins=%.6f%%2C%.6f"
        "&destinations=%.6f%%2C%.6f"
        "&mode=driving"
        "&key=%s",
        origin_lat, origin_lon,
        dest->lat,  dest->lon,
        g_api_key);

    /* Reset response buffer */
    g_response_len = 0;
    g_response_buf[0] = '\0';

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPGET, 1L);

    CURLcode res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[Routes] Distance API request failed: %s\n",
                curl_easy_strerror(res));
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "[Routes] Distance API HTTP %ld\n", http_code);
        return -1;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(g_response_buf);
    if (!root) {
        fprintf(stderr, "[Routes] Failed to parse Distance API response\n");
        return -1;
    }

    int distance_m = -1;

    cJSON *rows = cJSON_GetObjectItem(root, "rows");
    if (rows && cJSON_IsArray(rows)) {
        cJSON *row0 = cJSON_GetArrayItem(rows, 0);
        if (row0) {
            cJSON *elements = cJSON_GetObjectItem(row0, "elements");
            if (elements && cJSON_IsArray(elements)) {
                cJSON *elem0 = cJSON_GetArrayItem(elements, 0);
                if (elem0) {
                    cJSON *dist_obj = cJSON_GetObjectItem(elem0, "distance");
                    if (dist_obj) {
                        cJSON *val = cJSON_GetObjectItem(dist_obj, "value");
                        if (val && cJSON_IsNumber(val)) {
                            distance_m = (int)val->valuedouble;
                        }
                    }
                }
            }
        }
    }

    cJSON_Delete(root);

    if (distance_m < 0) {
        fprintf(stderr, "[Routes] Distance value not found in API response\n");
    }

    return distance_m;
}

/* ── Fare tiers ───────────────────────────────────────────────── */

int routes_distance_to_base_fare(int distance_m)
{
    if (distance_m <  2000) return 30;
    if (distance_m <  5000) return 50;
    if (distance_m < 10000) return 70;
    if (distance_m < 20000) return 100;
    return 130;
}

/* ── JSON serialiser ──────────────────────────────────────────── */

char *routes_stops_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "type", "stops_list");
    cJSON *arr = cJSON_AddArrayToObject(root, "stops");

    for (int i = 0; i < g_stop_count; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "id",   g_stops[i].id);
        cJSON_AddStringToObject(s, "name", g_stops[i].name);
        cJSON_AddNumberToObject(s, "lat",  g_stops[i].lat);
        cJSON_AddNumberToObject(s, "lon",  g_stops[i].lon);
        cJSON_AddItemToArray(arr, s);
    }

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}

/* ── Cleanup ──────────────────────────────────────────────────── */

void routes_cleanup(void)
{
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    g_enabled    = 0;
    g_stop_count = 0;
}
