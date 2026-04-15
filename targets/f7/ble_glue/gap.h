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
    uint16_t connection_handle; /**< BLE connection handle (0 if not applicable) */
    bool is_central;            /**< true if this event is for a central-role connection */
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

/** Activity flags — track concurrent BLE operations independently.
 *  Unlike GapState (single enum), these can be combined. */
typedef enum {
    GapActivityAdvertising = (1 << 0),
    GapActivityScanning    = (1 << 1),
    GapActivityConnecting  = (1 << 2),
    GapActivityConnected   = (1 << 3),
} GapActivity;

/** Get active BLE activities (bitmask of GapActivity flags). */
uint8_t gap_get_activities(void);

/** Check if a specific activity is active. */
bool gap_is_activity_active(GapActivity activity);

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

/** Get connection handle for a specific role.
 *  @param central  true = get central connection, false = peripheral
 *  @return connection handle, or 0 if none with that role
 */
uint16_t gap_get_connection_handle_by_role(bool central);

/** Get current number of active connections */
uint8_t gap_get_connection_count(void);

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

/*
 * PHY preference — request 1M or 2M PHY per connection
 * Note: STM32WB55 does NOT support LE Coded PHY (Long Range)
 */

typedef enum {
    GapPhy1M = 0x01,
    GapPhy2M = 0x02,
} GapPhy;

/** Request PHY update for a connection.
 *  @param conn_handle  connection handle
 *  @param tx_phy       preferred TX PHY (GapPhy bitmask, 0 = no preference)
 *  @param rx_phy       preferred RX PHY (GapPhy bitmask, 0 = no preference)
 *  @return true if request sent
 */
bool gap_set_phy_preference(uint16_t conn_handle, uint8_t tx_phy, uint8_t rx_phy);

/** Read current PHY for a connection.
 *  @param conn_handle  connection handle
 *  @param[out] tx_phy  current TX PHY
 *  @param[out] rx_phy  current RX PHY
 *  @return true if read succeeded
 */
bool gap_get_phy(uint16_t conn_handle, uint8_t* tx_phy, uint8_t* rx_phy);

/*
 * Extended Advertising — up to 254 bytes of adv data, multiple simultaneous sets
 * Requires BLE Full stack with EXT_ADV option enabled
 */

#define GAP_EXT_ADV_MAX_DATA_LEN 254
#define GAP_EXT_ADV_MAX_SETS     4

/* Advertising event properties (bitmask) */
#define GAP_EXT_ADV_PROP_CONNECTABLE   0x0001
#define GAP_EXT_ADV_PROP_SCANNABLE     0x0002
#define GAP_EXT_ADV_PROP_LEGACY        0x0010
#define GAP_EXT_ADV_PROP_INCLUDE_TX_PW 0x0040

/** Configure an extended advertising set.
 *  @param adv_handle         0x00-0xEF, identifies the set
 *  @param adv_event_props    bitmask of GAP_EXT_ADV_PROP_*
 *  @param interval_min       min interval in 0.625ms units (0x20 = 20ms minimum)
 *  @param interval_max       max interval in 0.625ms units
 *  @param secondary_phy      0x01=1M, 0x02=2M
 *  @param adv_sid            0x00-0x0F
 *  @return true if configured
 */
bool gap_ext_adv_configure(
    uint8_t adv_handle,
    uint16_t adv_event_props,
    uint32_t interval_min,
    uint32_t interval_max,
    uint8_t secondary_phy,
    uint8_t adv_sid);

/** Set extended advertising data.
 *  @param adv_handle   set handle
 *  @param data         AD structure data
 *  @param data_len     length (up to GAP_EXT_ADV_MAX_DATA_LEN)
 *  @return true if data set
 */
bool gap_ext_adv_set_data(uint8_t adv_handle, const uint8_t* data, uint8_t data_len);

/** Set extended scan response data. */
bool gap_ext_adv_set_scan_resp(uint8_t adv_handle, const uint8_t* data, uint8_t data_len);

/** Enable an extended advertising set.
 *  @param adv_handle   set handle
 *  @param duration_ms  duration in ms (0 = indefinite)
 *  @return true if enabled
 */
bool gap_ext_adv_start(uint8_t adv_handle, uint16_t duration_ms);

/** Stop an extended advertising set. */
bool gap_ext_adv_stop(uint8_t adv_handle);

/** Remove an extended advertising set. */
bool gap_ext_adv_remove(uint8_t adv_handle);

#ifdef __cplusplus
}
#endif
