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

/* Replace the current stop list. Saves to stops.json. Returns 0 on success. */
int routes_set_stops(const char *route_name, const route_stop_t *stops, int count);

/* Get the current route name */
const char *routes_get_route_name(void);

/* Build a dropoff summary JSON from current seat states.
   seats_arr: array of seat_t, seat_count: number of seats.
   Returns malloc'd JSON string:
   {"type":"dropoffs","stops":[{"id":1,"name":"CBD","count":2,"seats":[3,7]},...]}
   Caller must free(). */
char *routes_dropoffs_to_json(void *seats_arr, int seat_count);

#endif
