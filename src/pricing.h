/*
 * pricing.h — TransitTag fare pricing engine
 *
 * Manages route, base fare, and peak-hour multiplier.
 * Peak hours: 07:00–09:00 and 17:00–19:00 (Nairobi rush hours).
 */

#ifndef PRICING_H
#define PRICING_H

#include "routes.h"

/* ── Configuration record ───────────────────────────────────────── */

typedef struct {
    char  route[128];
    int   base_fare;           /* KES */
    float peak_multiplier;     /* e.g. 1.5 */
    int   peak1_start;         /* Hour (24h), default 7  */
    int   peak1_end;           /* Hour (24h), default 9  */
    int   peak2_start;         /* Hour (24h), default 17 */
    int   peak2_end;           /* Hour (24h), default 19 */
    int   distance_m;          /* Road distance to destination (metres), 0 = flat-rate */
    int   dest_stop_id;        /* Destination stop ID, -1 if not set */
    char  dest_stop_name[STOP_NAME_LEN]; /* Destination stop name */
} pricing_config_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialise with defaults.  Must be called before other functions. */
void pricing_init(void);

/* Update route and base fare (e.g. from conductor WebSocket command). */
void pricing_set(const char *route, int base_fare);

/*
 * Return current effective fare.
 * Returns base_fare * peak_multiplier during peak hours, base_fare otherwise.
 */
int pricing_get_fare(void);

/* Return 1 if the current local time falls in a peak window, else 0. */
int pricing_is_peak(void);

/* Return the current route string. */
const char *pricing_get_route(void);

/* Return a pointer to the full config (read-only, do not free). */
pricing_config_t *pricing_get_config(void);

/*
 * Set distance-based pricing for the current passenger.
 * Updates base_fare via routes_distance_to_base_fare().
 */
void pricing_set_distance(int stop_id, int distance_m, const char *stop_name);

/* Return 1 if distance-based mode is active (distance_m > 0). */
int  pricing_is_distance_mode(void);

#endif /* PRICING_H */
