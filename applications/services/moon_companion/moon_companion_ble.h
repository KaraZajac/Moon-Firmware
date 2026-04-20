#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * BLE central wrapper for Moon Companion service.
 *
 * Maintains a small state machine that scans for adverts claiming the
 * Moon Companion service UUID, connects to the matching phone, runs a
 * GATT discovery pass to locate the RPC characteristics, enables
 * notifications on RPC_RX, and surfaces received packets back to the
 * service thread.
 *
 * State transitions are driven by:
 *   - External commands from the service thread (start/stop/scan/connect)
 *   - GATT/GAP events delivered by the BLE stack
 *
 * All callbacks are invoked from the BT service thread context — the
 * receiver should hand work off to its own thread if it takes more than
 * a few hundred µs.
 */

typedef enum {
    MoonBleStateIdle,        /* not scanning, not connected */
    MoonBleStateScanning,    /* looking for our service UUID */
    MoonBleStateConnecting,  /* GAP create_connection in flight */
    MoonBleStatePairing,     /* SMP Just-Works + bonding in flight */
    MoonBleStateDiscovering, /* GATT primary / char discovery */
    MoonBleStateConnected,   /* RPC up, notifications enabled */
    MoonBleStateError,       /* transient — will auto-recover */
} MoonBleState;

typedef struct MoonBle MoonBle;

/* RPC payload delivered from RPC_RX notifications. `data` is valid only
 * during the callback; copy if you need it longer. */
typedef void (*MoonBleRxCallback)(const uint8_t* data, size_t len, void* ctx);

/* State-change notification. Fires on every transition. */
typedef void (*MoonBleStateCallback)(MoonBleState state, void* ctx);

/* ── Service UUIDs (128-bit, little-endian-when-on-wire) ──────────────
 * Generated randomly; no affiliation with any published SIG service.
 * Kept as string form here for readability; moon_companion_ble.c parses
 * them into the stack's native little-endian byte order. */
#define MOON_BLE_SERVICE_UUID "df98ad38-b0b8-44c0-bd88-905db2d6b365"
#define MOON_BLE_RPC_TX_UUID  "7a30f8d0-2a4e-43b7-a383-1f786173991b"
#define MOON_BLE_RPC_RX_UUID  "902dac74-55ba-46d4-8ff4-6d40893ae052"

MoonBle* moon_ble_alloc(void);
void     moon_ble_free(MoonBle* ble);

void moon_ble_set_rx_callback(MoonBle* ble, MoonBleRxCallback cb, void* ctx);
void moon_ble_set_state_callback(MoonBle* ble, MoonBleStateCallback cb, void* ctx);

/* Begin scanning for our service UUID. Transitions Idle → Scanning.
 * Safe to call from Idle or Error; no-op otherwise. */
bool moon_ble_start_scan(MoonBle* ble);

/* Skip scanning and initiate a connection directly to a known bonded
 * peer by its identity address. The controller scans internally and
 * resolves any RPA the peer is advertising with against the HCI
 * resolving list, then reuses the stored LTK — so no fresh SMP runs
 * and the phone side sees no pair prompt.
 *
 * identity_addr_type: 0 = public identity, 1 = random static identity
 * (as returned by gap_get_bonded_peer).
 *
 * Safe to call from Idle or Error; no-op otherwise. Intended for the
 * auto-reconnect path after Flipper boot when persist.phone_mac holds
 * a previously-captured identity.
 */
bool moon_ble_start_reconnect(
    MoonBle* ble,
    const uint8_t identity_addr[6],
    uint8_t identity_addr_type);

/* Stop whatever is happening; tear down connection if connected.
 * Transitions to Idle. */
void moon_ble_stop(MoonBle* ble);

/* Send a packet over RPC_TX. Returns false if not Connected, if the
 * packet exceeds the negotiated MTU, or if the GATT write fails. */
bool moon_ble_send(MoonBle* ble, const uint8_t* data, size_t len);

MoonBleState moon_ble_get_state(MoonBle* ble);

/* Return the active peripheral connection handle, or 0 if not connected.
 * Exposed so app-level transports (e.g. L2CAP CoC for bulk transfers)
 * can be opened on top of the same link. */
uint16_t moon_ble_get_connection_handle(MoonBle* ble);
