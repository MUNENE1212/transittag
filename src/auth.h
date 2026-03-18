/*
 * auth.h — TransitTag per-WebSocket-connection authentication state
 *
 * Each WS client gets one ws_auth_t (stored as libwebsockets per-session
 * data).  Privileged commands (conductor/owner) are rejected unless the
 * client has successfully authenticated via the "auth" action.
 *
 * Design principles:
 *   - PIN comparison uses constant-time memcmp to prevent timing attacks
 *   - Failed attempts are counted; after PIN_MAX_ATTEMPTS the session is
 *     locked for PIN_LOCKOUT_SEC seconds
 *   - Role is stored server-side after successful auth; the JS client
 *     cannot forge it via a subsequent message
 */

#ifndef TRANSITTAG_AUTH_H
#define TRANSITTAG_AUTH_H

#include <time.h>
#include "config.h"

/* ── Role identifiers ─────────────────────────────────────────────── */

typedef enum {
    ROLE_NONE       = 0,   /* Not yet authenticated */
    ROLE_PASSENGER  = 1,   /* No PIN needed */
    ROLE_DRIVER     = 2,   /* No PIN needed */
    ROLE_CONDUCTOR  = 3,   /* PIN required */
    ROLE_OWNER      = 4,   /* PIN required */
} ws_role_t;

/* ── Per-connection auth state (stored in LWS per-session data) ───── */

typedef struct {
    ws_role_t  role;                       /* Granted role after auth     */
    int        authenticated;              /* 1 if PIN accepted           */
    int        pin_attempts;              /* Consecutive failures         */
    time_t     locked_until;             /* Epoch; 0 = not locked        */
    time_t     last_attempt_time;        /* Rate-limit consecutive tries */
} ws_auth_t;

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * auth_init — initialise a fresh auth state for a new connection.
 * Must be called in LWS_CALLBACK_ESTABLISHED.
 */
void auth_init(ws_auth_t *a);

/*
 * auth_attempt — try to authenticate with the given role name and PIN.
 *
 * Returns:
 *   1  — success; a->role and a->authenticated are updated
 *   0  — wrong PIN or locked
 *  -1  — role string unknown
 *
 * Side-effects: increments pin_attempts on failure; sets locked_until
 * after PIN_MAX_ATTEMPTS failures; enforces PIN_ATTEMPT_INTERVAL_SEC
 * between calls.
 */
int auth_attempt(ws_auth_t *a, const char *role, const char *pin);

/*
 * auth_require_role — check that the connection holds at least the
 * required role.  Returns 1 if allowed, 0 if denied.
 *
 * Hierarchy: OWNER > CONDUCTOR > DRIVER = PASSENGER > NONE
 * (owner can do anything conductor can)
 */
int auth_require_role(const ws_auth_t *a, ws_role_t min_role);

/* Human-readable role name for logging. */
const char *auth_role_str(ws_role_t r);

/* Return seconds remaining in lockout, 0 if not locked. */
int auth_lockout_remaining(const ws_auth_t *a);

#endif /* TRANSITTAG_AUTH_H */
