#include "moon_companion.h"
#include "moon_companion_i.h"
#include "proto/moon_companion.pb.h"

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <furi_ble/l2cap_coc.h>
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
    MoonMsgReconnect,
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

/* Fires a few seconds after the BLE link goes Idle/Error to retry scan.
 * Runs on the FuriTimer thread — post to the service queue and let the
 * service thread do the actual work (it owns the BLE state). */
static void moon_companion_reconnect_tick(void* ctx) {
    MoonCompanion* moon = ctx;
    MoonMsg m = {.type = MoonMsgReconnect};
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

/* ── HTTP proxy ────────────────────────────────────────────────────── */

bool moon_companion_http_request(
    MoonCompanion* moon,
    const MoonHttpRequest* req,
    MoonHttpResponse* resp_out) {
    furi_check(moon);
    furi_check(req);
    furi_check(req->url);
    furi_check(resp_out);

    memset(resp_out, 0, sizeof(*resp_out));

    if(moon_ble_get_state(moon->ble) != MoonBleStateConnected ||
       !moon->persist.paired) {
        FURI_LOG_W(TAG, "http_request: not connected/paired");
        return false;
    }

    /* Build MoonRequest{http}. */
    moon_companion_v1_MoonRequest pbreq = moon_companion_v1_MoonRequest_init_zero;
    pbreq.request_id = moon_companion_next_rid(moon);
    pbreq.auth_token.size = MOON_COMPANION_AUTH_TOKEN_SIZE;
    memcpy(pbreq.auth_token.bytes, moon->persist.auth_token,
           MOON_COMPANION_AUTH_TOKEN_SIZE);
    pbreq.which_payload = moon_companion_v1_MoonRequest_http_tag;

    moon_companion_v1_HttpRequest* h = &pbreq.payload.http;
    strncpy(h->method, req->method ? req->method : "GET", sizeof(h->method) - 1);
    strncpy(h->url, req->url, sizeof(h->url) - 1);
    h->timeout_ms = req->timeout_ms;
    h->use_bulk = false; /* v1: caller cannot hint bulk; phone decides */

    if(req->headers && req->headers_count > 0) {
        size_t n = req->headers_count;
        if(n > sizeof(h->headers) / sizeof(h->headers[0])) {
            n = sizeof(h->headers) / sizeof(h->headers[0]);
        }
        for(size_t i = 0; i < n; i++) {
            if(req->headers[i].key) {
                strncpy(h->headers[i].key, req->headers[i].key,
                        sizeof(h->headers[i].key) - 1);
            }
            if(req->headers[i].value) {
                strncpy(h->headers[i].value, req->headers[i].value,
                        sizeof(h->headers[i].value) - 1);
            }
        }
        h->headers_count = (pb_size_t)n;
    }

    if(req->body && req->body_len > 0) {
        size_t n = req->body_len;
        if(n > sizeof(h->body.bytes)) n = sizeof(h->body.bytes);
        memcpy(h->body.bytes, req->body, n);
        h->body.size = (pb_size_t)n;
    }

    /* Begin RPC slot. */
    MoonRpcInFlight* slot = moon_companion_begin_rpc(moon, pbreq.request_id);
    if(!slot) {
        FURI_LOG_E(TAG, "http_request: RPC table full");
        return false;
    }

    if(!moon_companion_encode_and_send(moon, &pbreq)) {
        /* pb_encode may have rejected the envelope as too large for a
         * single GATT notification; the FAP's URL + headers + body is
         * over budget. */
        FURI_LOG_E(TAG, "http_request: encode/send failed (envelope too large?)");
        moon_companion_finish_rpc(moon, slot);
        return false;
    }

    /* Wait for response. Phone side has its own timeout; we allow a bit
     * of headroom here. */
    uint32_t wait_ms = req->timeout_ms ? req->timeout_ms + 5000 : 35000;
    uint32_t flags = furi_event_flag_wait(
        slot->done, 0x1, FuriFlagWaitAny, wait_ms);

    bool ok = false;
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    bool filled = slot->filled;
    moon_companion_v1_MoonResponse* resp = slot->response_buf;
    slot->response_buf = NULL;
    furi_mutex_release(moon->mutex);

    if((flags & FuriFlagError) || !filled || !resp) {
        FURI_LOG_W(TAG, "http_request: RPC timed out");
        if(resp) free(resp);
        moon_companion_finish_rpc(moon, slot);
        return false;
    }

    if(resp->which_payload == moon_companion_v1_MoonResponse_http_tag) {
        const moon_companion_v1_HttpResponse* hr = &resp->payload.http;
        resp_out->status_code = hr->status_code;

        /* Copy headers. */
        if(hr->headers_count > 0) {
            resp_out->headers = malloc(hr->headers_count * sizeof(MoonHttpHeaderOut));
            if(resp_out->headers) {
                for(pb_size_t i = 0; i < hr->headers_count; i++) {
                    strncpy(resp_out->headers[i].key, hr->headers[i].key,
                            sizeof(resp_out->headers[i].key) - 1);
                    resp_out->headers[i].key[sizeof(resp_out->headers[i].key) - 1] = '\0';
                    strncpy(resp_out->headers[i].value, hr->headers[i].value,
                            sizeof(resp_out->headers[i].value) - 1);
                    resp_out->headers[i].value[sizeof(resp_out->headers[i].value) - 1] = '\0';
                }
                resp_out->headers_count = hr->headers_count;
            }
        }

        if(hr->bulk_bytes > 0) {
            /* Phone signalled bulk transfer. v1: we surface the metadata
             * and return true — the caller can consume it diagnostically
             * or, in a later build, dial the SPSM to stream. */
            resp_out->bulk_bytes = hr->bulk_bytes;
            resp_out->spsm = (uint16_t)hr->spsm;
            size_t sid = hr->session_id.size;
            if(sid > sizeof(resp_out->session_id)) sid = sizeof(resp_out->session_id);
            memcpy(resp_out->session_id, hr->session_id.bytes, sid);
            resp_out->session_id_len = sid;
            ok = true;
        } else if(hr->body.size > 0) {
            resp_out->body = malloc(hr->body.size);
            if(resp_out->body) {
                memcpy(resp_out->body, hr->body.bytes, hr->body.size);
                resp_out->body_len = hr->body.size;
                ok = true;
            } else {
                FURI_LOG_E(TAG, "http_request: body malloc failed");
            }
        } else {
            /* Empty body is legal (HEAD, 204, etc.) — success. */
            ok = true;
        }
    } else {
        /* Phone returned a MoonResponse that isn't an HttpResponse — e.g.
         * an UnauthorizedAck or similar. Reflect status_code best-effort. */
        FURI_LOG_W(TAG, "http_request: unexpected response payload tag %d",
                   resp->which_payload);
    }

    free(resp);
    moon_companion_finish_rpc(moon, slot);
    return ok;
}

void moon_companion_http_response_free(MoonHttpResponse* resp) {
    if(!resp) return;
    if(resp->body) {
        free(resp->body);
        resp->body = NULL;
    }
    if(resp->headers) {
        free(resp->headers);
        resp->headers = NULL;
    }
    resp->body_len = 0;
    resp->headers_count = 0;
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

/* ── Bulk transfer (L2CAP CoC) ──────────────────────────────────
 *
 * The Moon Companion protocol's bulk path works like this:
 *
 *   1. Caller invokes moon_companion_bulk_open_blocking(kind, name, ...).
 *   2. We send an OpenBulkChannelRequest on GATT; phone replies with
 *      the PSM its L2CAP server is listening on + an 8-byte session id.
 *   3. We dial an L2CAP CoC to that PSM.
 *   4. Phone streams bytes on the CoC; each chunk lands in our
 *      moon_bulk_coc_callback (on the BT thread) and is forwarded to
 *      the caller's on_data hook.
 *   5. Phone closes the CoC when done, we unblock the caller with the
 *      final byte count.
 *
 * Only one bulk transfer is permitted at a time — the code keeps a
 * single `active_bulk` slot on the service struct.
 */

static void moon_bulk_coc_callback(BleL2capCocEvent* event, void* context) {
    MoonCompanion* moon = context;
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    MoonBulkSession* s = moon->active_bulk;
    if(!s) {
        /* CoC event on a stale channel — ignore. */
        furi_mutex_release(moon->mutex);
        return;
    }

    switch(event->type) {
    case BleL2capCocEventConnected:
        s->coc_channel_index = event->channel_index;
        s->coc_channel_valid = true;
        FURI_LOG_I(TAG, "Bulk CoC up (channel=%u, peer_mtu=%u)",
                   event->channel_index, event->connected.peer_mtu);
        furi_event_flag_set(s->event, MOON_BULK_EVT_COC_CONNECTED);
        break;

    case BleL2capCocEventDataReceived: {
        MoonBulkDataCallback cb = s->on_data;
        void* cb_ctx = s->on_data_ctx;
        const uint8_t* data = event->data.data;
        uint16_t len = event->data.data_len;
        s->bytes_received += len;
        /* Drop the mutex before invoking the caller's callback — it
         * might call back into the service (e.g., to log via the same
         * mutex) and we don't want to deadlock. */
        furi_mutex_release(moon->mutex);
        if(cb && len) cb(data, len, cb_ctx);
        return; /* already released */
    }

    case BleL2capCocEventDisconnected:
        FURI_LOG_I(TAG, "Bulk CoC closed after %lu bytes",
                   (unsigned long)s->bytes_received);
        s->coc_channel_valid = false;
        furi_event_flag_set(s->event, MOON_BULK_EVT_COC_DONE);
        break;

    case BleL2capCocEventError:
        FURI_LOG_W(TAG, "Bulk CoC error 0x%04x", event->error.code);
        s->error_code = event->error.code;
        s->coc_channel_valid = false;
        furi_event_flag_set(s->event, MOON_BULK_EVT_COC_ERROR);
        break;

    case BleL2capCocEventCreditsReceived:
    case BleL2capCocEventTxDone:
        /* Don't care — we're receive-only in Phase 3. */
        break;
    }

    furi_mutex_release(moon->mutex);
}

/* Called from moon_companion_deliver_response when a response with
 * which_payload == open_bulk_channel arrives. Pulls the PSM + session
 * id out of the decoded response and signals the waiting thread. */
static void moon_bulk_on_open_response(
    MoonCompanion* moon,
    const moon_companion_v1_MoonResponse* resp) {
    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    MoonBulkSession* s = moon->active_bulk;
    if(s) {
        if(resp->status == moon_companion_v1_MoonStatus_MOON_OK &&
           resp->which_payload == moon_companion_v1_MoonResponse_open_bulk_channel_tag) {
            s->spsm = (uint16_t)resp->payload.open_bulk_channel.spsm;
            s->expected_bytes = resp->payload.open_bulk_channel.total_bytes;
            size_t id_len = resp->payload.open_bulk_channel.session_id.size;
            if(id_len > sizeof(s->session_id)) id_len = sizeof(s->session_id);
            memcpy(s->session_id, resp->payload.open_bulk_channel.session_id.bytes, id_len);
            furi_event_flag_set(s->event, MOON_BULK_EVT_OPENED);
        } else {
            FURI_LOG_W(TAG, "OpenBulkChannel refused (status=%d)", resp->status);
            s->error_code = 0xFFFF;
            furi_event_flag_set(s->event, MOON_BULK_EVT_COC_ERROR);
        }
    }
    furi_mutex_release(moon->mutex);
}

bool moon_companion_bulk_open_blocking(
    MoonCompanion* moon,
    MoonBulkKind kind,
    const char* name,
    uint32_t timeout_ms,
    MoonBulkDataCallback on_data,
    void* on_data_ctx,
    uint32_t* out_bytes_received,
    uint16_t* out_error_code) {
    furi_check(moon);
    if(timeout_ms == 0) timeout_ms = 30 * 1000;

    if(moon_ble_get_state(moon->ble) != MoonBleStateConnected ||
       !moon->persist.paired) {
        if(out_error_code) *out_error_code = 0xFFFE; /* not connected */
        return false;
    }

    uint16_t conn_handle = moon_ble_get_connection_handle(moon->ble);
    if(conn_handle == 0) {
        if(out_error_code) *out_error_code = 0xFFFE;
        return false;
    }

    /* Allocate the session up front. CoC events the peer might already
     * be delivering need somewhere to land — publish active_bulk before
     * we send the request. */
    MoonBulkSession* s = malloc(sizeof(MoonBulkSession));
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->on_data = on_data;
    s->on_data_ctx = on_data_ctx;
    s->event = furi_event_flag_alloc();

    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    if(moon->active_bulk != NULL) {
        furi_mutex_release(moon->mutex);
        furi_event_flag_free(s->event);
        free(s);
        if(out_error_code) *out_error_code = 0xFFFD; /* busy */
        return false;
    }
    moon->active_bulk = s;
    furi_mutex_release(moon->mutex);

    /* Register the CoC callback on our BLE connection. */
    ble_l2cap_coc_set_callback(conn_handle, moon_bulk_coc_callback, moon);

    bool success = false;
    uint16_t err = 0xFFFF;

    /* Step 1: send OpenBulkChannelRequest and wait for the response. */
    moon_companion_v1_MoonRequest req = moon_companion_v1_MoonRequest_init_zero;
    req.request_id = moon_companion_next_rid(moon);
    req.auth_token.size = MOON_COMPANION_AUTH_TOKEN_SIZE;
    memcpy(req.auth_token.bytes, moon->persist.auth_token, MOON_COMPANION_AUTH_TOKEN_SIZE);
    req.which_payload = moon_companion_v1_MoonRequest_open_bulk_channel_tag;
    req.payload.open_bulk_channel.kind = (moon_companion_v1_BulkKind)kind;
    if(name) {
        strncpy(req.payload.open_bulk_channel.name, name,
                sizeof(req.payload.open_bulk_channel.name) - 1);
    }

    MoonRpcInFlight* slot = moon_companion_begin_rpc(moon, req.request_id);
    if(!slot) {
        FURI_LOG_E(TAG, "RPC table full for OpenBulkChannel");
        goto cleanup;
    }
    if(!moon_companion_encode_and_send(moon, &req)) {
        FURI_LOG_E(TAG, "Failed to send OpenBulkChannelRequest");
        moon_companion_finish_rpc(moon, slot);
        goto cleanup;
    }

    uint32_t flags = furi_event_flag_wait(
        slot->done, 0x1, FuriFlagWaitAny, MOON_COMPANION_RPC_TIMEOUT_MS);

    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    bool filled = slot->filled;
    moon_companion_v1_MoonResponse* resp = slot->response_buf;
    slot->response_buf = NULL;
    furi_mutex_release(moon->mutex);

    if(!filled || !resp || (flags & FuriFlagError)) {
        FURI_LOG_E(TAG, "OpenBulkChannel RPC timed out");
        if(resp) free(resp);
        moon_companion_finish_rpc(moon, slot);
        err = 0xFFF0;
        goto cleanup;
    }
    moon_bulk_on_open_response(moon, resp);
    free(resp);
    moon_companion_finish_rpc(moon, slot);

    /* Step 2: wait for MOON_BULK_EVT_OPENED. moon_bulk_on_open_response
     * set it if the response carried a valid PSM; otherwise it set
     * MOON_BULK_EVT_COC_ERROR. */
    uint32_t got = furi_event_flag_wait(
        s->event,
        MOON_BULK_EVT_OPENED | MOON_BULK_EVT_COC_ERROR,
        FuriFlagWaitAny,
        MOON_COMPANION_RPC_TIMEOUT_MS);
    if(!(got & MOON_BULK_EVT_OPENED) || (got & FuriFlagError)) {
        err = s->error_code ? s->error_code : 0xFFF1;
        goto cleanup;
    }

    /* Step 3: dial the CoC. */
    FURI_LOG_I(TAG, "Dialing CoC: conn=0x%04x psm=%u expected=%lu bytes",
               conn_handle, s->spsm, (unsigned long)s->expected_bytes);
    if(!ble_l2cap_coc_connect(
           conn_handle, s->spsm, MOON_BULK_MTU, MOON_BULK_MPS, MOON_BULK_CREDITS)) {
        FURI_LOG_E(TAG, "ble_l2cap_coc_connect failed");
        err = 0xFFF2;
        goto cleanup;
    }

    /* Step 4: wait for Connected, then for Done/Error (data arrives in
     * between via the callback). Use the whole caller-supplied timeout
     * for the full transfer window. */
    got = furi_event_flag_wait(
        s->event,
        MOON_BULK_EVT_COC_CONNECTED | MOON_BULK_EVT_COC_ERROR,
        FuriFlagWaitAny,
        10 * 1000);
    if(!(got & MOON_BULK_EVT_COC_CONNECTED)) {
        err = s->error_code ? s->error_code : 0xFFF3;
        goto cleanup;
    }

    got = furi_event_flag_wait(
        s->event,
        MOON_BULK_EVT_COC_DONE | MOON_BULK_EVT_COC_ERROR,
        FuriFlagWaitAny,
        timeout_ms);

    if(got & MOON_BULK_EVT_COC_DONE) {
        /* Clean close. Transfer is successful iff expected_bytes is 0
         * (streaming, take whatever we got) or we received exactly that
         * many bytes. */
        if(s->expected_bytes == 0 || s->bytes_received == s->expected_bytes) {
            success = true;
            err = 0;
        } else {
            err = 0xFFF4; /* short read */
        }
    } else if(got & MOON_BULK_EVT_COC_ERROR) {
        err = s->error_code ? s->error_code : 0xFFF5;
    } else {
        /* Timeout — attempt to tear down the channel. */
        if(s->coc_channel_valid) {
            ble_l2cap_coc_disconnect(s->coc_channel_index);
        }
        err = 0xFFF6;
    }

cleanup:
    if(out_bytes_received) *out_bytes_received = s->bytes_received;
    if(out_error_code) *out_error_code = err;

    furi_mutex_acquire(moon->mutex, FuriWaitForever);
    moon->active_bulk = NULL;
    furi_mutex_release(moon->mutex);

    ble_l2cap_coc_set_callback(conn_handle, NULL, NULL);
    furi_event_flag_free(s->event);
    free(s);
    return success;
}

/* ── Service entry point ──────────────────────────────────────── */

int32_t moon_companion_srv(void* p) {
    UNUSED(p);

    MoonCompanion* moon = malloc(sizeof(MoonCompanion));
    memset(moon, 0, sizeof(*moon));

    moon->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    moon->queue = furi_message_queue_alloc(16, sizeof(MoonMsg));
    moon->state = MoonConnStateDisconnected;
    moon->reconnect_timer =
        furi_timer_alloc(moon_companion_reconnect_tick, FuriTimerTypeOnce, moon);

    moon_companion_load(moon);

    moon->ble = moon_ble_alloc();
    moon_ble_set_state_callback(moon->ble, moon_companion_on_ble_state, moon);
    moon_ble_set_rx_callback(moon->ble, moon_companion_on_rpc_rx, moon);

    /* Note on boot ordering: do NOT call ble_l2cap_coc_init() (or any
     * other ACI call) here. This thread runs concurrently with bt_srv,
     * and gap_init() — which also calls ble_event_dispatcher_init() —
     * hasn't finished yet. Touching the event dispatcher at this point
     * dereferences NULL and boot-loops the whole device. We lazily
     * init the CoC layer inside moon_ble_start_scan() (and lazily
     * init the GATT client there too), which only runs once the user
     * or auto-reconnect triggers a connection attempt — by which time
     * BT is fully up. */

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
            /* Only tear the link down if pairing never completed. A
             * successful pair left persist.paired = true; in that case
             * we want the connection to survive the pair-scene exit so
             * other apps can pull position / time / notifications. */
            if(!moon->persist.paired) {
                furi_timer_stop(moon->reconnect_timer);
                moon_ble_stop(moon->ble);
            }
            break;

        case MoonMsgForget:
            memset(&moon->persist, 0, sizeof(moon->persist));
            moon_companion_save(moon);
            furi_timer_stop(moon->reconnect_timer);
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
            furi_timer_stop(moon->reconnect_timer);
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

        case MoonMsgReconnect:
            /* The reconnect timer fired — try to re-establish the link
             * if we still should. Bail if the user forgot the phone in
             * the meantime, yielded the radio, or we're already back up
             * (a concurrent event could have raced us). */
            if(moon->persist.paired && !moon->yielded && !moon->pairing_active &&
               moon_ble_get_state(moon->ble) == MoonBleStateIdle) {
                FURI_LOG_I(TAG, "Auto-reconnect: re-scanning for paired phone");
                moon_ble_start_scan(moon->ble);
            }
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
                /* Link is back up — cancel any pending reconnect timer
                 * so we don't kick off a redundant scan. */
                furi_timer_stop(moon->reconnect_timer);
                if(moon->pairing_active) {
                    moon_companion_on_pair_connected(moon);
                } else if(moon->persist.paired) {
                    moon_companion_on_authed_connected(moon);
                }
            } else if(
                (msg.data.ble_state == MoonBleStateIdle ||
                 msg.data.ble_state == MoonBleStateError) &&
                moon->persist.paired && !moon->yielded && !moon->pairing_active) {
                /* Paired phone fell off the link. Arm the reconnect timer
                 * rather than immediately re-scanning, so a flaky peer or
                 * out-of-range phone doesn't turn the service into a tight
                 * scan/connect/fail loop. */
                FURI_LOG_I(TAG,
                           "Link lost (ble_state=%d) — reconnect in %d ms",
                           msg.data.ble_state,
                           MOON_COMPANION_RECONNECT_DELAY_MS);
                furi_timer_start(
                    moon->reconnect_timer,
                    furi_ms_to_ticks(MOON_COMPANION_RECONNECT_DELAY_MS));
            }
            break;
        }
    }
    return 0;
}
