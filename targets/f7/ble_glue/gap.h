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
    GapEventTypeScanResult,
    GapEventTypeScanComplete,
} GapEventType;

typedef struct {
    uint8_t address_type;
    uint8_t address[GAP_MAC_ADDR_SIZE];
    int8_t rssi;
    uint8_t data_len;
    const uint8_t* data;
} GapScanResultData;

typedef union {
    uint32_t pin_code;
    uint16_t max_packet_size;
    GapScanResultData scan_result;
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
} GapState;

typedef struct {
    uint16_t interval;   // Scan interval (N * 0.625 ms)
    uint16_t window;     // Scan window (N * 0.625 ms)
    bool active;         // Active scan (sends SCAN_REQ)
    uint32_t timeout_ms; // 0 = scan until stopped
} GapScanParams;

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

/** Get the current connection handle (valid when state == GapStateConnected)
 *
 * @return connection handle, or 0xFFFF if not connected
 */
uint16_t gap_get_connection_handle(void);

void gap_thread_stop(void);

void gap_emit_ble_beacon_status_event(bool active);

/** Start BLE scanning (Central mode)
 *
 * @param params  scan parameters
 * @return        true on success
 */
typedef void (*GapScanCallback)(GapScanResultData* result, void* context);

/** Set callback for scan results (independent of main GAP event callback)
 *
 * @param callback  scan result callback, or NULL to clear
 * @param context   user context
 */
void gap_set_scan_callback(GapScanCallback callback, void* context);

bool gap_start_scanning(const GapScanParams* params);

/** Stop BLE scanning
 */
void gap_stop_scanning(void);

/** Connect to a BLE device (Central mode)
 *
 * @param address_type  peer address type (0=public, 1=random)
 * @param address       6-byte peer address
 * @return              true on success
 */
bool gap_connect(uint8_t address_type, const uint8_t* address);

/** Disconnect from a specific connection
 *
 * @param connection_handle  the connection handle
 * @return                   true on success
 */
bool gap_disconnect(uint16_t connection_handle);

#ifdef __cplusplus
}
#endif
