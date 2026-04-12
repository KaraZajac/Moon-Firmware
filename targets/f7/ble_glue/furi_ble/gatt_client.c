#include "gatt_client.h"
#include "event_dispatcher.h"

#include "../app_common.h"
#include <ble/ble.h>

#include <furi.h>
#include <string.h>

#define TAG "BleGattClient"

static BleGattClientCallback gatt_client_callback = NULL;
static void* gatt_client_context = NULL;

static BleGattService discovered_services[BLE_GATT_CLIENT_MAX_SERVICES];
static uint8_t discovered_services_count = 0;

static BleGattCharacteristic discovered_chars[BLE_GATT_CLIENT_MAX_CHARS];
static uint8_t discovered_chars_count = 0;

typedef enum {
    GattOpNone,
    GattOpDiscoverServices,
    GattOpDiscoverChars,
    GattOpRead,
    GattOpWrite,
    GattOpSubscribe,
} GattPendingOp;

static GattPendingOp pending_op = GattOpNone;

static GapSvcEventHandler* gatt_client_handler = NULL;
/* event_dispatcher requires non-NULL context; use this as sentinel */
static uint8_t gatt_client_sentinel = 0;

static BleEventAckStatus gatt_client_event_handler(void* pckt, void* context) {
    UNUSED(context);
    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        return BleEventNotAck;
    }
    if(!gatt_client_callback) {
        return BleEventNotAck;
    }

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;

    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        /* Service discovery response */
        aci_att_read_by_group_type_resp_event_rp0* resp =
            (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
        uint8_t attr_len = resp->Attribute_Data_Length;
        uint8_t num_attr = resp->Data_Length / attr_len;

        for(uint8_t i = 0; i < num_attr && discovered_services_count < BLE_GATT_CLIENT_MAX_SERVICES;
            i++) {
            uint8_t* data = &resp->Attribute_Data_List[i * attr_len];
            BleGattService* svc = &discovered_services[discovered_services_count];
            svc->start_handle = (data[1] << 8) | data[0];
            svc->end_handle = (data[3] << 8) | data[2];
            if(attr_len == 6) {
                /* 16-bit UUID */
                svc->uuid_type = 1;
                svc->uuid_16 = (data[5] << 8) | data[4];
            } else if(attr_len == 20) {
                /* 128-bit UUID */
                svc->uuid_type = 2;
                memcpy(svc->uuid_128, &data[4], 16);
            }
            discovered_services_count++;
        }
    } break;

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        /* Characteristic discovery response */
        aci_att_read_by_type_resp_event_rp0* resp =
            (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
        uint8_t pair_len = resp->Handle_Value_Pair_Length;
        uint8_t num_pairs = resp->Data_Length / pair_len;

        for(uint8_t i = 0; i < num_pairs && discovered_chars_count < BLE_GATT_CLIENT_MAX_CHARS;
            i++) {
            uint8_t* data = &resp->Handle_Value_Pair_Data[i * pair_len];
            BleGattCharacteristic* chr = &discovered_chars[discovered_chars_count];
            chr->decl_handle = (data[1] << 8) | data[0];
            chr->properties = data[2];
            chr->value_handle = (data[4] << 8) | data[3];
            if(pair_len == 7) {
                /* 16-bit UUID */
                chr->uuid_type = 1;
                chr->uuid_16 = (data[6] << 8) | data[5];
            } else if(pair_len == 21) {
                /* 128-bit UUID */
                chr->uuid_type = 2;
                memcpy(chr->uuid_128, &data[5], 16);
            }
            discovered_chars_count++;
        }
    } break;

    case ACI_ATT_READ_RESP_VSEVT_CODE: {
        aci_att_read_resp_event_rp0* resp = (aci_att_read_resp_event_rp0*)blue_evt->data;
        BleGattClientEvent event = {
            .type = BleGattClientEventReadComplete,
            .read =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Event_Data_Length,
                },
        };
        gatt_client_callback(&event, gatt_client_context);
    } break;

    case ACI_GATT_NOTIFICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* resp =
            (aci_gatt_notification_event_rp0*)blue_evt->data;
        FURI_LOG_D(TAG, "Notif: conn=0x%04X attr=0x%04X len=%d",
            resp->Connection_Handle, resp->Attribute_Handle,
            resp->Attribute_Value_Length);
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .notification =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Attribute_Value_Length,
                    .value_handle = resp->Attribute_Handle,
                },
        };
        gatt_client_callback(&event, gatt_client_context);
    } break;

    case ACI_GATT_NOTIFICATION_EXT_VSEVT_CODE: {
        /* Extended notification — used when MTU > default and data
         * exceeds the standard notification event buffer */
        aci_gatt_notification_ext_event_rp0* resp =
            (aci_gatt_notification_ext_event_rp0*)blue_evt->data;
        FURI_LOG_D(TAG, "Notif EXT: off=0x%04X len=%d",
            resp->Offset, resp->Attribute_Value_Length);
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .notification =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Attribute_Value_Length,
                    .value_handle = resp->Attribute_Handle,
                    .offset = resp->Offset,
                },
        };
        gatt_client_callback(&event, gatt_client_context);
    } break;

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* resp =
            (aci_gatt_proc_complete_event_rp0*)blue_evt->data;
        GattPendingOp completed_op = pending_op;
        pending_op = GattOpNone;

        /* Error 0x0A (Attribute Not Found) is the normal end-of-discovery
         * signal during service/characteristic discovery — not a real error.
         * Error 0x05 (Insufficient Authentication) during discovery means
         * we need encryption but can still deliver partial results. */
        bool is_discovery = (completed_op == GattOpDiscoverServices ||
                            completed_op == GattOpDiscoverChars);
        bool is_benign_error = (resp->Error_Code == 0x0A); /* Attribute Not Found */

        if(resp->Error_Code != 0 && !is_benign_error && !is_discovery) {
            BleGattClientEvent event = {
                .type = BleGattClientEventError,
                .error = {.error_code = resp->Error_Code},
            };
            gatt_client_callback(&event, gatt_client_context);
        } else {
            switch(completed_op) {
            case GattOpDiscoverServices:
                if(discovered_services_count > 0) {
                    BleGattClientEvent event = {
                        .type = BleGattClientEventDiscoverComplete,
                        .discover =
                            {
                                .services = discovered_services,
                                .count = discovered_services_count,
                            },
                    };
                    gatt_client_callback(&event, gatt_client_context);
                    discovered_services_count = 0;
                }
                break;
            case GattOpDiscoverChars:
                if(discovered_chars_count > 0) {
                    BleGattClientEvent event = {
                        .type = BleGattClientEventCharDiscoverComplete,
                        .char_discover =
                            {
                                .chars = discovered_chars,
                                .count = discovered_chars_count,
                            },
                    };
                    gatt_client_callback(&event, gatt_client_context);
                    discovered_chars_count = 0;
                }
                break;
            case GattOpWrite:
            case GattOpSubscribe: {
                BleGattClientEvent event = {.type = BleGattClientEventWriteComplete};
                gatt_client_callback(&event, gatt_client_context);
            } break;
            default:
                break;
            }
        }
    } break;

    case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
        aci_gatt_error_resp_event_rp0* resp = (aci_gatt_error_resp_event_rp0*)blue_evt->data;
        /* 0x0A = Attribute Not Found — normal end-of-discovery, not an error */
        if(resp->Error_Code == 0x0A &&
           (pending_op == GattOpDiscoverServices || pending_op == GattOpDiscoverChars)) {
            FURI_LOG_D(TAG, "Discovery end signal at attr=0x%04X", resp->Attribute_Handle);
        } else {
            FURI_LOG_W(TAG, "GATT error: attr=0x%04X code=0x%02X", resp->Attribute_Handle, resp->Error_Code);
            BleGattClientEvent event = {
                .type = BleGattClientEventError,
                .error = {.error_code = resp->Error_Code},
            };
            gatt_client_callback(&event, gatt_client_context);
        }
    } break;

    default:
        return BleEventNotAck;
    }

    return BleEventNotAck;
}

