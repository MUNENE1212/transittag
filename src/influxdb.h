#ifndef INFLUXDB_H
#define INFLUXDB_H

/* Initialize InfluxDB writer. Returns 0 on success, -1 if curl init fails. */
int influx_init(const char *url, const char *org, const char *bucket, const char *token);

/* Write a heartbeat measurement */
int influx_write_heartbeat(const char *imei, double battery, int gsm,
    double lat, double lon, double speed, int sats,
    const char *acc, int moving, int alarm);

/* Write a WiFi measurement */
int influx_write_wifi(const char *imei, const char *ssid, int client_count);

/* Write an RFID scan */
int influx_write_rfid(const char *imei, const char *user_id,
    const char *station_id, int status);

/* Write a login event */
int influx_write_login(const char *imei);

/* Cleanup curl resources */
void influx_cleanup(void);

#endif
