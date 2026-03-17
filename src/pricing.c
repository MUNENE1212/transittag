/*
 * pricing.c — TransitTag fare pricing engine
 *
 * Defaults: CBD → Westlands, KES 50 base, 1.5× peak multiplier.
 */

#include "pricing.h"
#include "routes.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Internal state ─────────────────────────────────────────────── */

static pricing_config_t g_config;

/* ── Init ───────────────────────────────────────────────────────── */

void pricing_init(void)
{
    memset(&g_config, 0, sizeof(g_config));

    strncpy(g_config.route, "CBD \xe2\x86\x92 Westlands",
            sizeof(g_config.route) - 1);    /* UTF-8 → arrow */

    g_config.base_fare       = 50;
    g_config.peak_multiplier = 1.5f;
    g_config.peak1_start     = 7;
    g_config.peak1_end       = 9;
    g_config.peak2_start     = 17;
    g_config.peak2_end       = 19;
}

/* ── Setters ────────────────────────────────────────────────────── */

void pricing_set(const char *route, int base_fare)
{
    if (route && route[0]) {
        strncpy(g_config.route, route, sizeof(g_config.route) - 1);
        g_config.route[sizeof(g_config.route) - 1] = '\0';
    }
    if (base_fare > 0) {
        g_config.base_fare = base_fare;
    }
}

/* ── Peak detection ─────────────────────────────────────────────── */

int pricing_is_peak(void)
{
    time_t     now = time(NULL);
    struct tm *lt  = localtime(&now);
    int        h   = lt->tm_hour;

    /* Morning peak: [peak1_start, peak1_end) */
    if (h >= g_config.peak1_start && h < g_config.peak1_end) return 1;

    /* Evening peak: [peak2_start, peak2_end) */
    if (h >= g_config.peak2_start && h < g_config.peak2_end) return 1;

    return 0;
}

/* ── Fare calculation ───────────────────────────────────────────── */

int pricing_get_fare(void)
{
    if (pricing_is_peak()) {
        /* Integer rounding: multiply then divide to stay in KES */
        return (int)(g_config.base_fare * g_config.peak_multiplier + 0.5f);
    }
    return g_config.base_fare;
}

/* ── Getters ────────────────────────────────────────────────────── */

const char *pricing_get_route(void)
{
    return g_config.route;
}

pricing_config_t *pricing_get_config(void)
{
    return &g_config;
}

/* ── Distance-based pricing ─────────────────────────────────────── */

void pricing_set_distance(int stop_id, int distance_m, const char *stop_name)
{
    g_config.dest_stop_id = stop_id;
    g_config.distance_m   = distance_m;
    if (stop_name) {
        strncpy(g_config.dest_stop_name, stop_name, STOP_NAME_LEN - 1);
        g_config.dest_stop_name[STOP_NAME_LEN - 1] = '\0';
    }
    g_config.base_fare = routes_distance_to_base_fare(distance_m);
}

int pricing_is_distance_mode(void)
{
    return g_config.distance_m > 0;
}
