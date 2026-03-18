/*
 * seat_manager.h — TransitTag in-vehicle seat state manager
 *
 * Tracks up to MAX_SEATS physical seats: occupancy detected via weight
 * sensors, payment status updated by MQTT messages and WebSocket commands.
 *
 * Thread model: single-threaded (called only from the LWS event loop).
 */

#ifndef SEAT_MANAGER_H
#define SEAT_MANAGER_H

#include <time.h>

/* ── Limits ─────────────────────────────────────────────────────── */

#define MAX_SEATS            20
#define PAYMENT_TIMEOUT_SEC  90   /* Seconds after seating before overdue */

/* ── Seat status ────────────────────────────────────────────────── */

typedef enum {
    SEAT_EMPTY          = 0,  /* No passenger */
    SEAT_SEATED         = 1,  /* Passenger seated, payment pending */
    SEAT_PAYING         = 2,  /* STK Push sent, awaiting M-Pesa callback */
    SEAT_PAID_MPESA     = 3,  /* Paid via M-Pesa */
    SEAT_PAID_CASH      = 4,  /* Paid cash to conductor */
    SEAT_PAID_NEIGHBOUR = 5,  /* Third party paid for this seat */
    SEAT_OVERDUE        = 6   /* Seated but payment timeout elapsed */
} seat_status_t;

/* ── Seat record ────────────────────────────────────────────────── */

typedef struct {
    int            id;
    seat_status_t  status;
    double         weight_kg;    /* Latest reading from weight sensor */
    char           phone[16];    /* Passenger phone (9 digits, no prefix) */
    char           payer_phone[16]; /* Third-party payer phone if different */
    time_t         seated_at;    /* Epoch when passenger sat down */
    time_t         paid_at;      /* Epoch when payment confirmed */
    int            fare_kes;     /* Fare amount at time of seating */
    char           receipt[32];  /* TT-XXXXXX receipt code */
    char           payment_method[20]; /* "mpesa" | "cash" | "neighbour" */
    int            dest_stop_id; /* Destination stop ID, 0 if not set */
} seat_t;

/* ── Day summary (for conductor + owner dashboards) ─────────────── */

typedef struct {
    int total_passengers;
    int paid_mpesa;
    int paid_cash;
    int paid_neighbour;
    int evaders;
    int revenue_total;
    int revenue_mpesa;
    int revenue_cash;
    int trips;
} day_summary_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialise the seat array.  Must be called before any other function. */
void seat_manager_init(int seat_count);

/* Reset all seats to EMPTY (start of new trip). */
void seat_manager_reset_all(void);

/* Retrieve a single seat by 1-based ID.  Returns NULL if out of range. */
seat_t *seat_get(int seat_id);

/* Retrieve pointer to internal array; *count receives the seat count. */
seat_t *seat_get_all(int *count);

/* Called by weight-sensor MQTT handler.  Transitions EMPTY↔SEATED. */
int seat_set_weight(int seat_id, double weight_kg, int fare_kes);

/* Mark seat as PAYING (STK Push sent).  Stores passenger phone. */
int seat_set_paying(int seat_id, const char *phone);

/* Mark seat as PAID_MPESA.  Stores receipt code and optional payer phone. */
int seat_set_paid_mpesa(int seat_id, const char *receipt,
                        const char *payer_phone);

/* Mark seat as PAID_CASH. */
int seat_set_paid_cash(int seat_id);

/* Mark seat as OVERDUE (timeout triggered). */
int seat_set_overdue(int seat_id);

/* Reset seat to EMPTY (passenger left or conductor override). */
int seat_reset(int seat_id);

/* Check all SEATED seats for payment timeout; flip to OVERDUE as needed. */
void seat_check_timeouts(void);

/* Build a day summary across all seats. */
day_summary_t seat_get_day_summary(void);

/* ── Serialisation ──────────────────────────────────────────────── */

/* Human-readable status string ("empty", "seated", …). */
const char *seat_status_str(seat_status_t s);

/*
 * seat_to_json — caller must free() the returned string.
 * Format: {"id":1,"status":"seated","phone":"","receipt":"","fare":50}
 */
char *seat_to_json(seat_t *s);

/*
 * seats_state_to_json — full seats_state message, caller must free().
 * Format: {"type":"seats_state","seats":[…],"fare":50,"route":"…","peak":false}
 */
char *seats_state_to_json(int fare, const char *route, int peak);

/*
 * day_summary_to_json — full day_summary message, caller must free().
 * Includes GPS / vehicle telemetry fields.
 */
char *day_summary_to_json(day_summary_t *ds,
                           double lat, double lon,
                           double speed, int battery, int gsm);

#endif /* SEAT_MANAGER_H */
