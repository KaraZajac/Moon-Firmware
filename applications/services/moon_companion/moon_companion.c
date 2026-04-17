#include "moon_companion.h"
#include "moon_companion_i.h"

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "MoonCompanion"

/* ── Internal message plumbing ─────────────────────────────────────── */

typedef enum {
    MoonMsgStop,
    MoonMsgBeginPairing,
    MoonMsgCancelPairing,
    MoonMsgForget,
    MoonMsgYield,
    MoonMsgResume,
    /* BLE plumbing and incoming RPC will land here in Phase 1+ */
} MoonMsgType;

typedef struct {
    MoonMsgType type;
    union {
        uint32_t yield_duration_ms;
    } data;
} MoonMsg;

/* ── Persisted settings ────────────────────────────────────────────── */

static void moon_companion_load(MoonCompanion* moon) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(
           f, MOON_COMPANION_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t read = storage_file_read(f, &moon->persist, sizeof(moon->persist));
        ok = (read == sizeof(moon->persist));
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    if(!ok) {
        memset(&moon->persist, 0, sizeof(moon->persist));
    }
    FURI_LOG_I(
        TAG,
        "Loaded persist: paired=%d",
        moon->persist.paired);
}

static void moon_companion_save(MoonCompanion* moon) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(
           f, MOON_COMPANION_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, &moon->persist, sizeof(moon->persist));
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

/* ── Public API (query side) ───────────────────────────────────────── */

MoonConnectionState moon_companion_get_state(MoonCompanion* moon) {
    furi_check(moon);
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    MoonConnectionState s = moon->state;
    furi_mutex_release(moon->mutex);
    return s;
}

bool moon_companion_is_paired(MoonCompanion* moon) {
    furi_check(moon);
    return moon->persist.paired;
}

bool moon_companion_get_position(
    MoonCompanion* moon,
    MoonPosition* out,
    uint32_t max_age_ms) {
    furi_check(moon);
    furi_check(out);
    bool fresh = false;
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    if(moon->last_position_valid) {
        uint32_t age = furi_get_tick() - moon->last_position.tick;
        if(max_age_ms == 0 || age <= max_age_ms) {
            *out = moon->last_position;
            fresh = true;
        }
    }
    furi_mutex_release(moon->mutex);
    return fresh;
}

void moon_companion_subscribe_position(
    MoonCompanion* moon,
    MoonPositionCallback cb,
    void* ctx) {
    furi_check(moon);
    furi_check(cb);
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    /* Dedupe: (cb, ctx) pair is idempotent */
    for(uint8_t i = 0; i < moon->pos_sub_count; i++) {
        if(moon->pos_subs[i].cb == cb && moon->pos_subs[i].ctx == ctx) {
            furi_mutex_release(moon->mutex);
            return;
        }
    }
    if(moon->pos_sub_count < MOON_COMPANION_MAX_SUBSCRIBERS) {
        moon->pos_subs[moon->pos_sub_count].cb = cb;
        moon->pos_subs[moon->pos_sub_count].ctx = ctx;
        moon->pos_sub_count++;
    } else {
        FURI_LOG_W(TAG, "Position subscriber table full (max %d)", MOON_COMPANION_MAX_SUBSCRIBERS);
    }
    furi_mutex_release(moon->mutex);
}

void moon_companion_unsubscribe_position(
    MoonCompanion* moon,
    MoonPositionCallback cb,
    void* ctx) {
    furi_check(moon);
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    for(uint8_t i = 0; i < moon->pos_sub_count; i++) {
        if(moon->pos_subs[i].cb == cb && moon->pos_subs[i].ctx == ctx) {
            /* Swap-with-last, decrement. Order doesn't matter. */
            moon->pos_subs[i] = moon->pos_subs[moon->pos_sub_count - 1];
            moon->pos_sub_count--;
            break;
        }
    }
    furi_mutex_release(moon->mutex);
}

bool moon_companion_get_time(MoonCompanion* moon, MoonTime* out) {
    furi_check(moon);
    furi_check(out);
    bool ok = false;
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    if(moon->last_time_valid) {
        *out = moon->last_time;
        ok = true;
    }
    furi_mutex_release(moon->mutex);
    return ok;
}

bool moon_companion_send_notification(
    MoonCompanion* moon,
    const char* title,
    const char* body,
    MoonNotificationPriority priority) {
    furi_check(moon);
    furi_check(title);
    furi_check(body);
    (void)priority;
    /* Phase 0 stub: will be wired once the BLE RPC transport lands. */
    FURI_LOG_D(TAG, "notification stub: [%s] %s", title, body);
    return false;
}