void ble_gatt_client_init(void) {
    if(!gatt_client_handler) {
        gatt_client_handler = ble_event_dispatcher_register_svc_handler(
            gatt_client_event_handler, &gatt_client_sentinel);

        /* Enable extended GATT events. CRITICAL: without this, notifications
         * larger than 248 bytes arrive with Attribute_Value_Length=0 because
         * the standard notification event buffer is too small. Enabling
         * ACI_GATT_NOTIFICATION_EXT_EVENT lets the stack use the extended
         * event format (0x0C1F) with uint16_t length and larger buffer. */
        aci_gatt_set_event_mask(
            0x00000001 | /* ACI_GATT_ATTRIBUTE_MODIFIED_EVENT */
            0x00000004 | /* ACI_ATT_EXCHANGE_MTU_RESP_EVENT */
            0x00000020 | /* ACI_ATT_READ_BY_TYPE_RESP_EVENT */
            0x00000040 | /* ACI_ATT_READ_RESP_EVENT */
            0x00000200 | /* ACI_ATT_READ_BY_GROUP_TYPE_RESP_EVENT */
            0x00004000 | /* ACI_GATT_NOTIFICATION_EVENT */
            0x00008000 | /* ACI_GATT_ERROR_RESP_EVENT */
            0x00010000 | /* ACI_GATT_PROC_COMPLETE_EVENT */
            0x00400000   /* ACI_GATT_NOTIFICATION_EXT_EVENT */
        );
        FURI_LOG_I(TAG, "GATT client init + extended events enabled");
    }
}

