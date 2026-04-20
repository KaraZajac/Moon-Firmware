#include "moon_companion_ble.h"

#include <furi.h>
#include <furi_hal.h>
#include <string.h>
#include <stdlib.h>

#include <gap.h>
#include <furi_ble/gatt_client.h>
#include <ble/ble.h>

#define TAG "MoonBle"

#define MOON_BLE_TICK_MS         100
#define MOON_BLE_CONNECT_TIMEOUT (5000 / MOON_BLE_TICK_MS)
#define MOON_BLE_PAIR_DELAY      5  /* send Pair Request this many ticks after connect */
/* Fallback timer — if the stack never fires pairing-complete (peer already
 * bonded via stored LTK, or peer refuses to pair at all), we still need to
 * make forward progress. Real Just-Works pairing with a modern Android
 * takes 3-5 s; 8 s gives comfortable headroom. The pairing-complete event
 * from gap.c short-circuits this when it arrives. */
#define MOON_BLE_PAIR_FALLBACK   80
/* Discovery timing — see the ATT-slot-contention block below. The 5 s
 * delay lets Android finish its own opportunistic sweep of the Flipper's
 * peripheral GATT server before we try to claim the ATT slot; retrying
 * faster just generates 0x0C noise. */
#define MOON_BLE_DISCOVER_DELAY  50  /* first discover attempt, 5 s into Discovering */
#define MOON_BLE_DISCOVER_RETRY  10  /* retry once per second if still contended */

/* Background-friendly connection parameters.
 * Flipper is central, so it owns the interval. Default gap_connect() uses
 * 50-70 ms which Android aggressively drops in Doze; apply a looser profile
 * once the link is fully up. Units:
 *   Conn_Interval     * 1.25 ms   (80 = 100 ms, 200 = 250 ms)
 *   Slave_Latency     in events
 *   Supervision_Timeout * 10 ms   (400 = 4 s)
 * Timeout must be > (1 + latency) * interval_max * 2; at 250 ms/0 latency
 * that's 500 ms, so 4 s is plenty.
 */
#define MOON_BLE_CONN_INT_MIN   0x0050  /* 100 ms */
#define MOON_BLE_CONN_INT_MAX   0x00C8  /* 250 ms */
#define MOON_BLE_CONN_LATENCY   0x0000
#define MOON_BLE_CONN_TIMEOUT   0x0190  /* 4 s */
/* Android's system-level GATT clients (MCP, CCS, TMAP, HAS, etc.) all kick
 * off opportunistic discovery against the Flipper's peripheral GATT server
 * as soon as the link is up. The STM32WB has one ATT procedure slot per
 * connection — while Android is walking our peripheral tree, our central-
 * side disc_primary_service_by_uuid returns 0x0C (COMMAND_DISALLOWED).
 * Observed Android to take 3-6 s to finish its initial sweep on a modern
 * phone, so MOON_BLE_DISCOVER_DELAY intentionally waits that out before
 * the first attempt. Overall timeout covers the slow-peer tail — if we
 * haven't seen services after 40 s we declare the peer unreachable. */
#define MOON_BLE_DISCOVER_TIMEOUT 400

/* ── UUID helpers ──────────────────────────────────────────────────────
 *
 * BLE stacks on-air use little-endian 128-bit UUIDs. The MOON_BLE_*_UUID
 * constants in the header are printable big-endian strings; parse them
 * once at init and keep a reversed 16-byte buffer ready for memcmp and
 * for passing to gap_connect() / discover filters. */

static bool parse_uuid_128(const char* s, uint8_t out[16]) {
    uint8_t tmp[16] = {0};
    uint8_t byte_idx = 0;
    uint8_t nibble = 0;
    uint8_t val = 0;
    for(const char* p = s; *p && byte_idx < 16; p++) {
        if(*p == '-') continue;
        uint8_t n;
        if(*p >= '0' && *p <= '9') n = *p - '0';
        else if(*p >= 'a' && *p <= 'f') n = 10 + (*p - 'a');
        else if(*p >= 'A' && *p <= 'F') n = 10 + (*p - 'A');
        else return false;
        val = (uint8_t)((val << 4) | n);
        if(++nibble == 2) {
            tmp[byte_idx++] = val;
            val = 0;
            nibble = 0;
        }
    }
    if(byte_idx != 16) return false;
    /* Reverse: wire format is little-endian (low byte first). */
    for(uint8_t i = 0; i < 16; i++) out[i] = tmp[15 - i];
    return true;
}

