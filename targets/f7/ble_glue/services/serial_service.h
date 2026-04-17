#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Serial service. Implements RPC over BLE, with flow control.
 */

#define BLE_SVC_SERIAL_DATA_LEN_MAX       (486)
#define BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX (243)

typedef enum {
    SerialServiceEventTypeDataReceived,
    SerialServiceEventTypeDataSent,
    SerialServiceEventTypesBleResetRequest,
} SerialServiceEventType;

typedef struct {
    uint8_t* buffer;
    uint16_t size;
} SerialServiceData;

typedef struct {
    SerialServiceEventType event;
    SerialServiceData data;
} SerialServiceEvent;

typedef uint16_t (*SerialServiceEventCallback)(SerialServiceEvent event, void* context);

typedef struct BleServiceSerial BleServiceSerial;

/** Runtime diagnostics for the serial service.
 *
 * All counters are monotonic, saturate at UINT32_MAX, and are cleared by
 * ble_svc_serial_reset_stats(). Each counter has a single writer thread,
 * so increments are race-free; a 32-bit read from another thread may be
 * stale but not torn on Cortex-M. */
typedef struct {
    uint32_t tx_submitted;  /**< fragments successfully queued to the BLE stack */
    uint32_t tx_acked;      /**< indication ACKs received (peer confirmed) */
    uint32_t tx_retries;    /**< total INSUFFICIENT_RESOURCES retry attempts */
    uint32_t tx_errors;     /**< TX submissions that failed after retries */
    uint32_t rx_bytes;      /**< total bytes received on the RX characteristic */
    uint32_t rx_overruns;   /**< peer writes that exceeded available credits */
    uint32_t credits_resets;/**< times bytes_ready_to_receive was replenished */
} BleServiceSerialStats;

BleServiceSerial* ble_svc_serial_start(void);

void ble_svc_serial_stop(BleServiceSerial* service);

void ble_svc_serial_set_callbacks(
    BleServiceSerial* service,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context);

void ble_svc_serial_set_rpc_active(BleServiceSerial* service, bool active);

void ble_svc_serial_notify_buffer_is_empty(BleServiceSerial* service);

bool ble_svc_serial_update_tx(BleServiceSerial* service, uint8_t* data, uint16_t data_len);

void ble_svc_serial_get_stats(BleServiceSerial* service, BleServiceSerialStats* stats);

void ble_svc_serial_reset_stats(BleServiceSerial* service);

#ifdef __cplusplus
}
#endif