void ble_gatt_client_deinit(void) {
    if(gatt_client_handler) {
        ble_event_dispatcher_unregister_svc_handler(gatt_client_handler);
        gatt_client_handler = NULL;
    }
    gatt_client_callback = NULL;
    gatt_client_context = NULL;
}

void ble_gatt_client_set_callback(BleGattClientCallback callback, void* context) {
    gatt_client_callback = callback;
    gatt_client_context = context;
}

bool ble_gatt_client_discover_services(uint16_t connection_handle) {
    discovered_services_count = 0;
    pending_op = GattOpDiscoverServices;
    tBleStatus status = aci_gatt_disc_all_primary_services(connection_handle);
    if(status != BLE_STATUS_SUCCESS) {
        pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Discover services failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_discover_characteristics(
    uint16_t connection_handle,
    const BleGattService* service) {
    discovered_chars_count = 0;
    pending_op = GattOpDiscoverChars;
    tBleStatus status = aci_gatt_disc_all_char_of_service(
        connection_handle, service->start_handle, service->end_handle);
    if(status != BLE_STATUS_SUCCESS) {
        pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Discover chars failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_read(uint16_t connection_handle, uint16_t value_handle) {
    pending_op = GattOpRead;
    tBleStatus status = aci_gatt_read_char_value(connection_handle, value_handle);
    if(status != BLE_STATUS_SUCCESS) {
        pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Read failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_write(
    uint16_t connection_handle,
    uint16_t value_handle,
    const uint8_t* data,
    uint16_t data_len) {
    pending_op = GattOpWrite;
    tBleStatus status =
        aci_gatt_write_char_value(connection_handle, value_handle, data_len, data);
    if(status != BLE_STATUS_SUCCESS) {
        pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Write failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_exchange_mtu(uint16_t connection_handle) {
    tBleStatus status = aci_gatt_exchange_config(connection_handle);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "MTU exchange failed: 0x%02X", status);
    } else {
        FURI_LOG_I(TAG, "MTU exchange requested");
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_subscribe_notifications(
    uint16_t connection_handle,
    uint16_t value_handle,
    bool enable) {
    /* Write to Client Characteristic Configuration Descriptor (CCCD).
     * CCCD handle is typically value_handle + 1. This assumption holds
     * for most standard BLE services. */
    pending_op = GattOpSubscribe;
    uint16_t cccd_handle = value_handle + 1;
    uint8_t cccd_val[2] = {enable ? 0x01 : 0x00, 0x00};
    tBleStatus status =
        aci_gatt_write_char_desc(connection_handle, cccd_handle, 2, cccd_val);
    if(status != BLE_STATUS_SUCCESS) {
        pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Subscribe notifications failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}