/* ── Internal state ────────────────────────────────────────────────────
 *
 * All fields guarded by `mutex` except during pure-read public accessors
 * which can race benignly. The main state machine advances on the tick
 * timer, which fires on the FuriTimer thread — not the BT stack thread
 * that delivers scan/GATT callbacks. Callbacks therefore only *set*
 * fields (under mutex); the tick thread reads and acts. */

struct MoonBle {
    FuriMutex* mutex;
    FuriTimer* tick_timer;

    uint8_t svc_uuid[16];    /* little-endian (wire order) */
    uint8_t rpc_tx_uuid[16]; /* little-endian */
    uint8_t rpc_rx_uuid[16]; /* little-endian */

    MoonBleState state;
    uint16_t connection_handle;
    uint16_t rpc_tx_handle;
    uint16_t rpc_rx_handle;

    /* Discovery bookkeeping. Populated by the GATT callback on the BLE
     * thread; consumed by the tick thread during state transitions. */
    BleGattService* services;
    uint8_t service_count;
    BleGattCharacteristic* chars;
    uint8_t char_count;

    /* Scan target — first advert that matches svc_uuid wins. Cleared
     * by the tick when it acts on it. */
    uint8_t  scan_addr[6];
    uint8_t  scan_addr_type;
    bool     scan_target_found;

    /* Transition flags set by BLE callbacks, consumed by tick. */
    bool     services_ready;
    bool     chars_ready;
    bool     write_complete;
    bool     subscribe_complete;
    bool     gatt_error;
    /* Central-role SMP pairing-complete event. Set by the gap.c hook we
     * register in moon_ble_start_scan; consumed by the tick handler in
     * MoonBleStatePairing to short-circuit the fallback timer. `pairing_status`
     * carries the SMP status code (0 = success, non-zero = SMP failure). */
    bool     pairing_complete;
    uint8_t  pairing_status;
    /* Once-per-connection latch for the looser connection-param update.
     * Applied after we reach MoonBleStateConnected to survive Android Doze. */
    bool     conn_params_loose_applied;

    /* Timeout / retry counter, in ticks. Reset on every state change. */
    uint16_t phase_ticks;

    /* Consumer callbacks. */
    MoonBleRxCallback    rx_cb;
    void*                rx_ctx;
    MoonBleStateCallback state_cb;
    void*                state_ctx;
};

/* ── Forward declarations ──────────────────────────────────────────── */

static void moon_ble_tick(void* ctx);
static void moon_ble_scan_callback(GapScanResultData* result, void* context);
static void moon_ble_gatt_callback(BleGattClientEvent* event, void* context);
static void moon_ble_transition(MoonBle* ble, MoonBleState next);
static void moon_ble_on_central_pairing_complete(
    uint16_t connection_handle, uint8_t status, void* context);

/* ── Alloc / free ─────────────────────────────────────────────────── */

MoonBle* moon_ble_alloc(void) {
    MoonBle* ble = malloc(sizeof(MoonBle));
    memset(ble, 0, sizeof(*ble));

    ble->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    ble->tick_timer = furi_timer_alloc(moon_ble_tick, FuriTimerTypePeriodic, ble);

    ble->services = malloc(sizeof(BleGattService) * BLE_GATT_CLIENT_MAX_SERVICES);
    ble->chars    = malloc(sizeof(BleGattCharacteristic) * BLE_GATT_CLIENT_MAX_CHARS);

    if(!parse_uuid_128(MOON_BLE_SERVICE_UUID, ble->svc_uuid) ||
       !parse_uuid_128(MOON_BLE_RPC_TX_UUID, ble->rpc_tx_uuid) ||
       !parse_uuid_128(MOON_BLE_RPC_RX_UUID, ble->rpc_rx_uuid)) {
        FURI_LOG_E(TAG, "UUID parse failure — service will not start");
    }

    ble->state = MoonBleStateIdle;
    return ble;
}

