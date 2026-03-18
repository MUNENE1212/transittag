/*
 * auth.c — TransitTag WebSocket connection authentication
 *
 * Constant-time PIN comparison, per-session attempt counting,
 * and exponential-style lockout after repeated failures.
 */

#include <string.h>
#include <time.h>
#include <stdio.h>
#include "auth.h"
#include "config.h"

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * ct_strcmp — constant-time string comparison.
 * Returns 0 if equal, non-zero otherwise.
 * Prevents timing-based PIN enumeration attacks.
 */
static int ct_strcmp(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    /* Compare full length of longest string to hide which position differs */
    size_t maxlen = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);  /* length mismatch flag */
    for (size_t i = 0; i < maxlen; i++) {
        unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
        diff |= (ca ^ cb);
    }
    return (int)diff;
}

static ws_role_t parse_role(const char *role_str)
{
    if (!role_str) return ROLE_NONE;
    if (strcmp(role_str, "conductor") == 0) return ROLE_CONDUCTOR;
    if (strcmp(role_str, "owner")     == 0) return ROLE_OWNER;
    if (strcmp(role_str, "driver")    == 0) return ROLE_DRIVER;
    if (strcmp(role_str, "passenger") == 0) return ROLE_PASSENGER;
    return ROLE_NONE;
}

/* ── Public API ───────────────────────────────────────────────────── */

void auth_init(ws_auth_t *a)
{
    memset(a, 0, sizeof(*a));
    a->role          = ROLE_NONE;
    a->authenticated = 0;
    a->pin_attempts  = 0;
    a->locked_until  = 0;
    a->last_attempt_time = 0;
}

int auth_attempt(ws_auth_t *a, const char *role_str, const char *pin)
{
    ws_role_t role = parse_role(role_str);
    if (role == ROLE_NONE) return -1;   /* Unknown role string */

    /* Roles that need no PIN — grant immediately */
    if (role == ROLE_PASSENGER || role == ROLE_DRIVER) {
        a->role          = role;
        a->authenticated = 1;
        return 1;
    }

    /* Lockout check */
    time_t now = time(NULL);
    if (a->locked_until > now) {
        printf("[Auth] Session locked for %d more seconds\n",
               (int)(a->locked_until - now));
        return 0;
    }

    /* Rate-limit consecutive attempts */
    if (a->last_attempt_time > 0 &&
        (now - a->last_attempt_time) < PIN_ATTEMPT_INTERVAL_SEC) {
        return 0;   /* Too fast — silent reject */
    }
    a->last_attempt_time = now;

    /* Determine expected PIN */
    const char *expected = NULL;
    if (role == ROLE_CONDUCTOR) expected = CONDUCTOR_PIN;
    else if (role == ROLE_OWNER) expected = OWNER_PIN;

    if (!expected || !pin) return 0;

    /* Constant-time comparison */
    int match = (ct_strcmp(pin, expected) == 0);

    if (match) {
        a->role          = role;
        a->authenticated = 1;
        a->pin_attempts  = 0;
        a->locked_until  = 0;
        printf("[Auth] Authenticated: role=%s\n", auth_role_str(role));
        return 1;
    }

    /* Failed attempt */
    a->pin_attempts++;
    printf("[Auth] Failed PIN attempt %d/%d for role=%s\n",
           a->pin_attempts, PIN_MAX_ATTEMPTS, auth_role_str(role));

    if (a->pin_attempts >= PIN_MAX_ATTEMPTS) {
        a->locked_until = now + PIN_LOCKOUT_SEC;
        printf("[Auth] Session locked for %d seconds after %d failures\n",
               PIN_LOCKOUT_SEC, PIN_MAX_ATTEMPTS);
    }

    return 0;
}

int auth_require_role(const ws_auth_t *a, ws_role_t min_role)
{
    if (!a->authenticated) return 0;
    /* Owner can do anything */
    if (a->role == ROLE_OWNER) return 1;
    /* Conductor can do conductor-level and below */
    if (min_role <= ROLE_CONDUCTOR && a->role == ROLE_CONDUCTOR) return 1;
    /* Driver and passenger are only allowed their own level */
    if (a->role >= min_role) return 1;
    return 0;
}

const char *auth_role_str(ws_role_t r)
{
    switch (r) {
    case ROLE_PASSENGER:  return "passenger";
    case ROLE_DRIVER:     return "driver";
    case ROLE_CONDUCTOR:  return "conductor";
    case ROLE_OWNER:      return "owner";
    default:              return "none";
    }
}

int auth_lockout_remaining(const ws_auth_t *a)
{
    time_t now = time(NULL);
    if (a->locked_until <= now) return 0;
    return (int)(a->locked_until - now);
}
