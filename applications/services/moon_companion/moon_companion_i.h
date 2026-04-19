#pragma once

#include "moon_companion.h"
#include "moon_companion_ble.h"
#include <furi.h>

#define MOON_COMPANION_MAX_SUBSCRIBERS 4
#define MOON_COMPANION_AUTH_TOKEN_SIZE 16
#define MOON_COMPANION_MAX_INFLIGHT    4
#define MOON_COMPANION_SETTINGS_PATH   "/int/.moon_companion.settings"
#define MOON_COMPANION_RPC_TIMEOUT_MS  3000

typedef struct {
    MoonPositionCallback cb;
    void* ctx;
} MoonPositionSubscriber;

/* An outstanding RPC — keyed by request_id. Waiters block on
 * `done` (FuriEventFlag) until the matching response notification
 * lands, times out, or the connection drops. `response_buf` is
 * the decoded MoonResponse payload; only the `status` and the
 * relevant oneof branch are valid. */
typedef struct {
    uint32_t request_id;
    FuriEventFlag* done;
    void* response_buf; /* heap-allocated moon_companion_v1_MoonResponse* */
    bool filled;
    bool timed_out;
} MoonRpcInFlight;

typedef struct {
    bool    paired;
    uint8_t auth_token[MOON_COMPANION_AUTH_TOKEN_SIZE];
    /* Last-seen phone BLE MAC and address type — helps re-connect
     * quickly after an RPA rotation by writing the new one in when a
     * post-pair advertisement matches our auth_token. */
    uint8_t phone_mac[6];
    uint8_t phone_addr_type;
} MoonCompanionPersist;

struct MoonCompanion {
    FuriThread* thread;
    FuriMessageQueue* queue;
    FuriMutex* mutex;

    /* BLE transport */
    MoonBle* ble;

    /* Connection / service state */
    MoonConnectionState state;

    /* Persisted pairing */
    MoonCompanionPersist persist;

    /* Pairing-in-progress */
    bool pairing_active;
    char pairing_pin[7]; /* 6 digits + NUL */

    /* RPC bookkeeping */
    uint32_t next_request_id;
    MoonRpcInFlight inflight[MOON_COMPANION_MAX_INFLIGHT];

    /* Last-known values (mutex-protected) */
    MoonPosition last_position;
    bool last_position_valid;
    MoonTime last_time;
    bool last_time_valid;

    /* Push subscribers */
    MoonPositionSubscriber pos_subs[MOON_COMPANION_MAX_SUBSCRIBERS];
    uint8_t pos_sub_count;

    /* Yield */
    bool yielded;
    uint32_t yield_until_tick;

    /* One-shot timer that posts MoonMsgReconnect a few seconds after the
     * BLE link transitions to Idle/Error, so the service retries scan
     * without us hammering the radio when the phone is just briefly out
     * of range. */
    FuriTimer* reconnect_timer;
};

#define MOON_COMPANION_RECONNECT_DELAY_MS 5000