void moon_ble_free(MoonBle* ble) {
    if(!ble) return;
    moon_ble_stop(ble);
    furi_timer_free(ble->tick_timer);
    furi_mutex_free(ble->mutex);
    free(ble->services);
    free(ble->chars);
    free(ble);
}

/* ── Consumer-facing setters / getters ────────────────────────────── */

void moon_ble_set_rx_callback(MoonBle* ble, MoonBleRxCallback cb, void* ctx) {
    furi_check(ble);
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    ble->rx_cb = cb;
    ble->rx_ctx = ctx;
    furi_mutex_release(ble->mutex);
}

void moon_ble_set_state_callback(MoonBle* ble, MoonBleStateCallback cb, void* ctx) {
    furi_check(ble);
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    ble->state_cb = cb;
    ble->state_ctx = ctx;
    furi_mutex_release(ble->mutex);
}

MoonBleState moon_ble_get_state(MoonBle* ble) {
    furi_check(ble);
    return ble->state; /* single-word read, no mutex needed */
}

uint16_t moon_ble_get_connection_handle(MoonBle* ble) {
    furi_check(ble);
    /* Single-word read — worst case returns 0 if the field is being
     * written concurrently, which just means "not connected yet." */
    return ble->connection_handle;
}

/* ── State machine entry points ──────────────────────────────────── */

bool moon_ble_start_scan(MoonBle* ble) {
    furi_check(ble);
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    if(ble->state != MoonBleStateIdle && ble->state != MoonBleStateError) {
        furi_mutex_release(ble->mutex);
        return false;
    }
    ble->scan_target_found = false;
    ble->services_ready = false;
    ble->chars_ready = false;
    ble->write_complete = false;
    ble->subscribe_complete = false;
    ble->gatt_error = false;
    ble->pairing_complete = false;
    ble->pairing_status = 0;
    ble->conn_params_loose_applied = false;
    ble->phase_ticks = 0;
    furi_mutex_release(ble->mutex);

    /* Make sure the radio is free — a stale central link would cause
     * gap_start_scanning to return HCI_COMMAND_DISALLOWED. */
    if(gap_get_state() == GapStateConnected) {
        uint16_t h = gap_get_connection_handle_by_role(true);
        if(h) gap_disconnect(h);
        for(int i = 0; i < 20; i++) {
            furi_delay_ms(50);
            if(gap_get_state() != GapStateConnected) break;
        }
    }

    ble_gatt_client_init();
    /* Configure Just-Works pairing + bonding. Without this, macOS Core
     * Bluetooth refuses to expose custom services to an unpaired central
     * — service discovery returns ATT 0x0A (Attribute Not Found) for
     * our UUID even though the peer has it registered. Bonding also
     * means subsequent reconnects reuse the LTK, so the user never sees
     * a pairing prompt after the first time. */
    gap_set_just_works_pairing();
    /* Event-driven transition out of MoonBleStatePairing — without this,
     * the state machine relies on a fixed fallback timer and discovery
     * races the still-in-flight SMP exchange, hammering out 0x0C errors
     * until pairing actually settles. See moon_ble_on_central_pairing_complete. */
    gap_set_central_pairing_complete_callback(moon_ble_on_central_pairing_complete, ble);
    gap_set_scan_callback(moon_ble_scan_callback, ble);

    GapScanParams params = {
        .interval   = 0x60,
        .window     = 0x30,
        .active     = true,
        .timeout_ms = 0, /* we drive our own retry via tick */
    };
    if(!gap_start_scanning(&params)) {
        FURI_LOG_E(TAG, "gap_start_scanning failed (radio busy?)");
        gap_set_scan_callback(NULL, NULL);
        moon_ble_transition(ble, MoonBleStateError);
        return false;
    }

    moon_ble_transition(ble, MoonBleStateScanning);
    furi_timer_start(ble->tick_timer, furi_ms_to_ticks(MOON_BLE_TICK_MS));
    return true;
}

