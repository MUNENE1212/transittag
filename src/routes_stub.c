/*
 * routes_stub.c — Stub when libcurl is not available.
 * All distance functions return -1 / NULL.
 */

#include "routes.h"
#include <stdio.h>

int routes_init(void)
{
    printf("[Routes] Install libcurl4-openssl-dev to enable distance pricing\n");
    return -1;
}

const route_stop_t *routes_get_stops(int *count)
{
    if (count) *count = 0;
    return NULL;
}

int routes_get_distance_m(double origin_lat, double origin_lon, int stop_id)
{
    (void)origin_lat; (void)origin_lon; (void)stop_id;
    return -1;
}

int routes_distance_to_base_fare(int distance_m)
{
    (void)distance_m;
    return -1;
}

char *routes_stops_to_json(void)
{
    return NULL;
}

void routes_cleanup(void)
{
    /* nothing */
}
