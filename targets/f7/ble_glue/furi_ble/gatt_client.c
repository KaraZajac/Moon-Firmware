#include "gatt_client.h"
#include "event_dispatcher.h"

#include "../app_common.h"
#include <ble/ble.h>

#include <furi.h>
#include <string.h>

#define TAG "BleGattClient"

typedef struct {
    BleGattClientCallback callback;
    void* context;
    BleGattService services[BLE_GATT_CLIENT_MAX_SERVICES];
    uint8_t service_count;
    BleGattCharacteristic chars[BLE_GATT_CLIENT_MAX_CHARS];
    uint8_t char_count;
    GapSvcEventHandler* event_handler;
} BleGattClient;

static BleGattClient* gatt_client = NULL;

static BleEventAckStatus gatt_client_event_handler(void* raw_event, void* context) {
    UNUSED(context);
    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)raw_event)->data;

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        return BleEventNotAck;
    }
    if(!gatt_client || !gatt_client->callback) {
        return BleEventNotAck;
    }

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;

    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        // Service discovery response
        aci_att_read_by_group_type_resp_event_rp0* resp =
            (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
        uint8_t attr_len = resp->Attribute_Data_Length;
        uint8_t num_entries = resp->Data_Length / attr_len;
        uint8_t* data = resp->Attribute_Data_List;

        for(uint8_t i = 0; i < num_entries && gatt_client->service_count < BLE_GATT_CLIENT_MAX_SERVICES; i++) {
            BleGattService* svc = &gatt_client->services[gatt_client->service_count];
            svc->start_handle = data[0] | (data[1] << 8);
            svc->end_handle = data[2] | (data[3] << 8);
            if(attr_len == 6) {
                svc->uuid_type = 1;
                svc->uuid_16 = data[4] | (data[5] << 8);
            } else if(attr_len == 20) {
                svc->uuid_type = 2;
                memcpy(svc->uuid_128, &data[4], 16);
            }
            gatt_client->service_count++;
            data += attr_len;
        }
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        // Characteristic discovery response
        aci_att_read_by_type_resp_event_rp0* resp =
            (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
        uint8_t pair_len = resp->Handle_Value_Pair_Length;
        uint8_t num_entries = resp->Data_Length / pair_len;
        uint8_t* data = resp->Handle_Value_Pair_Data;

        for(uint8_t i = 0; i < num_entries && gatt_client->char_count < BLE_GATT_CLIENT_MAX_CHARS; i++) {
            BleGattCharacteristic* chr = &gatt_client->chars[gatt_client->char_count];
            chr->decl_handle = data[0] | (data[1] << 8);
            chr->properties = data[2];
            chr->value_handle = data[3] | (data[4] << 8);
            if(pair_len == 7) {
                chr->uuid_type = 1;
                chr->uuid_16 = data[5] | (data[6] << 8);
            } else if(pair_len == 21) {
                chr->uuid_type = 2;
                memcpy(chr->uuid_128, &data[5], 16);
            }
            gatt_client->char_count++;
            data += pair_len;
        }
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* proc_complete =
            (aci_gatt_proc_complete_event_rp0*)blue_evt->data;

        if(proc_complete->Error_Code != BLE_STATUS_SUCCESS) {
            // Not necessarily an error — 0x0A (ATTR_NOT_FOUND) means discovery finished
            if(proc_complete->Error_Code != 0x0A) {
                FURI_LOG_W(TAG, "GATT proc error: 0x%02X", proc_complete->Error_Code);
            }
        }

        // Determine what completed based on accumulated state
        if(gatt_client->service_count > 0 && gatt_client->char_count == 0) {
            // Service discovery completed
            BleGattClientEvent event = {
                .type = BleGattClientEventDiscoverComplete,
                .connection_handle = proc_complete->Connection_Handle,
                .discover = {
                    .services = gatt_client->services,
                    .count = gatt_client->service_count,
                }};
            gatt_client->callback(&event, gatt_client->context);
        } else if(gatt_client->char_count > 0) {
            // Characteristic discovery completed
            BleGattClientEvent event = {
                .type = BleGattClientEventCharDiscoverComplete,
                .connection_handle = proc_complete->Connection_Handle,
                .char_discover = {
                    .chars = gatt_client->chars,
                    .count = gatt_client->char_count,
                }};
            gatt_client->callback(&event, gatt_client->context);
        }
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_READ_PERMIT_REQ_VSEVT_CODE:
        // We are a GATT client reading, this shouldn't fire for us
        return BleEventNotAck;

    case ACI_ATT_READ_RESP_VSEVT_CODE: {
        // Read response
        aci_att_read_resp_event_rp0* resp =
            (aci_att_read_resp_event_rp0*)blue_evt->data;
        BleGattClientEvent event = {
            .type = BleGattClientEventReadComplete,
            .connection_handle = resp->Connection_Handle,
            .read = {
                .attr_handle = 0, // Not provided in this event
                .data = resp->Attribute_Value,
                .data_len = resp->Event_Data_Length - 2,
            }};
        gatt_client->callback(&event, gatt_client->context);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_NOTIFICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* notif =
            (aci_gatt_notification_event_rp0*)blue_evt->data;
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .connection_handle = notif->Connection_Handle,
            .notification = {
                .attr_handle = notif->Attribute_Handle,
                .data = notif->Attribute_Value,
                .data_len = notif->Attribute_Value_Length,
            }};
        gatt_client->callback(&event, gatt_client->context);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_INDICATION_VSEVT_CODE: {
        aci_gatt_indication_event_rp0* ind = (void*)blue_evt->data;
        // Confirm the indication
        aci_gatt_confirm_indication(ind->Connection_Handle);
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .connection_handle = ind->Connection_Handle,
            .notification = {
                .attr_handle = ind->Attribute_Handle,
                .data = ind->Attribute_Value,
                .data_len = ind->Attribute_Value_Length,
            }};
        gatt_client->callback(&event, gatt_client->context);
        return BleEventAckFlowEnable;
    }

    default:
        break;
    }

    return BleEventNotAck;
}