void moon_ble_stop(MoonBle* ble) {
    furi_check(ble);
    furi_timer_stop(ble->tick_timer);

    if(gap_get_state() == GapStateScanning) gap_stop_scanning();
    gap_set_scan_callback(NULL, NULL);
    gap_set_central_pairing_complete_callback(NULL, NULL);

    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    uint16_t h = ble->connection_handle;
    ble->connection_handle = 0;
    ble->rpc_tx_handle = 0;
    ble->rpc_rx_handle = 0;
    furi_mutex_release(ble->mutex);

    if(h) {
        ble_gatt_client_set_callback(h, NULL, NULL);
        gap_disconnect(h);
    }

    moon_ble_transition(ble, MoonBleStateIdle);
}

bool moon_ble_send(MoonBle* ble, const uint8_t* data, size_t len) {
    furi_check(ble);
    furi_check(data);
    if(ble->state != MoonBleStateConnected) return false;
    if(len == 0 || len > 244) return false; /* single-notification budget */
    return ble_gatt_client_write(ble->connection_handle, ble->rpc_tx_handle, data, (uint16_t)len);
}

/* ── BLE scan callback ────────────────────────────────────────────
 *
 * Called from the BT service thread whenever the radio receives a valid
 * advert while scanning. Walks the AD records looking for a 128-bit UUID
 * list (0x06 / 0x07) containing MOON_BLE_SERVICE_UUID. First match wins
 * — stores target in `ble` and stops the scan. Tick thread picks it up
 * and initiates the connection. */

static void moon_ble_scan_callback(GapScanResultData* result, void* context) {
    MoonBle* ble = context;
    if(!result->data || result->data_len == 0) return;

    bool match = false;
    uint8_t pos = 0;
    while(pos < result->data_len) {
        uint8_t len = result->data[pos];
        if(len == 0 || pos + len >= result->data_len) break;
        uint8_t type = result->data[pos + 1];
        if((type == 0x06 || type == 0x07) && len >= 17) {
            for(uint8_t i = 0; i + 15 < len - 1; i += 16) {
                if(memcmp(&result->data[pos + 2 + i], ble->svc_uuid, 16) == 0) {
                    match = true;
                    break;
                }
            }
        }
        if(match) break;
        pos += len + 1;
    }
    if(!match) return;

    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    if(!ble->scan_target_found) {
        memcpy(ble->scan_addr, result->address, 6);
        ble->scan_addr_type = result->address_type;
        ble->scan_target_found = true;
        gap_stop_scanning();
        FURI_LOG_I(TAG, "Moon phone found: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
            result->address[5], result->address[4], result->address[3],
            result->address[2], result->address[1], result->address[0],
            result->rssi);
    }
    furi_mutex_release(ble->mutex);
}

/* ── GATT callback ────────────────────────────────────────────────
 *
 * Delivered by ble_gatt_client on the BT service thread once we've
 * registered via ble_gatt_client_set_callback. We only *copy* the
 * event data into our struct and set flags; the tick thread acts on
 * them. */

