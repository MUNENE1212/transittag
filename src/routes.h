#ifndef ROUTES_H
#define ROUTES_H

#define MAX_STOPS 32
#define STOP_NAME_LEN 64

typedef struct {
    int    id;
    char   name[STOP_NAME_LEN];
    double lat;
    double lon;
} route_stop_t;

int routes_init(void);
const route_stop_t *routes_get_stops(int *count);
int routes_get_distance_m(double origin_lat, double origin_lon, int stop_id);
int routes_distance_to_base_fare(int distance_m);
char *routes_stops_to_json(void);
void routes_cleanup(void);

#endif