void ble_gatt_client_init(void) {
    if(gatt_client) return;
    gatt_client = malloc(sizeof(BleGattClient));
    memset(gatt_client, 0, sizeof(BleGattClient));
    gatt_client->event_handler =
        ble_event_dispatcher_register_svc_handler(gatt_client_event_handler, NULL);
}

void ble_gatt_client_set_callback(BleGattClientCallback callback, void* context) {
    furi_check(gatt_client);
    gatt_client->callback = callback;
    gatt_client->context = context;
}

bool ble_gatt_client_discover_services(uint16_t connection_handle) {
    furi_check(gatt_client);
    gatt_client->service_count = 0;
    gatt_client->char_count = 0;
    tBleStatus status = aci_gatt_disc_all_primary_services(connection_handle);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Discover services failed: %d", status);
        return false;
    }
    FURI_LOG_I(TAG, "Service discovery started");
    return true;
}

bool ble_gatt_client_discover_characteristics(
    uint16_t connection_handle,
    const BleGattService* service) {
    furi_check(gatt_client);
    furi_check(service);
    gatt_client->char_count = 0;
    tBleStatus status = aci_gatt_disc_all_char_of_service(
        connection_handle, service->start_handle, service->end_handle);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Discover characteristics failed: %d", status);
        return false;
    }
    FURI_LOG_I(TAG, "Characteristic discovery started");
    return true;
}

bool ble_gatt_client_read(uint16_t connection_handle, uint16_t attr_handle) {
    furi_check(gatt_client);
    tBleStatus status = aci_gatt_read_char_value(connection_handle, attr_handle);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Read characteristic failed: %d", status);
        return false;
    }
    return true;
}

bool ble_gatt_client_write(
    uint16_t connection_handle,
    uint16_t attr_handle,
    const uint8_t* data,
    uint16_t data_len) {
    furi_check(gatt_client);
    furi_check(data);
    tBleStatus status =
        aci_gatt_write_char_value(connection_handle, attr_handle, data_len, (uint8_t*)data);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Write characteristic failed: %d", status);
        return false;
    }
    return true;
}

bool ble_gatt_client_subscribe_notifications(
    uint16_t connection_handle,
    uint16_t attr_handle,
    bool enable) {
    furi_check(gatt_client);
    // CCCD is typically at value_handle + 1
    uint16_t cccd_handle = attr_handle + 1;
    uint8_t cccd_val[2] = {enable ? 0x01 : 0x00, 0x00};
    tBleStatus status =
        aci_gatt_write_char_value(connection_handle, cccd_handle, 2, cccd_val);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Subscribe notifications failed: %d", status);
        return false;
    }
    FURI_LOG_I(TAG, "Notifications %s for handle 0x%04X", enable ? "enabled" : "disabled", attr_handle);
    return true;
}