static void moon_ble_gatt_callback(BleGattClientEvent* event, void* context) {
    MoonBle* ble = context;
    switch(event->type) {
    case BleGattClientEventDiscoverComplete:
        furi_mutex_acquire(ble->mutex, FuriWaitForever);
        ble->service_count = event->discover.count > BLE_GATT_CLIENT_MAX_SERVICES ?
            BLE_GATT_CLIENT_MAX_SERVICES : event->discover.count;
        memcpy(ble->services, event->discover.services,
               ble->service_count * sizeof(BleGattService));
        ble->services_ready = true;
        furi_mutex_release(ble->mutex);
        break;

    case BleGattClientEventCharDiscoverComplete:
        furi_mutex_acquire(ble->mutex, FuriWaitForever);
        ble->char_count = event->char_discover.count > BLE_GATT_CLIENT_MAX_CHARS ?
            BLE_GATT_CLIENT_MAX_CHARS : event->char_discover.count;
        memcpy(ble->chars, event->char_discover.chars,
               ble->char_count * sizeof(BleGattCharacteristic));
        ble->chars_ready = true;
        furi_mutex_release(ble->mutex);
        break;

    case BleGattClientEventWriteComplete:
        furi_mutex_acquire(ble->mutex, FuriWaitForever);
        /* Subscribe completion is indistinguishable from a regular write
         * ACK at this layer — the state machine interprets by phase. */
        ble->write_complete = true;
        ble->subscribe_complete = true;
        furi_mutex_release(ble->mutex);
        break;

    case BleGattClientEventNotification: {
        MoonBleRxCallback cb;
        void* ctx;
        furi_mutex_acquire(ble->mutex, FuriWaitForever);
        cb = ble->rx_cb;
        ctx = ble->rx_ctx;
        furi_mutex_release(ble->mutex);
        if(cb && event->notification.data_len > 0) {
            /* Caller is responsible for draining quickly — we're on the
             * BT thread. First fragment bit is in the top bit of offset;
             * for Phase 1 we require payloads fit in a single notification,
             * so ignore fragmentation. */
            cb(event->notification.data, event->notification.data_len, ctx);
        }
        break;
    }

    case BleGattClientEventError:
        /* Don't treat arbitrary GATT errors as fatal. On a dual-role
         * link the peer often issues its own reads against us (device
         * name, appearance, etc.) that land here as 0x0A Attribute Not
         * Found — has nothing to do with our central-side discovery.
         * Rely on the discovery timeout to catch a genuinely stuck
         * state instead. */
        FURI_LOG_D(TAG, "GATT error %02x (ignored, not state-bearing)",
                   event->error.error_code);
        break;

    default:
        break;
    }
}

/* ── State transition helper ───────────────────────────────────── */

static void moon_ble_transition(MoonBle* ble, MoonBleState next) {
    MoonBleStateCallback cb = NULL;
    void* ctx = NULL;
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    if(ble->state != next) {
        ble->state = next;
        ble->phase_ticks = 0;
        cb = ble->state_cb;
        ctx = ble->state_ctx;
    }
    furi_mutex_release(ble->mutex);
    if(cb) cb(next, ctx);
}

/* ── Central-role pairing-complete hook ──────────────────────────
 *
 * Registered with gap.c via gap_set_central_pairing_complete_callback.
 * Fires on the BLE event thread when SMP pairing completes (success or
 * failure). The tick thread picks up the flag at the next 100 ms boundary
 * and transitions out of MoonBleStatePairing without waiting on the
 * fallback timer. Only updates a flag + status; keeps the callback cheap
 * and avoids cross-thread reentry into gap_*. */

static void moon_ble_on_central_pairing_complete(
    uint16_t connection_handle,
    uint8_t status,
    void* context) {
    MoonBle* ble = context;
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    /* Stack can deliver pairing-complete for a stale handle if a second
     * link got paired before we cleaned up; guard against that. */
    if(ble->connection_handle != 0 && connection_handle == ble->connection_handle) {
        ble->pairing_complete = true;
        ble->pairing_status = status;
    }
    furi_mutex_release(ble->mutex);
}

/* ── Tick handler: drive the state machine ─────────────────────── */

