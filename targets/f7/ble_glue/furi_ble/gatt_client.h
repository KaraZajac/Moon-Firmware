#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GATT_CLIENT_MAX_SERVICES 16
#define BLE_GATT_CLIENT_MAX_CHARS    64

/* Full documented GATT event mask for aci_gatt_set_event_mask().
 * Applies globally (both central/client and peripheral/server roles);
 * any narrower mask risks starving the peripheral event path, breaking
 * things like indication-based flow control and Android GATT discovery
 * when the companion app uses indications. */
#define BLE_GATT_FULL_EVENT_MASK                                                  \
    (0x00000001u | /* ACI_GATT_ATTRIBUTE_MODIFIED_EVENT                     */    \
     0x00000002u | /* ACI_GATT_PROC_TIMEOUT_EVENT                           */    \
     0x00000004u | /* ACI_ATT_EXCHANGE_MTU_RESP_EVENT                       */    \
     0x00000008u | /* ACI_ATT_FIND_INFO_RESP_EVENT                          */    \
     0x00000010u | /* ACI_ATT_FIND_BY_TYPE_VALUE_RESP_EVENT                 */    \
     0x00000020u | /* ACI_ATT_READ_BY_TYPE_RESP_EVENT                       */    \
     0x00000040u | /* ACI_ATT_READ_RESP_EVENT                               */    \
     0x00000080u | /* ACI_ATT_READ_BLOB_RESP_EVENT                          */    \
     0x00000100u | /* ACI_ATT_READ_MULTIPLE_RESP_EVENT                      */    \
     0x00000200u | /* ACI_ATT_READ_BY_GROUP_TYPE_RESP_EVENT                 */    \
     0x00000800u | /* ACI_ATT_PREPARE_WRITE_RESP_EVENT                      */    \
     0x00001000u | /* ACI_ATT_EXEC_WRITE_RESP_EVENT                         */    \
     0x00002000u | /* ACI_GATT_INDICATION_EVENT                             */    \
     0x00004000u | /* ACI_GATT_NOTIFICATION_EVENT                           */    \
     0x00008000u | /* ACI_GATT_ERROR_RESP_EVENT                             */    \
     0x00010000u | /* ACI_GATT_PROC_COMPLETE_EVENT                          */    \
     0x00020000u | /* ACI_GATT_DISC_READ_CHAR_BY_UUID_RESP_EVENT            */    \
     0x00040000u | /* ACI_GATT_TX_POOL_AVAILABLE_EVENT                      */    \
     0x00100000u | /* ACI_GATT_READ_EXT_EVENT                               */    \
     0x00200000u | /* ACI_GATT_INDICATION_EXT_EVENT                         */    \
     0x00400000u) /* ACI_GATT_NOTIFICATION_EXT_EVENT                        */

typedef struct {
    uint8_t uuid_type; // 1 = 16-bit, 2 = 128-bit
    uint16_t uuid_16;
    uint8_t uuid_128[16];
    uint16_t start_handle;
    uint16_t end_handle;
} BleGattService;

typedef struct {
    uint8_t uuid_type; // 1 = 16-bit, 2 = 128-bit
    uint16_t uuid_16;
    uint8_t uuid_128[16];
    uint16_t decl_handle;
    uint16_t value_handle;
    uint8_t properties;
} BleGattCharacteristic;

typedef enum {
    BleGattClientEventDiscoverComplete,
    BleGattClientEventCharDiscoverComplete,
    BleGattClientEventReadComplete,
    BleGattClientEventWriteComplete,
    BleGattClientEventNotification,
    BleGattClientEventError,
} BleGattClientEventType;

typedef struct {
    BleGattClientEventType type;
    uint16_t connection_handle; /**< Connection this event belongs to */
    union {
        struct {
            BleGattService* services;
            uint8_t count;
        } discover;
        struct {
            BleGattCharacteristic* chars;
            uint8_t count;
        } char_discover;
        struct {
            const uint8_t* data;
            uint16_t data_len;
            uint16_t value_handle;
        } read;
        struct {
            const uint8_t* data;
            uint16_t data_len;
            uint16_t value_handle;
            uint16_t offset; // For extended notifications: bit 15 = first fragment
        } notification;
        struct {
            uint8_t error_code;
        } error;
    };
} BleGattClientEvent;

typedef void (*BleGattClientCallback)(BleGattClientEvent* event, void* context);

/** Initialize GATT client */
void ble_gatt_client_init(void);

/** Deinitialize GATT client (unregister event handler) */
void ble_gatt_client_deinit(void);

/** Set GATT client event callback for a specific connection.
 *  Each connection can have its own callback and context.
 *  Call with callback=NULL to unregister a connection.
 *
 *  @param connection_handle  BLE connection handle
 *  @param callback           event callback, or NULL to unregister
 *  @param context            user context passed to callback
 */
void ble_gatt_client_set_callback(
    uint16_t connection_handle,
    BleGattClientCallback callback,
    void* context);

/** Discover all primary services on a connected device */
bool ble_gatt_client_discover_services(uint16_t connection_handle);

/** Discover all characteristics within a service */
bool ble_gatt_client_discover_characteristics(
    uint16_t connection_handle,
    const BleGattService* service);

/** Read a characteristic value */
bool ble_gatt_client_read(uint16_t connection_handle, uint16_t value_handle);

/** Write a characteristic value */
bool ble_gatt_client_write(
    uint16_t connection_handle,
    uint16_t value_handle,
    const uint8_t* data,
    uint16_t data_len);

/** Request MTU exchange (must be called after connection, before reads).
 *  The server will respond with its max MTU; the actual MTU is the minimum of both.
 *  Result is delivered asynchronously via the GapEventTypeUpdateMTU event. */
bool ble_gatt_client_exchange_mtu(uint16_t connection_handle);

/** Enable/disable notifications for a characteristic */
bool ble_gatt_client_subscribe_notifications(
    uint16_t connection_handle,
    uint16_t value_handle,
    bool enable);

#ifdef __cplusplus
}
#endif
