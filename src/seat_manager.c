/*
 * seat_manager.c — TransitTag in-vehicle seat state manager
 *
 * All state lives in a static array; no heap except for JSON output
 * strings (caller must free).  Receipt codes use a global counter so
 * they are unique for the lifetime of the process.
 */

#include "seat_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Weight thresholds ──────────────────────────────────────────── */

#define WEIGHT_SEATED_KG  5.0   /* Above this → passenger present */
#define WEIGHT_EMPTY_KG   2.0   /* Below this → seat vacated */

/* ── Internal state ─────────────────────────────────────────────── */

static seat_t   g_seats[MAX_SEATS];
static int      g_seat_count  = 0;
static int      g_receipt_ctr = 0;   /* Monotonic receipt counter */

/* Day-level aggregate totals (accumulated across resets) */
static int      g_day_trips   = 0;

/* ── Init ───────────────────────────────────────────────────────── */

void seat_manager_init(int seat_count)
{
    if (seat_count < 1)  seat_count = 1;
    if (seat_count > MAX_SEATS) seat_count = MAX_SEATS;

    g_seat_count = seat_count;
    memset(g_seats, 0, sizeof(g_seats));

    for (int i = 0; i < seat_count; i++) {
        g_seats[i].id     = i + 1;   /* 1-based IDs */
        g_seats[i].status = SEAT_EMPTY;
    }
}

/* ── Reset ──────────────────────────────────────────────────────── */

void seat_manager_reset_all(void)
{
    g_day_trips++;
    for (int i = 0; i < g_seat_count; i++) {
        int id = g_seats[i].id;
        memset(&g_seats[i], 0, sizeof(seat_t));
        g_seats[i].id     = id;
        g_seats[i].status = SEAT_EMPTY;
    }
}

/* ── Accessors ──────────────────────────────────────────────────── */

seat_t *seat_get(int seat_id)
{
    if (seat_id < 1 || seat_id > g_seat_count) return NULL;
    return &g_seats[seat_id - 1];
}

seat_t *seat_get_all(int *count)
{
    if (count) *count = g_seat_count;
    return g_seats;
}

/* ── Transitions ────────────────────────────────────────────────── */

int seat_set_weight(int seat_id, double weight_kg, int fare_kes)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    s->weight_kg = weight_kg;

    if (s->status == SEAT_EMPTY && weight_kg >= WEIGHT_SEATED_KG) {
        /* Passenger just sat down */
        s->status    = SEAT_SEATED;
        s->seated_at = time(NULL);
        s->fare_kes  = fare_kes;
        s->phone[0]  = '\0';
        s->receipt[0] = '\0';
        s->payer_phone[0] = '\0';
        s->payment_method[0] = '\0';
    } else if ((s->status == SEAT_SEATED || s->status == SEAT_PAYING ||
                s->status == SEAT_OVERDUE)
               && weight_kg < WEIGHT_EMPTY_KG) {
        /* Passenger left without paying */
        s->status = SEAT_EMPTY;
    }
    /* Paid seats are not affected by weight changes until next trip reset */

    return 0;
}

int seat_set_paying(int seat_id, const char *phone)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    s->status = SEAT_PAYING;
    if (phone && phone[0]) {
        strncpy(s->phone, phone, sizeof(s->phone) - 1);
        s->phone[sizeof(s->phone) - 1] = '\0';
    }
    return 0;
}

int seat_set_paid_mpesa(int seat_id, const char *receipt,
                        const char *payer_phone)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    s->status  = SEAT_PAID_MPESA;
    s->paid_at = time(NULL);
    strncpy(s->payment_method, "mpesa", sizeof(s->payment_method) - 1);

    if (receipt && receipt[0]) {
        strncpy(s->receipt, receipt, sizeof(s->receipt) - 1);
        s->receipt[sizeof(s->receipt) - 1] = '\0';
    } else {
        /* Auto-generate receipt code */
        snprintf(s->receipt, sizeof(s->receipt), "TT-%06d", ++g_receipt_ctr);
    }

    if (payer_phone && payer_phone[0]) {
        /* Third-party payment — different payer */
        strncpy(s->payer_phone, payer_phone, sizeof(s->payer_phone) - 1);
        s->payer_phone[sizeof(s->payer_phone) - 1] = '\0';
        s->status = SEAT_PAID_NEIGHBOUR;
        strncpy(s->payment_method, "neighbour", sizeof(s->payment_method) - 1);
    }

    return 0;
}

int seat_set_paid_cash(int seat_id)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    s->status  = SEAT_PAID_CASH;
    s->paid_at = time(NULL);

    snprintf(s->receipt, sizeof(s->receipt), "TT-%06d", ++g_receipt_ctr);
    strncpy(s->payment_method, "cash", sizeof(s->payment_method) - 1);

    return 0;
}

int seat_set_overdue(int seat_id)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    /* Only mark SEATED seats overdue; PAYING might still succeed */
    if (s->status == SEAT_SEATED) {
        s->status = SEAT_OVERDUE;
    }
    return 0;
}

int seat_reset(int seat_id)
{
    seat_t *s = seat_get(seat_id);
    if (!s) return -1;

    int id = s->id;
    memset(s, 0, sizeof(seat_t));
    s->id     = id;
    s->status = SEAT_EMPTY;
    return 0;
}

/* ── Timeout checker (call every ~10 seconds from event loop) ───── */

void seat_check_timeouts(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_seat_count; i++) {
        seat_t *s = &g_seats[i];
        if (s->status == SEAT_SEATED && s->seated_at > 0) {
            if ((now - s->seated_at) >= PAYMENT_TIMEOUT_SEC) {
                s->status = SEAT_OVERDUE;
            }
        }
    }
}

