#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GATT_CLIENT_MAX_SERVICES 16
#define BLE_GATT_CLIENT_MAX_CHARS    64

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
