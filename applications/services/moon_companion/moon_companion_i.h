#pragma once

#include "moon_companion.h"
#include "moon_companion_ble.h"
#include <furi.h>
#include <furi_ble/l2cap_coc.h>

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

/* ── Bulk transfer (Phase 3) ──────────────────────────────────────── */

/* Event flags used on bulk_session->event for signalling between the
 * BT-thread CoC callback and the caller thread that's blocking in
 * moon_companion_bulk_open_blocking. */
#define MOON_BULK_EVT_OPENED        (1u << 0)  /* OpenBulkChannelResponse arrived */
#define MOON_BULK_EVT_COC_CONNECTED (1u << 1)  /* CoC channel established */
#define MOON_BULK_EVT_COC_DONE      (1u << 2)  /* CoC disconnected (clean EOF) */
#define MOON_BULK_EVT_COC_ERROR     (1u << 3)  /* CoC error or open failure */

/* CoC parameters. ble_l2cap_coc_connect accepts 0x0080–0x00FF for
 * custom SPSMs, but the phone picks the actual number at listen time
 * and returns it in OpenBulkChannelResponse — we just pass it through
 * verbatim. MTU/MPS/credits follow the defaults the wrapper exposes. */
#define MOON_BULK_MTU      BLE_L2CAP_COC_MTU_DEFAULT
#define MOON_BULK_MPS      BLE_L2CAP_COC_MPS_MAX
#define MOON_BULK_CREDITS  BLE_L2CAP_COC_CREDITS_DEFAULT

typedef struct {
    MoonBulkKind kind;
    uint32_t expected_bytes;   /* from OpenBulkChannelResponse.total_bytes */
    uint32_t bytes_received;   /* running count, mutex-protected */
    uint8_t  session_id[8];
    uint16_t spsm;
    uint8_t  coc_channel_index;
    bool     coc_channel_valid;
    uint16_t error_code;       /* L2CAP reject / error code, 0 on success */
    FuriEventFlag* event;

    /* Caller-supplied data pump. Invoked from the BT thread. */
    MoonBulkDataCallback on_data;
    void* on_data_ctx;
} MoonBulkSession;

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

    /* Currently active bulk transfer, or NULL. Guarded by `mutex`.
     * Only one transfer in flight at a time for Phase 3 — concurrent
     * transfers would need per-channel book-keeping in the CoC callback
     * which isn't worth the complexity before we've proven the basic
     * path. */
    MoonBulkSession* active_bulk;
};

#define MOON_COMPANION_RECONNECT_DELAY_MS 5000
