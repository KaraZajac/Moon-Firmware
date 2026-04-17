#pragma once

#include "moon_companion.h"
#include <furi.h>

#define MOON_COMPANION_MAX_SUBSCRIBERS 4
#define MOON_COMPANION_AUTH_TOKEN_SIZE 16
#define MOON_COMPANION_SETTINGS_PATH   "/int/.moon_companion.settings"

typedef struct {
    MoonPositionCallback cb;
    void* ctx;
} MoonPositionSubscriber;

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

    /* Connection / service state */
    MoonConnectionState state;

    /* Persisted pairing */
    MoonCompanionPersist persist;

    /* Pairing-in-progress */
    bool pairing_active;
    char pairing_pin[7]; /* 6 digits + NUL */

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
};
