#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <furi_hal_version.h>

#define GAP_MAC_ADDR_SIZE (6)
#define GAP_KEY_SIZE      (0x10)

/*
 * GAP helpers - background thread that handles BLE GAP events and advertising.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GapEventTypeConnected,
    GapEventTypeDisconnected,
    GapEventTypeStartAdvertising,
    GapEventTypeStopAdvertising,
    GapEventTypePinCodeShow,
    GapEventTypePinCodeVerify,
    GapEventTypeUpdateMTU,
    GapEventTypeBeaconStart,
    GapEventTypeBeaconStop,
} GapEventType;

typedef union {
    uint32_t pin_code;
    uint16_t max_packet_size;
} GapEventData;

typedef struct {
    GapEventType type;
    GapEventData data;
} GapEvent;

typedef bool (*GapEventCallback)(GapEvent event, void* context);

typedef enum {
    GapStateUninitialized,
    GapStateIdle,
    GapStateStartingAdv,
    GapStateAdvFast,
    GapStateAdvLowPower,
    GapStateConnected,
    GapStateScanning,
    GapStateConnecting,
} GapState;

typedef enum {
    GapPairingNone,
    GapPairingPinCodeShow,
    GapPairingPinCodeVerifyYesNo,
    GapPairingCount,
} GapPairing;

typedef struct {
    uint16_t conn_interval;
    uint16_t slave_latency;
    uint16_t supervisor_timeout;
} GapConnectionParams;

typedef struct {
    uint16_t conn_int_min;
    uint16_t conn_int_max;
    uint16_t slave_latency;
    uint16_t supervisor_timeout;
} GapConnectionParamsRequest;

typedef struct {
    struct {
        uint8_t UUID_Type;
        uint16_t Service_UUID_16;
        uint8_t Service_UUID_128[16];
    } adv_service;
    uint8_t mfg_data[23];
    uint8_t mfg_data_len;
    uint16_t appearance_char;
    bool bonding_mode;
    GapPairing pairing_method;
    uint8_t mac_address[GAP_MAC_ADDR_SIZE];
    char adv_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    GapConnectionParamsRequest conn_param;
} GapConfig;

typedef struct {
    // Encryption Root key. Must be unique per-device (or app)
    uint8_t erk[GAP_KEY_SIZE];
    // Identity Root key. Used for resolving RPAs, if configured
    uint8_t irk[GAP_KEY_SIZE];
} GapRootSecurityKeys;

bool gap_init(
    GapConfig* config,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback on_event_cb,
    void* context);

void gap_start_advertising(void);

void gap_stop_advertising(void);

GapState gap_get_state(void);

void gap_thread_stop(void);

void gap_emit_ble_beacon_status_event(bool active);

/*
 * Scanning (observer role) — requires BLE Full stack
 */

typedef struct {
    uint16_t interval;
    uint16_t window;
    bool active;
    uint16_t timeout_ms;
} GapScanParams;

typedef struct {
    uint8_t address[GAP_MAC_ADDR_SIZE];
    uint8_t address_type;
    int8_t rssi;
    const uint8_t* data;
    uint8_t data_len;
} GapScanResultData;

typedef void (*GapScanCallback)(GapScanResultData* result, void* context);

bool gap_start_scanning(const GapScanParams* params);
void gap_stop_scanning(void);
void gap_set_scan_callback(GapScanCallback callback, void* context);

/*
 * Central role connections — requires BLE Full stack
 */

bool gap_connect(uint8_t address_type, const uint8_t* address);
bool gap_disconnect(uint16_t connection_handle);
uint16_t gap_get_connection_handle(void);

/** Set a fixed PIN for the next pairing attempt.
 *  When set, the GAP layer will respond with this PIN instead of a random one
 *  when the remote device requests passkey authentication.
 *  Set to 0 to revert to random PIN generation.
 *
 *  @param pin  6-digit PIN code (e.g. 123456), or 0 to disable
 */
void gap_set_fixed_pin(uint32_t pin);

/** Configure pairing for central role with a fixed PIN.
 *  Sets IO capability to KEYBOARD_DISPLAY and configures the stack to use
 *  the provided PIN automatically. Call with pin=0 to restore defaults.
 *
 *  @param fixed_pin  6-digit PIN (e.g. 123456), or 0 to restore defaults
 */
void gap_set_pairing_method(uint32_t fixed_pin);

/** Initiate pairing/bonding with the connected peripheral (central role).
 *  Must be called after gap_connect() succeeds and before GATT discovery
 *  if the remote device requires authentication.
 *
 *  @param connection_handle  handle from gap_get_connection_handle()
 *  @param force_rebond       true to force re-pairing even if already bonded
 *  @return true if pairing request was sent
 */
bool gap_pair(uint16_t connection_handle, bool force_rebond);

#ifdef __cplusplus
}
#endif
