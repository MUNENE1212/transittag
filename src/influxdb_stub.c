/* Stub — InfluxDB disabled (libcurl not found) */
#include "influxdb.h"
#include <stdio.h>

int influx_init(const char *url, const char *org, const char *bucket, const char *token)
{
    (void)url; (void)org; (void)bucket; (void)token;
    printf("[InfluxDB] Stub — install libcurl4-openssl-dev to enable\n");
    return -1;
}

int influx_write_heartbeat(const char *imei, double battery, int gsm,
    double lat, double lon, double speed, int sats,
    const char *acc, int moving, int alarm)
{
    (void)imei; (void)battery; (void)gsm; (void)lat; (void)lon;
    (void)speed; (void)sats; (void)acc; (void)moving; (void)alarm;
    return -1;
}

int influx_write_wifi(const char *imei, const char *ssid, int client_count)
{
    (void)imei; (void)ssid; (void)client_count;
    return -1;
}

int influx_write_rfid(const char *imei, const char *user_id,
    const char *station_id, int status)
{
    (void)imei; (void)user_id; (void)station_id; (void)status;
    return -1;
}

int influx_write_login(const char *imei)
{
    (void)imei;
    return -1;
}

void influx_cleanup(void) {}
