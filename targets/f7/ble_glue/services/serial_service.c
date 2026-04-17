#include "serial_service.h"
#include "app_common.h"
#include <ble/ble.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <gap.h>

#include <furi.h>

#include "serial_service_uuid.inc"
#include <stdint.h>

#define TAG "BtSerialSvc"

typedef enum {
    SerialSvcGattCharacteristicRx = 0,
    SerialSvcGattCharacteristicTx,
    SerialSvcGattCharacteristicFlowCtrl,
    SerialSvcGattCharacteristicStatus,
    SerialSvcGattCharacteristicCount,
} SerialSvcGattCharacteristicId;

static const BleGattCharacteristicParams ble_svc_serial_chars[SerialSvcGattCharacteristicCount] = {
    [SerialSvcGattCharacteristicRx] =
        {.name = "RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BLE_SVC_SERIAL_DATA_LEN_MAX,
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_RX_CHAR_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE | CHAR_PROP_READ,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [SerialSvcGattCharacteristicTx] =
        {.name = "TX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BLE_SVC_SERIAL_DATA_LEN_MAX,
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_TX_CHAR_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_INDICATE,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [SerialSvcGattCharacteristicFlowCtrl] =
        {.name = "Flow control",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = sizeof(uint32_t),
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_FLOW_CONTROL_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
    [SerialSvcGattCharacteristicStatus] = {
        .name = "RPC status",
        .data_prop_type = FlipperGattCharacteristicDataFixed,
        .data.fixed.length = sizeof(uint32_t),
        .uuid.Char_UUID_128 = BLE_SVC_SERIAL_RPC_STATUS_UUID,
        .uuid_type = UUID_TYPE_128,
        .char_properties = CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_NOTIFY,
        .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
        .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
        .is_variable = CHAR_VALUE_LEN_CONSTANT}};

struct BleServiceSerial {
    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[SerialSvcGattCharacteristicCount];
    FuriMutex* buff_size_mtx;
    uint32_t buff_size;
    uint16_t bytes_ready_to_receive;
    SerialServiceEventCallback callback;
    void* context;
    GapSvcEventHandler* event_handler;
    BleServiceSerialStats stats;
};

/* Saturating uint32_t increment — avoids wrap-around masking real volume. */
static inline void ble_svc_serial_stat_inc(uint32_t* counter, uint32_t delta) {
    uint32_t current = *counter;
    *counter = (UINT32_MAX - current < delta) ? UINT32_MAX : current + delta;
}

static BleEventAckStatus ble_svc_serial_event_handler(void* event, void* context) {
    BleServiceSerial* serial_svc = (BleServiceSerial*)context;
    BleEventAckStatus ret = BleEventNotAck;
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    aci_gatt_attribute_modified_event_rp0* attribute_modified;
    if(event_pckt->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        if(blecore_evt->ecode == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
            attribute_modified = (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;
            if(attribute_modified->Attr_Handle ==
               serial_svc->chars[SerialSvcGattCharacteristicRx].handle + 2) {
                // Descriptor handle
                ret = BleEventAckFlowEnable;
                FURI_LOG_D(TAG, "RX descriptor event");
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[SerialSvcGattCharacteristicRx].handle + 1) {
                FURI_LOG_D(TAG, "Received %d bytes", attribute_modified->Attr_Data_Length);
                ble_svc_serial_stat_inc(
                    &serial_svc->stats.rx_bytes, attribute_modified->Attr_Data_Length);
                if(serial_svc->callback) {
                    furi_check(
                        furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) ==
                        FuriStatusOk);
                    if(attribute_modified->Attr_Data_Length > serial_svc->bytes_ready_to_receive) {
                        FURI_LOG_W(
                            TAG,
                            "Received %d, while was ready to receive %d bytes. Can lead to buffer overflow!",
                            attribute_modified->Attr_Data_Length,
                            serial_svc->bytes_ready_to_receive);
                        ble_svc_serial_stat_inc(&serial_svc->stats.rx_overruns, 1);
                    }
                    serial_svc->bytes_ready_to_receive -= MIN(
                        serial_svc->bytes_ready_to_receive, attribute_modified->Attr_Data_Length);
                    SerialServiceEvent event = {
                        .event = SerialServiceEventTypeDataReceived,
                        .data = {
                            .buffer = attribute_modified->Attr_Data,
                            .size = attribute_modified->Attr_Data_Length,
                        }};
                    uint32_t buff_free_size = serial_svc->callback(event, serial_svc->context);
                    FURI_LOG_D(TAG, "Available buff size: %ld", buff_free_size);
                    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
                }
                ret = BleEventAckFlowEnable;
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[SerialSvcGattCharacteristicStatus].handle + 1) {
                bool* rpc_status = (bool*)attribute_modified->Attr_Data;
                if(!*rpc_status) {
                    if(serial_svc->callback) {
                        SerialServiceEvent event = {
                            .event = SerialServiceEventTypesBleResetRequest,
                        };
                        serial_svc->callback(event, serial_svc->context);
                    }
                }
            }
        } else if(blecore_evt->ecode == ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE) {
            FURI_LOG_T(TAG, "Ack received");
            ble_svc_serial_stat_inc(&serial_svc->stats.tx_acked, 1);
            if(serial_svc->callback) {
                SerialServiceEvent event = {
                    .event = SerialServiceEventTypeDataSent,
                };
                serial_svc->callback(event, serial_svc->context);
            }
            ret = BleEventAckFlowEnable;
        }
    }
    return ret;
}

typedef enum {
    SerialServiceRpcStatusNotActive = 0UL,
    SerialServiceRpcStatusActive = 1UL,
} SerialServiceRpcStatus;

static void
    ble_svc_serial_update_rpc_char(BleServiceSerial* serial_svc, SerialServiceRpcStatus status) {
    ble_gatt_characteristic_update(
        serial_svc->svc_handle, &serial_svc->chars[SerialSvcGattCharacteristicStatus], &status);
}

BleServiceSerial* ble_svc_serial_start(void) {
    BleServiceSerial* serial_svc = malloc(sizeof(BleServiceSerial));
    memset(&serial_svc->stats, 0, sizeof(serial_svc->stats));

    serial_svc->event_handler =
        ble_event_dispatcher_register_svc_handler(ble_svc_serial_event_handler, serial_svc);

    if(!ble_gatt_service_add(
           UUID_TYPE_128, &service_uuid, PRIMARY_SERVICE, 12, &serial_svc->svc_handle)) {
        free(serial_svc);
        return NULL;
    }
    for(uint8_t i = 0; i < SerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_init(
            serial_svc->svc_handle, &ble_svc_serial_chars[i], &serial_svc->chars[i]);
    }

    ble_svc_serial_update_rpc_char(serial_svc, SerialServiceRpcStatusNotActive);
    serial_svc->buff_size_mtx = furi_mutex_alloc(FuriMutexTypeNormal);

    return serial_svc;
}

void ble_svc_serial_set_callbacks(
    BleServiceSerial* serial_svc,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    furi_check(serial_svc);
    serial_svc->callback = callback;
    serial_svc->context = context;
    serial_svc->buff_size = buff_size;
    serial_svc->bytes_ready_to_receive = buff_size;

    uint32_t buff_size_reversed = REVERSE_BYTES_U32(serial_svc->buff_size);
    ble_gatt_characteristic_update(
        serial_svc->svc_handle,
        &serial_svc->chars[SerialSvcGattCharacteristicFlowCtrl],
        &buff_size_reversed);
}

void ble_svc_serial_notify_buffer_is_empty(BleServiceSerial* serial_svc) {
    furi_check(serial_svc);
    furi_check(serial_svc->buff_size_mtx);

    furi_check(furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) == FuriStatusOk);
    if(serial_svc->bytes_ready_to_receive == 0) {
        FURI_LOG_D(TAG, "Buffer is empty. Notifying client");
        serial_svc->bytes_ready_to_receive = serial_svc->buff_size;
        ble_svc_serial_stat_inc(&serial_svc->stats.credits_resets, 1);

        uint32_t buff_size_reversed = REVERSE_BYTES_U32(serial_svc->buff_size);
        ble_gatt_characteristic_update(
            serial_svc->svc_handle,
            &serial_svc->chars[SerialSvcGattCharacteristicFlowCtrl],
            &buff_size_reversed);
    }
    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
}

void ble_svc_serial_stop(BleServiceSerial* serial_svc) {
    furi_check(serial_svc);

    ble_event_dispatcher_unregister_svc_handler(serial_svc->event_handler);

    for(uint8_t i = 0; i < SerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_delete(serial_svc->svc_handle, &serial_svc->chars[i]);
    }
    ble_gatt_service_delete(serial_svc->svc_handle);
    furi_mutex_free(serial_svc->buff_size_mtx);
    free(serial_svc);
}

bool ble_svc_serial_update_tx(BleServiceSerial* serial_svc, uint8_t* data, uint16_t data_len) {
    if(data_len > BLE_SVC_SERIAL_DATA_LEN_MAX) {
        return false;
    }

    /* Target peripheral connection specifically — in dual-connection mode,
     * conn_handle=0 would notify ALL connections including central-role ones
     * that shouldn't receive serial service notifications. */
    uint16_t conn_handle = gap_get_connection_handle_by_role(false);
    if(conn_handle == 0) {
        FURI_LOG_W(TAG, "No peripheral connection for TX notification");
        return false;
    }

    for(uint16_t remained = data_len; remained > 0;) {
        uint8_t value_len = MIN(BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX, remained);
        uint16_t value_offset = data_len - remained;
        bool is_last_fragment = (remained == value_len);

        /* Retry when the stack's TX pool is momentarily full. A stalled TX
         * here used to silently fail and deadlock the RPC thread waiting
         * for the indication ACK that would never come. Cap at ~200 ms so
         * a persistently wedged stack surfaces as an error instead of
         * blocking the RPC thread indefinitely. */
        tBleStatus result = BLE_STATUS_INSUFFICIENT_RESOURCES;
        size_t retries_left = 200;
        while(retries_left-- > 0 && result == BLE_STATUS_INSUFFICIENT_RESOURCES) {
            result = aci_gatt_update_char_value_ext(
                conn_handle,
                serial_svc->svc_handle,
                serial_svc->chars[SerialSvcGattCharacteristicTx].handle,
                is_last_fragment ? 0x02 : 0x00, /* 0x02 = indication (ACK-gated backpressure) */
                data_len,
                value_offset,
                value_len,
                data + value_offset);
            if(result == BLE_STATUS_INSUFFICIENT_RESOURCES) {
                ble_svc_serial_stat_inc(&serial_svc->stats.tx_retries, 1);
                furi_delay_ms(1);
            }
        }

        if(result != BLE_STATUS_SUCCESS) {
            FURI_LOG_E(TAG, "Failed updating TX characteristic: %d", result);
            ble_svc_serial_stat_inc(&serial_svc->stats.tx_errors, 1);
            /* No indication was queued, so no ACK will arrive — unblock the
             * RPC thread so it can abort the message instead of deadlocking
             * on BT_RPC_EVENT_BUFF_SENT. */
            if(serial_svc->callback) {
                SerialServiceEvent event = {
                    .event = SerialServiceEventTypeDataSent,
                };
                serial_svc->callback(event, serial_svc->context);
            }
            return false;
        }

        ble_svc_serial_stat_inc(&serial_svc->stats.tx_submitted, 1);
        remained -= value_len;
    }

    return true;
}

void ble_svc_serial_set_rpc_active(BleServiceSerial* serial_svc, bool active) {
    furi_check(serial_svc);
    ble_svc_serial_update_rpc_char(
        serial_svc, active ? SerialServiceRpcStatusActive : SerialServiceRpcStatusNotActive);
}

void ble_svc_serial_get_stats(BleServiceSerial* serial_svc, BleServiceSerialStats* stats) {
    furi_check(serial_svc);
    furi_check(stats);
    /* Plain copy — each counter has a single writer thread and aligned
     * 32-bit reads are atomic on Cortex-M, so no mutex needed for a
     * best-effort diagnostic snapshot. */
    *stats = serial_svc->stats;
}

void ble_svc_serial_reset_stats(BleServiceSerial* serial_svc) {
    furi_check(serial_svc);
    memset(&serial_svc->stats, 0, sizeof(serial_svc->stats));
}