/* ── Day summary ────────────────────────────────────────────────── */

day_summary_t seat_get_day_summary(void)
{
    day_summary_t ds;
    memset(&ds, 0, sizeof(ds));

    ds.trips = g_day_trips;

    for (int i = 0; i < g_seat_count; i++) {
        seat_t *s = &g_seats[i];
        if (s->status == SEAT_EMPTY) continue;

        ds.total_passengers++;

        if (s->status == SEAT_PAID_MPESA) {
            ds.paid_mpesa++;
            ds.revenue_mpesa  += s->fare_kes;
            ds.revenue_total  += s->fare_kes;
        } else if (s->status == SEAT_PAID_CASH) {
            ds.paid_cash++;
            ds.revenue_cash   += s->fare_kes;
            ds.revenue_total  += s->fare_kes;
        } else if (s->status == SEAT_PAID_NEIGHBOUR) {
            ds.paid_neighbour++;
            ds.revenue_mpesa  += s->fare_kes;  /* neighbour pays via M-Pesa */
            ds.revenue_total  += s->fare_kes;
        } else if (s->status == SEAT_OVERDUE) {
            ds.evaders++;
        }
        /* SEATED and PAYING are in-progress — not yet counted either way */
    }

    return ds;
}

/* ── Status string ──────────────────────────────────────────────── */

const char *seat_status_str(seat_status_t s)
{
    switch (s) {
    case SEAT_EMPTY:          return "empty";
    case SEAT_SEATED:         return "seated";
    case SEAT_PAYING:         return "paying";
    case SEAT_PAID_MPESA:     return "paid_mpesa";
    case SEAT_PAID_CASH:      return "paid_cash";
    case SEAT_PAID_NEIGHBOUR: return "paid_neighbour";
    case SEAT_OVERDUE:        return "overdue";
    default:                  return "unknown";
    }
}

/* ── JSON helpers ───────────────────────────────────────────────── */

/*
 * Append a JSON-safe escaped string.  Returns new pointer past written bytes.
 * Caller must ensure buf is large enough.
 */
static char *json_str(char *buf, const char *s) __attribute__((unused));
static char *json_str(char *buf, const char *s)
{
    *buf++ = '"';
    while (*s) {
        if (*s == '"' || *s == '\\') *buf++ = '\\';
        *buf++ = *s++;
    }
    *buf++ = '"';
    return buf;
}

char *seat_to_json(seat_t *s)
{
    if (!s) return NULL;

    /* Conservative upper bound for one seat record */
    char *buf = malloc(256);
    if (!buf) return NULL;

    snprintf(buf, 256,
        "{\"id\":%d,\"status\":\"%s\",\"phone\":\"%s\","
        "\"receipt\":\"%s\",\"fare\":%d,\"paidBy\":\"%s\"}",
        s->id,
        seat_status_str(s->status),
        s->phone,
        s->receipt,
        s->fare_kes,
        s->payer_phone[0] ? s->payer_phone : s->phone);

    return buf;
}

char *seats_state_to_json(int fare, const char *route, int peak)
{
    if (!route) route = "";

    /* Each seat ~256 bytes; overhead ~128 for wrapper */
    size_t buflen = (size_t)g_seat_count * 256 + 512;
    char  *buf    = malloc(buflen);
    if (!buf) return NULL;

    /* Build seats array */
    size_t off = 0;
    off += (size_t)snprintf(buf + off, buflen - off,
        "{\"type\":\"seats_state\",\"seats\":[");

    for (int i = 0; i < g_seat_count && off < buflen - 2; i++) {
        char *sj = seat_to_json(&g_seats[i]);
        if (sj) {
            size_t slen = strlen(sj);
            if (off + slen + 2 < buflen) {
                memcpy(buf + off, sj, slen);
                off += slen;
                if (i < g_seat_count - 1) buf[off++] = ',';
            }
            free(sj);
        }
    }

    /* Build route JSON-escaped inline */
    char route_escaped[300];
    {
        char *p = route_escaped;
        const char *r = route;
        while (*r && (p - route_escaped) < (int)(sizeof(route_escaped) - 4)) {
            if (*r == '"' || *r == '\\') *p++ = '\\';
            *p++ = *r++;
        }
        *p = '\0';
    }

    off += (size_t)snprintf(buf + off, buflen - off,
        "],\"fare\":%d,\"route\":\"%s\",\"peak\":%s}",
        fare, route_escaped, peak ? "true" : "false");

    return buf;
}

char *day_summary_to_json(day_summary_t *ds,
                           double lat, double lon,
                           double speed, int battery, int gsm)
{
    if (!ds) return NULL;

    char *buf = malloc(512);
    if (!buf) return NULL;

    snprintf(buf, 512,
        "{"
        "\"type\":\"day_summary\","
        "\"total_passengers\":%d,"
        "\"paid_mpesa\":%d,"
        "\"paid_cash\":%d,"
        "\"paid_neighbour\":%d,"
        "\"evaders\":%d,"
        "\"revenue_total\":%d,"
        "\"revenue_mpesa\":%d,"
        "\"revenue_cash\":%d,"
        "\"trips\":%d,"
        "\"lat\":%.6f,"
        "\"lon\":%.6f,"
        "\"speed\":%.1f,"
        "\"battery\":%d,"
        "\"gsm\":%d"
        "}",
        ds->total_passengers,
        ds->paid_mpesa,
        ds->paid_cash,
        ds->paid_neighbour,
        ds->evaders,
        ds->revenue_total,
        ds->revenue_mpesa,
        ds->revenue_cash,
        ds->trips,
        lat,
        lon,
        speed,
        battery,
        gsm);

    return buf;
}