static void moon_ble_tick(void* ctx) {
    MoonBle* ble = ctx;
    furi_mutex_acquire(ble->mutex, FuriWaitForever);
    MoonBleState s = ble->state;
    ble->phase_ticks++;
    uint16_t ticks = ble->phase_ticks;
    bool found = ble->scan_target_found;
    bool services_ready = ble->services_ready;
    bool chars_ready = ble->chars_ready;
    bool subscribe_complete = ble->subscribe_complete;
    bool pairing_complete = ble->pairing_complete;
    uint8_t pairing_status = ble->pairing_status;
    furi_mutex_release(ble->mutex);

    switch(s) {
    case MoonBleStateScanning:
        if(found) {
            uint8_t addr[6];
            uint8_t addr_type;
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            memcpy(addr, ble->scan_addr, 6);
            addr_type = ble->scan_addr_type;
            furi_mutex_release(ble->mutex);

            gap_set_scan_callback(NULL, NULL);
            moon_ble_transition(ble, MoonBleStateConnecting);
            if(!gap_connect(addr_type, addr)) {
                FURI_LOG_E(TAG, "gap_connect returned false");
                moon_ble_transition(ble, MoonBleStateError);
            }
        }
        break;

    case MoonBleStateConnecting: {
        uint16_t h = gap_get_connection_handle_by_role(true);
        if(h != 0) {
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            ble->connection_handle = h;
            furi_mutex_release(ble->mutex);
            ble_gatt_client_set_callback(h, moon_ble_gatt_callback, ble);
            moon_ble_transition(ble, MoonBleStatePairing);
        } else if(ticks >= MOON_BLE_CONNECT_TIMEOUT) {
            FURI_LOG_E(TAG, "Connect timeout");
            moon_ble_transition(ble, MoonBleStateError);
        }
        break;
    }

    case MoonBleStatePairing:
        /* Fire the SMP Pairing Request a few ticks after connect so the
         * link-layer connection is fully established. macOS Core Bluetooth
         * (and Android, for our custom service) refuse GATT discovery until
         * the link is bonded. Subsequent reconnects reuse the stored LTK
         * and gap_pair returns early with no SMP exchange.
         *
         * We transition out of this state on either:
         *   a) pairing_complete from the gap.c hook (success or SMP failure), or
         *   b) MOON_BLE_PAIR_FALLBACK ticks elapsed (peer never fires the event —
         *      e.g. already-bonded re-connect where the stack skips SMP).
         *
         * Discovery must NOT fire during the SMP exchange — the STM32WB ATT
         * engine has one outstanding-request slot and returns 0x0C
         * (COMMAND_DISALLOWED) while pairing is in flight. */
        if(ticks == MOON_BLE_PAIR_DELAY) {
            if(!gap_pair(ble->connection_handle, false)) {
                /* 0x0C here usually means the peer has already initiated
                 * pairing autonomously (stack-internal). Not fatal — we'll
                 * still receive pairing_complete when the SMP exchange
                 * settles. */
                FURI_LOG_D(TAG, "gap_pair returned false — peer may be driving SMP");
            }
        }
        if(gap_get_connection_handle_by_role(true) == 0) {
            FURI_LOG_W(TAG, "Peer disconnected during pairing");
            moon_ble_stop(ble);
            break;
        }
        if(pairing_complete && pairing_status != 0) {
            /* SMP failure — there's no recovery path short of a fresh pair
             * attempt from the user, which the caller will drive. */
            FURI_LOG_E(TAG, "Pairing failed (SMP status 0x%02X)", pairing_status);
            moon_ble_transition(ble, MoonBleStateError);
            break;
        }
        if(pairing_complete || ticks >= MOON_BLE_PAIR_FALLBACK) {
            FURI_LOG_I(
                TAG,
                "%s, starting MTU + discovery",
                pairing_complete ? "Pairing complete" : "Pairing fallback timer");
            ble_gatt_client_exchange_mtu(ble->connection_handle);
            moon_ble_transition(ble, MoonBleStateDiscovering);
        }
        break;

    case MoonBleStateDiscovering:
        /* MTU exchange and service discovery share the single outstanding
         * ATT request slot — kick off discover only once MTU has settled.
         * Some peers (notably `bless` on macOS) take ≥ 600 ms to reply to
         * the MTU exchange; firing discover too soon returns 0x0C
         * (COMMAND_DISALLOWED) with no retry path. Retry every few ticks
         * until it sticks or we hit a hard timeout. */
        if(!services_ready && ble->rpc_tx_handle == 0 && ticks >= MOON_BLE_DISCOVER_DELAY &&
           ((ticks - MOON_BLE_DISCOVER_DELAY) % MOON_BLE_DISCOVER_RETRY) == 0) {
            if(!ble_gatt_client_discover_services(ble->connection_handle)) {
                FURI_LOG_W(TAG, "discover_services busy, will retry");
            }
        }

        if(ticks >= MOON_BLE_DISCOVER_TIMEOUT && !services_ready && ble->rpc_tx_handle == 0) {
            FURI_LOG_E(TAG, "Discovery timeout — tearing down");
            moon_ble_transition(ble, MoonBleStateError);
            break;
        }

        /* If the peer disconnected mid-discovery (some peers drop after
         * a 0x0C or after a slow ATT response), there's no point waiting
         * any further — bail out so the caller can retry a fresh scan. */
        if(gap_get_connection_handle_by_role(true) == 0) {
            FURI_LOG_W(TAG, "Peer disconnected during discovery");
            moon_ble_stop(ble);
            break;
        }

        if(services_ready && ble->rpc_tx_handle == 0) {
            /* Find our service, kick off char discovery. */
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            int8_t svc_idx = -1;
            for(uint8_t i = 0; i < ble->service_count; i++) {
                if(ble->services[i].uuid_type == 2 &&
                   memcmp(ble->services[i].uuid_128, ble->svc_uuid, 16) == 0) {
                    svc_idx = i;
                    break;
                }
            }
            ble->services_ready = false;
            furi_mutex_release(ble->mutex);

            if(svc_idx < 0) {
                FURI_LOG_E(TAG, "Moon Companion service UUID not found on peer");
                moon_ble_transition(ble, MoonBleStateError);
                break;
            }
            ble_gatt_client_discover_characteristics(
                ble->connection_handle, &ble->services[svc_idx]);
        }

        if(chars_ready) {
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            for(uint8_t i = 0; i < ble->char_count; i++) {
                if(ble->chars[i].uuid_type != 2) continue;
                if(memcmp(ble->chars[i].uuid_128, ble->rpc_tx_uuid, 16) == 0) {
                    ble->rpc_tx_handle = ble->chars[i].value_handle;
                } else if(memcmp(ble->chars[i].uuid_128, ble->rpc_rx_uuid, 16) == 0) {
                    ble->rpc_rx_handle = ble->chars[i].value_handle;
                }
            }
            ble->chars_ready = false;
            ble->subscribe_complete = false;
            uint16_t rx = ble->rpc_rx_handle;
            uint16_t tx = ble->rpc_tx_handle;
            furi_mutex_release(ble->mutex);

            if(rx == 0 || tx == 0) {
                FURI_LOG_E(TAG, "RPC chars missing (tx=%u rx=%u)", tx, rx);
                moon_ble_transition(ble, MoonBleStateError);
                break;
            }
            ble_gatt_client_subscribe_notifications(ble->connection_handle, rx, true);
        }

        if(subscribe_complete && ble->rpc_rx_handle != 0) {
            FURI_LOG_I(TAG, "Moon Companion RPC ready (handle tx=%u rx=%u)",
                       ble->rpc_tx_handle, ble->rpc_rx_handle);
            moon_ble_transition(ble, MoonBleStateConnected);
        }
        break;

    case MoonBleStateConnected:
        /* Once-per-connection: request looser connection parameters. The
         * stack connects at 50-70 ms interval which modern Android drops
         * in Doze; widen to 100-250 ms with a 4 s supervision timeout so
         * the link survives screen-off / background. Central-role → we
         * set the interval unilaterally via hci_le_connection_update, no
         * peer negotiation. */
        if(!ble->conn_params_loose_applied) {
            tBleStatus rc = hci_le_connection_update(
                ble->connection_handle,
                MOON_BLE_CONN_INT_MIN,
                MOON_BLE_CONN_INT_MAX,
                MOON_BLE_CONN_LATENCY,
                MOON_BLE_CONN_TIMEOUT,
                0x0010, /* min CE length */
                0x0020 /* max CE length */);
            furi_mutex_acquire(ble->mutex, FuriWaitForever);
            ble->conn_params_loose_applied = true;
            furi_mutex_release(ble->mutex);
            if(rc == BLE_STATUS_SUCCESS) {
                FURI_LOG_I(TAG, "Requested background-friendly conn params (100-250 ms)");
            } else {
                FURI_LOG_W(TAG, "hci_le_connection_update failed: 0x%02X", rc);
            }
        }

        /* Nothing to poll here — RPC work is driven by the consumer. A
         * real disconnect will show up as gap_get_connection_handle()
         * returning 0. */
        if(gap_get_connection_handle_by_role(true) == 0) {
            FURI_LOG_W(TAG, "Peer disconnected; returning to scan");
            moon_ble_stop(ble);
        }
        break;

    case MoonBleStateError:
    case MoonBleStateIdle:
        break;
    }
}
