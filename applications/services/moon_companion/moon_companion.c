#include "moon_companion.h"
#include "moon_companion_i.h"
#include "proto/moon_companion.pb.h"

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <pb_decode.h>
#include <pb_encode.h>

#define TAG "MoonCompanion"

/* ── Internal message plumbing ─────────────────────────────────────── */

typedef enum {
    MoonMsgStop,
    MoonMsgBeginPairing,
    MoonMsgCancelPairing,
    MoonMsgForget,
    MoonMsgYield,
    MoonMsgResume,
    MoonMsgBleStateChanged,
} MoonMsgType;

typedef struct {
    MoonMsgType type;
    union {
        uint32_t yield_duration_ms;
        MoonBleState ble_state;
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
    FURI_LOG_I(TAG, "Loaded persist: paired=%d", moon->persist.paired);
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

/* ── BLE callbacks — forward into the service queue ──────────────── */

static void moon_companion_on_ble_state(MoonBleState state, void* ctx) {
    MoonCompanion* moon = ctx;
    MoonMsg m = {.type = MoonMsgBleStateChanged, .data.ble_state = state};
    furi_message_queue_put(moon->queue, &m, 0);
}

/* Forward declaration — defined after the RPC helpers. */
static void moon_companion_handle_rx_frame(
    MoonCompanion* moon,
    const uint8_t* data,
    size_t len);

static void moon_companion_on_rpc_rx(const uint8_t* data, size_t len, void* ctx) {
    MoonCompanion* moon = ctx;
    if(len == 0 || len > 512) return;
    /* Decode directly on the BT thread — avoids a queue hop that would
     * deadlock the pair handler (which blocks the service thread on an
     * event flag that only the service thread could otherwise set). */
    moon_companion_handle_rx_frame(moon, data, len);
}

/* ── RPC send helpers ──────────────────────────────────────────── */

static uint32_t moon_companion_next_rid(MoonCompanion* moon) {
    /* request_id = 0 is reserved for "unset / broadcast". */
    uint32_t r;
    do {
        r = ++moon->next_request_id;
    } while(r == 0);
    return r;
}

static bool moon_companion_encode_and_send(
    MoonCompanion* moon,
    const moon_companion_v1_MoonRequest* req) {
    uint8_t buf[244];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if(!pb_encode(&stream, moon_companion_v1_MoonRequest_fields, req)) {
        FURI_LOG_E(TAG, "pb_encode failed: %s", PB_GET_ERROR(&stream));
        return false;
    }
    return moon_ble_send(moon->ble, buf, stream.bytes_written);
}

/* Claim a slot and allocate an event flag. Returns NULL if the table is
 * full. Caller frees via moon_companion_finish_rpc. */
static MoonRpcInFlight* moon_companion_begin_rpc(MoonCompanion* moon, uint32_t rid) {
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    MoonRpcInFlight* slot = NULL;
    for(uint8_t i = 0; i < MOON_COMPANION_MAX_INFLIGHT; i++) {
        if(moon->inflight[i].request_id == 0) {
            slot = &moon->inflight[i];
            slot->request_id = rid;
            slot->done = furi_event_flag_alloc();
            slot->response_buf = NULL;
            slot->filled = false;
            slot->timed_out = false;
            break;
        }
    }
    furi_mutex_release(moon->mutex);
    return slot;
}

static void moon_companion_finish_rpc(MoonCompanion* moon, MoonRpcInFlight* slot) {
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    if(slot->done) {
        furi_event_flag_free(slot->done);
        slot->done = NULL;
    }
    if(slot->response_buf) {
        free(slot->response_buf);
        slot->response_buf = NULL;
    }
    slot->request_id = 0;
    slot->filled = false;
    slot->timed_out = false;
    furi_mutex_release(moon->mutex);
}

/* Find a waiting slot for this request_id and deliver the decoded
 * response. Returns true if a waiter claimed it. */
static bool moon_companion_deliver_response(
    MoonCompanion* moon,
    moon_companion_v1_MoonResponse* resp) {
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    MoonRpcInFlight* match = NULL;
    for(uint8_t i = 0; i < MOON_COMPANION_MAX_INFLIGHT; i++) {
        if(moon->inflight[i].request_id == resp->request_id &&
           moon->inflight[i].done != NULL) {
            match = &moon->inflight[i];
            break;
        }
    }
    if(match) {
        /* Hand ownership of the decoded response to the waiter. */
        match->response_buf = resp;
        match->filled = true;
        furi_event_flag_set(match->done, 0x1);
    }
    furi_mutex_release(moon->mutex);
    return match != NULL;
}

/* ── Incoming RPC frame decode ──────────────────────────────────── */

static void moon_companion_handle_event(
    MoonCompanion* moon,
    const moon_companion_v1_MoonEvent* ev) {
    if(ev->which_event == moon_companion_v1_MoonEvent_position_update_tag) {
        const moon_companion_v1_PositionData* p = &ev->event.position_update;
        MoonPosition pos = {0};
        pos.lat_e7       = p->lat_e7;
        pos.lon_e7       = p->lon_e7;
        pos.alt_mm       = p->alt_mm;
        pos.accuracy_mm  = p->accuracy_mm;
        pos.speed_mmps   = p->speed_mmps;
        pos.heading_cdeg = p->heading_cdeg;
        pos.timestamp_ms = p->timestamp_ms;
        pos.tick         = furi_get_tick();
        pos.satellites   = (uint8_t)p->satellites;
        pos.fix_quality  = (MoonFixQuality)p->fix_quality;
        size_t src_len = p->source_id.size > 8 ? 8 : p->source_id.size;
        memcpy(pos.source_id, p->source_id.bytes, src_len);

        MoonPositionCallback subs[MOON_COMPANION_MAX_SUBSCRIBERS];
        void* ctxs[MOON_COMPANION_MAX_SUBSCRIBERS];
        uint8_t n;
        furi_mutex_acquire(moon->mutex, FuriWaitForever);
        moon->last_position = pos;
        moon->last_position_valid = true;
        n = moon->pos_sub_count;
        for(uint8_t i = 0; i < n; i++) {
            subs[i] = moon->pos_subs[i].cb;
            ctxs[i] = moon->pos_subs[i].ctx;
        }
        furi_mutex_release(moon->mutex);
        for(uint8_t i = 0; i < n; i++) subs[i](&pos, ctxs[i]);
    }
}

static void moon_companion_handle_rx_frame(
    MoonCompanion* moon,
    const uint8_t* data,
    size_t len) {
    /* Envelope is MoonPhoneMessage — oneof{MoonResponse, MoonEvent}. */
    moon_companion_v1_MoonPhoneMessage* msg =
        malloc(sizeof(moon_companion_v1_MoonPhoneMessage));
    if(!msg) return;
    memset(msg, 0, sizeof(*msg));

    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if(!pb_decode(&stream, moon_companion_v1_MoonPhoneMessage_fields, msg)) {
        FURI_LOG_W(TAG, "RX decode failed: %s", PB_GET_ERROR(&stream));
        free(msg);
        return;
    }

    if(msg->which_kind == moon_companion_v1_MoonPhoneMessage_response_tag) {
        /* Hand off the response to the waiter — moves ownership. */
        moon_companion_v1_MoonResponse* resp =
            malloc(sizeof(moon_companion_v1_MoonResponse));
        if(!resp) {
            free(msg);
            return;
        }
        *resp = msg->kind.response;
        if(!moon_companion_deliver_response(moon, resp)) {
            /* Nobody waiting — log and drop. */
            FURI_LOG_W(TAG, "Unsolicited response rid=%lu", resp->request_id);
            free(resp);
        }
    } else if(msg->which_kind == moon_companion_v1_MoonPhoneMessage_event_tag) {
        moon_companion_handle_event(moon, &msg->kind.event);
    }
    free(msg);
}

/* ── Public API — query side ───────────────────────────────────── */

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
        FURI_LOG_W(TAG, "Position subscriber table full (max %d)",
                   MOON_COMPANION_MAX_SUBSCRIBERS);
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
    /* Phase 2: build MoonRequest{send_notification}, send, await ack. */
    FURI_LOG_D(TAG, "notification stub: [%s] %s", title, body);
    return false;
}

/* ── Public API — command side ─────────────────────────────────── */

bool moon_companion_begin_pairing(MoonCompanion* moon, char pin_out[7]) {
    furi_check(moon);
    furi_check(pin_out);
    /* Phase 1a: the phone (peripheral) generates the actual token; the
     * PIN displayed here is a decorative placeholder until Phase 1b
     * wires ECDH-based short-code verification. */
    uint32_t r = furi_hal_random_get();
    snprintf(pin_out, 7, "%06lu", (unsigned long)(r % 1000000));
    memcpy(moon->pairing_pin, pin_out, 7);

    MoonMsg m = {.type = MoonMsgBeginPairing};
    if(furi_message_queue_put(moon->queue, &m, 100) != FuriStatusOk) return false;
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

/* ── Post-connect bring-up ──────────────────────────────────────
 *
 * Once we hit MoonBleStateConnected with a valid auth_token, fire a
 * SubscribePositionRequest so the phone starts streaming PositionData
 * events that will keep last_position warm. */
static void moon_companion_on_authed_connected(MoonCompanion* moon) {
    moon_companion_v1_MoonRequest req = moon_companion_v1_MoonRequest_init_zero;
    req.request_id = moon_companion_next_rid(moon);
    req.auth_token.size = MOON_COMPANION_AUTH_TOKEN_SIZE;
    memcpy(req.auth_token.bytes, moon->persist.auth_token, MOON_COMPANION_AUTH_TOKEN_SIZE);
    req.which_payload = moon_companion_v1_MoonRequest_subscribe_position_tag;
    req.payload.subscribe_position.interval_ms = 2000;

    if(!moon_companion_encode_and_send(moon, &req)) {
        FURI_LOG_E(TAG, "SubscribePosition failed to send");
        return;
    }
    FURI_LOG_I(TAG, "Position subscription active");
}

/* ── Pairing flow (Phase 1a) ───────────────────────────────────── */

/* When we reach MoonBleStateConnected during an active pair window, send
 * a bare MoonRequest{pair} and wait (asynchronously) for PairResponse
 * to land in the RPC dispatch. */
static void moon_companion_on_pair_connected(MoonCompanion* moon) {
    moon_companion_v1_MoonRequest req = moon_companion_v1_MoonRequest_init_zero;
    req.request_id = moon_companion_next_rid(moon);
    /* No auth_token yet — phone must accept unpaired peers only while
     * it's in its own pair mode. */
    req.which_payload = moon_companion_v1_MoonRequest_pair_tag;
    strncpy(req.payload.pair.flipper_name, "Moon Flipper",
            sizeof(req.payload.pair.flipper_name) - 1);

    MoonRpcInFlight* slot = moon_companion_begin_rpc(moon, req.request_id);
    if(!slot) {
        FURI_LOG_E(TAG, "RPC table full, cannot pair");
        return;
    }

    if(!moon_companion_encode_and_send(moon, &req)) {
        FURI_LOG_E(TAG, "Failed to send pair request");
        moon_companion_finish_rpc(moon, slot);
        return;
    }

    /* Block briefly on the RPC response. Real deployments should decouple
     * this from the service thread — for Phase 1a we just wait. */
    uint32_t flags = furi_event_flag_wait(
        slot->done, 0x1, FuriFlagWaitAny, MOON_COMPANION_RPC_TIMEOUT_MS);

    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    bool filled = slot->filled;
    moon_companion_v1_MoonResponse* resp = slot->response_buf;
    slot->response_buf = NULL;
    furi_mutex_release(moon->mutex);

    if(!filled || !resp || (flags & FuriFlagError)) {
        FURI_LOG_E(TAG, "Pair RPC timed out or failed");
        if(resp) free(resp);
        moon_companion_finish_rpc(moon, slot);
        return;
    }

    if(resp->status == moon_companion_v1_MoonStatus_MOON_OK &&
       resp->which_payload == moon_companion_v1_MoonResponse_pair_tag &&
       resp->payload.pair.auth_token.size == MOON_COMPANION_AUTH_TOKEN_SIZE) {
        furi_mutex_acquire(moon->mutex, FuriWaitForever);
        memcpy(moon->persist.auth_token, resp->payload.pair.auth_token.bytes,
               MOON_COMPANION_AUTH_TOKEN_SIZE);
        moon->persist.paired = true;
        moon->pairing_active = false;
        furi_mutex_release(moon->mutex);
        moon_companion_save(moon);
        FURI_LOG_I(TAG, "Paired with phone: %s", resp->payload.pair.phone_name);
    } else {
        FURI_LOG_W(TAG, "Phone refused pairing (status=%d)", resp->status);
    }

    free(resp);
    moon_companion_finish_rpc(moon, slot);
}

/* ── Service entry point ──────────────────────────────────────── */

int32_t moon_companion_srv(void* p) {
    UNUSED(p);

    MoonCompanion* moon = malloc(sizeof(MoonCompanion));
    memset(moon, 0, sizeof(*moon));

    moon->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    moon->queue = furi_message_queue_alloc(16, sizeof(MoonMsg));
    moon->state = MoonConnStateDisconnected;

    moon_companion_load(moon);

    moon->ble = moon_ble_alloc();
    moon_ble_set_state_callback(moon->ble, moon_companion_on_ble_state, moon);
    moon_ble_set_rx_callback(moon->ble, moon_companion_on_rpc_rx, moon);

    furi_record_create(RECORD_MOON_COMPANION, moon);

    FURI_LOG_I(TAG, "Service started (paired=%d)", moon->persist.paired);

    if(moon->persist.paired) {
        FURI_LOG_I(TAG, "Auto-reconnect: paired phone known, scanning");
        moon_ble_start_scan(moon->ble);
    }

    for(;;) {
        MoonMsg msg;
        FuriStatus st = furi_message_queue_get(moon->queue, &msg, FuriWaitForever);
        if(st != FuriStatusOk) continue;

        switch(msg.type) {
        case MoonMsgStop:
            FURI_LOG_I(TAG, "Service stopping");
            moon_ble_stop(moon->ble);
            return 0;

        case MoonMsgBeginPairing:
            FURI_LOG_I(TAG, "Begin pairing (PIN %s)", moon->pairing_pin);
            moon->pairing_active = true;
            /* Drop any existing pairing — user asked for a fresh one. */
            memset(&moon->persist, 0, sizeof(moon->persist));
            moon_companion_save(moon);
            moon_ble_start_scan(moon->ble);
            break;

        case MoonMsgCancelPairing:
            FURI_LOG_I(TAG, "Cancel pairing");
            moon->pairing_active = false;
            moon_ble_stop(moon->ble);
            break;

        case MoonMsgForget:
            memset(&moon->persist, 0, sizeof(moon->persist));
            moon_companion_save(moon);
            moon_ble_stop(moon->ble);
            FURI_LOG_I(TAG, "Paired phone forgotten");
            break;

        case MoonMsgYield:
            furi_mutex_acquire(moon->mutex, FuriWaitForever);
            moon->yielded = true;
            moon->yield_until_tick = furi_get_tick() + msg.data.yield_duration_ms;
            moon->state = MoonConnStateYielded;
            furi_mutex_release(moon->mutex);
            FURI_LOG_I(TAG, "Yielding BLE for %lu ms",
                       (unsigned long)msg.data.yield_duration_ms);
            moon_ble_stop(moon->ble);
            break;

        case MoonMsgResume:
            furi_mutex_acquire(moon->mutex, FuriWaitForever);
            moon->yielded = false;
            moon->state = MoonConnStateDisconnected;
            furi_mutex_release(moon->mutex);
            FURI_LOG_I(TAG, "Resumed");
            if(moon->persist.paired) moon_ble_start_scan(moon->ble);
            break;

        case MoonMsgBleStateChanged:
            /* Mirror BLE state into the service-visible state enum. */
            furi_mutex_acquire(moon->mutex, FuriWaitForever);
            switch(msg.data.ble_state) {
            case MoonBleStateIdle:
            case MoonBleStateError:
                moon->state = MoonConnStateDisconnected;
                break;
            case MoonBleStateScanning:
                moon->state = MoonConnStateScanning;
                break;
            case MoonBleStateConnecting:
            case MoonBleStatePairing:
            case MoonBleStateDiscovering:
                moon->state = MoonConnStateConnecting;
                break;
            case MoonBleStateConnected:
                moon->state = MoonConnStateConnected;
                break;
            }
            furi_mutex_release(moon->mutex);

            if(msg.data.ble_state == MoonBleStateConnected) {
                if(moon->pairing_active) {
                    moon_companion_on_pair_connected(moon);
                } else if(moon->persist.paired) {
                    moon_companion_on_authed_connected(moon);
                }
            }
            break;
        }
    }
    return 0;
}
