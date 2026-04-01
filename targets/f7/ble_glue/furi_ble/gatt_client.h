/**
 * @file gatt_client.h
 * BLE GATT Client - discover services, read/write characteristics on remote devices
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GATT_CLIENT_MAX_SERVICES 16
#define BLE_GATT_CLIENT_MAX_CHARS    32

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t uuid_type; // 1=16bit, 2=128bit
    union {
        uint16_t uuid_16;
        uint8_t uuid_128[16];
    };
} BleGattService;

typedef struct {
    uint16_t decl_handle;
    uint16_t value_handle;
    uint8_t properties;
    uint8_t uuid_type;
    union {
        uint16_t uuid_16;
        uint8_t uuid_128[16];
    };
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
    uint16_t connection_handle;
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
            uint16_t attr_handle;
            const uint8_t* data;
            uint16_t data_len;
        } read;
        struct {
            uint16_t attr_handle;
        } write;
        struct {
            uint16_t attr_handle;
            const uint8_t* data;
            uint16_t data_len;
        } notification;
        uint8_t error_code;
    };
} BleGattClientEvent;

typedef void (*BleGattClientCallback)(BleGattClientEvent* event, void* context);

/** Initialize the GATT client module */
void ble_gatt_client_init(void);

/** Set the callback for GATT client events
 *
 * @param callback  event callback
 * @param context   user context
 */
void ble_gatt_client_set_callback(BleGattClientCallback callback, void* context);

/** Discover all primary services on a connected device
 *
 * @param connection_handle  BLE connection handle
 * @return                   true if discovery started
 */
bool ble_gatt_client_discover_services(uint16_t connection_handle);

/** Discover all characteristics of a service
 *
 * @param connection_handle  BLE connection handle
 * @param service            service to enumerate
 * @return                   true if discovery started
 */
bool ble_gatt_client_discover_characteristics(
    uint16_t connection_handle,
    const BleGattService* service);

/** Read a characteristic value
 *
 * @param connection_handle  BLE connection handle
 * @param attr_handle        attribute handle to read
 * @return                   true if read started
 */
bool ble_gatt_client_read(uint16_t connection_handle, uint16_t attr_handle);

/** Write a characteristic value
 *
 * @param connection_handle  BLE connection handle
 * @param attr_handle        attribute handle to write
 * @param data               data to write
 * @param data_len           data length
 * @return                   true if write started
 */
bool ble_gatt_client_write(
    uint16_t connection_handle,
    uint16_t attr_handle,
    const uint8_t* data,
    uint16_t data_len);

/** Enable notifications on a characteristic (write CCCD)
 *
 * @param connection_handle  BLE connection handle
 * @param attr_handle        characteristic value handle
 * @param enable             true to enable, false to disable
 * @return                   true if write started
 */
bool ble_gatt_client_subscribe_notifications(
    uint16_t connection_handle,
    uint16_t attr_handle,
    bool enable);

#ifdef __cplusplus
}
#endif
