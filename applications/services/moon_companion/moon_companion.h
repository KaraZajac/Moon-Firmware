#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Moon Companion — system service that bridges the Flipper to a paired
 * Android phone over BLE (Flipper is central, phone is peripheral). Apps
 * use this service to pull location, time, and notification delivery
 * without handling any BLE themselves.
 *
 * Example:
 *
 *   MoonCompanion* moon = furi_record_open(RECORD_MOON_COMPANION);
 *   MoonPosition pos;
 *   if(moon_companion_get_position(moon, &pos, 5000)) {
 *       geo_tag_my_capture(pos.lat_e7, pos.lon_e7);
 *   }
 *   furi_record_close(RECORD_MOON_COMPANION);
 */

#define RECORD_MOON_COMPANION "moon_companion"

typedef struct MoonCompanion MoonCompanion;

/* ── Types ─────────────────────────────────────────────────────────── */

typedef enum {
    MoonConnStateDisconnected,  /* no paired phone in range, or unpaired */
    MoonConnStateScanning,      /* looking for paired phone */
    MoonConnStateConnecting,    /* handshake in flight */
    MoonConnStateConnected,     /* RPC available */
    MoonConnStateYielded,       /* another app requested exclusive BLE */
} MoonConnectionState;

typedef enum {
    MoonFixNone = 0,
    MoonFix2D   = 1,
    MoonFix3D   = 2,
} MoonFixQuality;

typedef struct {
    int32_t  lat_e7;         /* latitude  × 1e7 */
    int32_t  lon_e7;         /* longitude × 1e7 */
    int32_t  alt_mm;         /* altitude above WGS-84, mm */
    uint32_t accuracy_mm;    /* 68 %-CEP horizontal accuracy */
    uint32_t speed_mmps;     /* speed over ground, mm/s */
    uint32_t heading_cdeg;   /* 0 – 35999 (0.01°) */
    uint64_t timestamp_ms;   /* Unix ms at which the fix was taken */
    uint32_t tick;           /* furi_get_tick() when this reached us */
    uint8_t  source_id[8];   /* stable id of upstream source; all-zero = unknown */
    MoonFixQuality fix_quality;
    uint8_t  satellites;
} MoonPosition;

typedef struct {
    uint64_t unix_ms;
    int32_t  tz_offset_seconds;
    char     tz_name[48];
} MoonTime;

typedef enum {
    MoonPriorityNormal = 0,
    MoonPriorityHigh   = 1,
    MoonPriorityUrgent = 2,
} MoonNotificationPriority;

/* Push-style subscription callback. Invoked from the Moon Companion
 * service thread, so callers should either do minimal work here and
 * post to their own queue, or keep it fast. */
typedef void (*MoonPositionCallback)(const MoonPosition* pos, void* context);

/* ── Connection state ──────────────────────────────────────────────── */

MoonConnectionState moon_companion_get_state(MoonCompanion* moon);
bool                moon_companion_is_paired(MoonCompanion* moon);

/* ── Pairing ───────────────────────────────────────────────────────── */

/* Begin a pairing flow. Writes a 6-digit code into pin_out (null-terminated,
 * 7 bytes) that the user types into the Android app. Returns false if
 * pairing cannot start right now (e.g., already paired and forget() not
 * called, or no phone in range yet). */
bool moon_companion_begin_pairing(MoonCompanion* moon, char pin_out[7]);

/* Abort a pairing flow in progress. Safe to call at any time. */
void moon_companion_cancel_pairing(MoonCompanion* moon);

/* Forget the currently paired phone. Deletes the stored auth token; the
 * next begin_pairing call will start fresh. */
void moon_companion_forget(MoonCompanion* moon);

/* ── GPS ───────────────────────────────────────────────────────────── */

/* Pull the most recent position. Returns true and fills *out only if a
 * fix exists and is no older than max_age_ms. Pass max_age_ms = 0 to
 * accept any cached fix regardless of age. */
bool moon_companion_get_position(
    MoonCompanion* moon,
    MoonPosition* out,
    uint32_t max_age_ms);

/* Push subscription: cb is invoked every time a fresh PositionData
 * arrives from the phone. Multiple subscribers are allowed. Idempotent
 * by (cb, ctx) — calling twice with the same pair registers once. */
void moon_companion_subscribe_position(
    MoonCompanion* moon,
    MoonPositionCallback cb,
    void* ctx);

void moon_companion_unsubscribe_position(
    MoonCompanion* moon,
    MoonPositionCallback cb,
    void* ctx);

/* ── Time ──────────────────────────────────────────────────────────── */

bool moon_companion_get_time(MoonCompanion* moon, MoonTime* out);

/* ── HTTP proxy ────────────────────────────────────────────────────── */

typedef struct {
    const char* key;
    const char* value;
} MoonHttpHeader;

typedef struct {
    const char* method;                 /* NULL → "GET" */
    const char* url;                    /* required, full scheme */
    const MoonHttpHeader* headers;      /* NULL OK when headers_count == 0 */
    size_t headers_count;
    const uint8_t* body;                /* NULL OK when body_len == 0 */
    size_t body_len;
    uint32_t timeout_ms;                /* 0 → phone default (30 s) */
} MoonHttpRequest;

typedef struct {
    char   key[32];
    char   value[96];
} MoonHttpHeaderOut;

typedef struct {
    uint32_t status_code;
    MoonHttpHeaderOut* headers;         /* owned by service; freed on _response_free */
    size_t headers_count;
    uint8_t* body;                      /* inline body */
    size_t body_len;
} MoonHttpResponse;

/* Issue an HTTP(S) request through the paired phone. The phone terminates
 * TLS using the system trust store.
 *
 * On success (return true): resp_out is populated and ownership of its
 * allocated fields (body, headers) transfers to the caller — call
 * moon_companion_http_response_free when done.
 *
 * Inline-only: response body has to fit in a single GATT notification
 * (~150 B after envelope overhead). Anything bigger comes back as
 * MOON_NOT_AVAILABLE — there is no bulk-streaming fallback because our
 * radio-stack variant doesn't ship L2CAP CoC and we don't do
 * GATT-chunked reassembly yet.
 *
 * On failure (return false): resp_out->status_code reflects any HTTP
 * status code the phone managed to return; other fields may be unset.
 * Do not call _response_free if this returns false.
 *
 * Do not call from the Moon Companion service thread — it would deadlock
 * on the response RPC. Safe from any app thread. */
bool moon_companion_http_request(
    MoonCompanion* moon,
    const MoonHttpRequest* req,
    MoonHttpResponse* resp_out);

void moon_companion_http_response_free(MoonHttpResponse* resp);

/* ── Notifications (Flipper → phone → user) ────────────────────────── */

/* Asks the phone to show a notification to its user (e.g., "Flipper:
 * Flock camera 50 m ahead"). Returns true if the request reached the
 * phone; delivery to the user is not guaranteed (phone OS may suppress). */
bool moon_companion_send_notification(
    MoonCompanion* moon,
    const char* title,
    const char* body,
    MoonNotificationPriority priority);

/* ── Yield (for apps that need exclusive BLE) ──────────────────────── */

/* Request that Moon Companion release its central BLE link for up to
 * duration_ms milliseconds so the caller can use the radio exclusively
 * (e.g., BitChat dual-role mesh). Safe to call while already yielded —
 * extends the window. Cached position / time remain valid and queryable
 * while yielded. */
void moon_companion_yield(MoonCompanion* moon, uint32_t duration_ms);

/* Resume Moon Companion before the yield window expires. */
void moon_companion_resume(MoonCompanion* moon);

#ifdef __cplusplus
}
#endif