/* ── Public API (command side — posts to service queue) ────────────── */

bool moon_companion_begin_pairing(MoonCompanion* moon, char pin_out[7]) {
    furi_check(moon);
    furi_check(pin_out);
    MoonMsg m = {.type = MoonMsgBeginPairing};
    if(furi_message_queue_put(moon->queue, &m, 100) != FuriStatusOk) return false;
    /* Wait a tick for the pairing thread to generate the pin. Phase 0: just
     * return a static placeholder. */
    strncpy(pin_out, "000000", 7);
    return true;
}

void moon_companion_cancel_pairing(MoonCompanion* moon) {
    furi_check(moon);
    MoonMsg m = {.type = MoonMsgCancelPairing};
    furi_message_queue_put(moon->queue, &m, 100);
}

void moon_companion_forget(MoonCompanion* moon) {
    furi_check(moon);
    MoonMsg m = {.type = MoonMsgForget};
    furi_message_queue_put(moon->queue, &m, 100);
}

void moon_companion_yield(MoonCompanion* moon, uint32_t duration_ms) {
    furi_check(moon);
    MoonMsg m = {.type = MoonMsgYield, .data.yield_duration_ms = duration_ms};
    furi_message_queue_put(moon->queue, &m, 100);
}

void moon_companion_resume(MoonCompanion* moon) {
    furi_check(moon);
    MoonMsg m = {.type = MoonMsgResume};
    furi_message_queue_put(moon->queue, &m, 100);
}

/* ── Service thread ────────────────────────────────────────────────── */

static int32_t moon_companion_thread(void* context) {
    MoonCompanion* moon = context;
    FURI_LOG_I(TAG, "Service started (paired=%d)", moon->persist.paired);

    for(;;) {
        MoonMsg msg;
        FuriStatus st = furi_message_queue_get(moon->queue, &msg, FuriWaitForever);
        if(st != FuriStatusOk) continue;

        switch(msg.type) {
        case MoonMsgStop:
            FURI_LOG_I(TAG, "Service stopping");
            return 0;

        case MoonMsgBeginPairing:
            /* TODO Phase 1: generate random 6-digit pin, set state to
             * scanning, accept any inbound advert claiming Moon Companion
             * service UUID, drive SMP-free pairing via the app-layer
             * token exchange. */
            FURI_LOG_I(TAG, "TODO: begin pairing");
            break;

        case MoonMsgCancelPairing:
            FURI_LOG_I(TAG, "TODO: cancel pairing");
            break;

        case MoonMsgForget:
            memset(&moon->persist, 0, sizeof(moon->persist));
            moon_companion_save(moon);
            FURI_LOG_I(TAG, "Paired phone forgotten");
            break;

        case MoonMsgYield:
            furi_mutex_acquire(moon->mutex, FuriWaitForever);
            moon->yielded = true;
            moon->yield_until_tick = furi_get_tick() + msg.data.yield_duration_ms;
            moon->state = MoonConnStateYielded;
            furi_mutex_release(moon->mutex);
            FURI_LOG_I(TAG, "Yielding BLE for %lu ms", msg.data.yield_duration_ms);
            /* TODO Phase 2: tear down central connection here. */
            break;

        case MoonMsgResume:
            furi_mutex_acquire(moon->mutex, FuriWaitForever);
            moon->yielded = false;
            moon->state = MoonConnStateDisconnected;
            furi_mutex_release(moon->mutex);
            FURI_LOG_I(TAG, "Resumed");
            /* TODO Phase 2: restart scan-and-connect loop here. */
            break;
        }
    }
    return 0;
}

/* ── Service entry point (referenced from application.fam) ─────────── */

int32_t moon_companion_srv(void* p) {
    UNUSED(p);

    MoonCompanion* moon = malloc(sizeof(MoonCompanion));
    memset(moon, 0, sizeof(*moon));

    moon->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    moon->queue = furi_message_queue_alloc(8, sizeof(MoonMsg));
    moon->state = MoonConnStateDisconnected;

    moon_companion_load(moon);

    moon->thread = furi_thread_alloc_ex("MoonCompanion", 2048, moon_companion_thread, moon);
    furi_thread_start(moon->thread);

    furi_record_create(RECORD_MOON_COMPANION, moon);

    /* Services never return in Flipper firmware — the record lives forever
     * and the thread loop drains the queue. */
    return 0;
}
