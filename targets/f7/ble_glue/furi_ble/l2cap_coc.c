#include "l2cap_coc.h"
#include "event_dispatcher.h"

#include "../app_common.h"
#include <ble/ble.h>

#include <furi.h>

#define TAG "BleL2capCoC"

static BleL2capCocCallback coc_callback = NULL;
static void* coc_context = NULL;
static GapSvcEventHandler* coc_handler = NULL;
static uint8_t coc_sentinel = 0;

static BleEventAckStatus l2cap_coc_event_handler(void* event, void* context) {
    UNUSED(context);
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;
    if(!coc_callback) return BleEventNotAck;

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;

    switch(blue_evt->ecode) {
    case ACI_L2CAP_COC_CONNECT_VSEVT_CODE: {
        /* Incoming CoC connection request */
        aci_l2cap_coc_connect_event_rp0* evt =
            (aci_l2cap_coc_connect_event_rp0*)blue_evt->data;
        FURI_LOG_I(TAG, "CoC connect request: SPSM=0x%04X MTU=%d MPS=%d credits=%d",
            evt->SPSM, evt->MTU, evt->MPS, evt->Initial_Credits);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventConnected,
            .channel_index = 0, // Will be set after accept
            .connected = {
                .peer_mtu = evt->MTU,
                .peer_mps = evt->MPS,
                .initial_credits = evt->Initial_Credits,
            },
        };
        coc_callback(&coc_evt, coc_context);
    } break;

    case ACI_L2CAP_COC_CONNECT_CONFIRM_VSEVT_CODE: {
        /* Response to our CoC connect request */
        aci_l2cap_coc_connect_confirm_event_rp0* evt =
            (aci_l2cap_coc_connect_confirm_event_rp0*)blue_evt->data;
        if(evt->Result == 0x0000 && evt->Channel_Number > 0) {
            FURI_LOG_I(TAG, "CoC connected: ch=%d MTU=%d MPS=%d credits=%d",
                evt->Channel_Index_List[0], evt->MTU, evt->MPS, evt->Initial_Credits);
            BleL2capCocEvent coc_evt = {
                .type = BleL2capCocEventConnected,
                .channel_index = evt->Channel_Index_List[0],
                .connected = {
                    .peer_mtu = evt->MTU,
                    .peer_mps = evt->MPS,
                    .initial_credits = evt->Initial_Credits,
                },
            };
            coc_callback(&coc_evt, coc_context);
        } else {
            FURI_LOG_E(TAG, "CoC connect rejected: result=0x%04X", evt->Result);
            BleL2capCocEvent coc_evt = {
                .type = BleL2capCocEventError,
                .error = {.code = evt->Result},
            };
            coc_callback(&coc_evt, coc_context);
        }
    } break;

    case ACI_L2CAP_COC_DISCONNECT_VSEVT_CODE: {
        aci_l2cap_coc_disconnect_event_rp0* evt =
            (aci_l2cap_coc_disconnect_event_rp0*)blue_evt->data;
        FURI_LOG_I(TAG, "CoC disconnected: ch=%d", evt->Channel_Index);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventDisconnected,
            .channel_index = evt->Channel_Index,
        };
        coc_callback(&coc_evt, coc_context);
    } break;

    case ACI_L2CAP_COC_FLOW_CONTROL_VSEVT_CODE: {
        aci_l2cap_coc_flow_control_event_rp0* evt =
            (aci_l2cap_coc_flow_control_event_rp0*)blue_evt->data;
        FURI_LOG_D(TAG, "CoC credits: ch=%d +%d", evt->Channel_Index, evt->Credits);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventCreditsReceived,
            .channel_index = evt->Channel_Index,
            .credits = {.credits = evt->Credits},
        };
        coc_callback(&coc_evt, coc_context);
    } break;

    case ACI_L2CAP_COC_RX_DATA_VSEVT_CODE: {
        aci_l2cap_coc_rx_data_event_rp0* evt =
            (aci_l2cap_coc_rx_data_event_rp0*)blue_evt->data;
        FURI_LOG_D(TAG, "CoC RX: ch=%d len=%d", evt->Channel_Index, evt->Length);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventDataReceived,
            .channel_index = evt->Channel_Index,
            .data = {
                .data = evt->Data,
                .data_len = evt->Length,
            },
        };
        coc_callback(&coc_evt, coc_context);
    } break;

    case ACI_L2CAP_COC_TX_POOL_AVAILABLE_VSEVT_CODE: {
        FURI_LOG_D(TAG, "CoC TX pool available");
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventTxDone,
        };
        coc_callback(&coc_evt, coc_context);
    } break;

    default:
        return BleEventNotAck;
    }

    return BleEventNotAck;
}

void ble_l2cap_coc_init(void) {
    if(!coc_handler) {
        coc_handler = ble_event_dispatcher_register_svc_handler(
            l2cap_coc_event_handler, &coc_sentinel);
        FURI_LOG_I(TAG, "L2CAP CoC initialized");
    }
}

void ble_l2cap_coc_deinit(void) {
    if(coc_handler) {
        ble_event_dispatcher_unregister_svc_handler(coc_handler);
        coc_handler = NULL;
    }
    coc_callback = NULL;
    coc_context = NULL;
}

void ble_l2cap_coc_set_callback(BleL2capCocCallback callback, void* context) {
    coc_callback = callback;
    coc_context = context;
}

bool ble_l2cap_coc_connect(
    uint16_t conn_handle,
    uint16_t spsm,
    uint16_t mtu,
    uint16_t mps,
    uint16_t initial_credits) {
    if(mps > BLE_L2CAP_COC_MPS_MAX) mps = BLE_L2CAP_COC_MPS_MAX;

    tBleStatus status = aci_l2cap_coc_connect(
        conn_handle,
        spsm,
        mtu,
        mps,
        initial_credits,
        0 /* Channel_Number: 0 = LE Credit Based (BLE 4.2+) */);

    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "CoC connect failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "CoC connect requested: SPSM=0x%04X MTU=%d MPS=%d", spsm, mtu, mps);
    return true;
}

bool ble_l2cap_coc_accept(
    uint16_t conn_handle,
    uint16_t mtu,
    uint16_t mps,
    uint16_t initial_credits,
    uint16_t result) {
    if(mps > BLE_L2CAP_COC_MPS_MAX) mps = BLE_L2CAP_COC_MPS_MAX;

    uint8_t channel_number = 0;
    uint8_t channel_index_list[1] = {0};

    tBleStatus status = aci_l2cap_coc_connect_confirm(
        conn_handle,
        mtu,
        mps,
        initial_credits,
        result,
        &channel_number,
        channel_index_list);

    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "CoC accept failed: 0x%02X", status);
        return false;
    }
    if(channel_number > 0) {
        FURI_LOG_I(TAG, "CoC accepted: ch_idx=%d", channel_index_list[0]);
    }
    return true;
}

bool ble_l2cap_coc_send(uint8_t channel_index, const uint8_t* data, uint16_t data_len) {
    if(data_len > 252) data_len = 252; // HCI command buffer limit

    tBleStatus status = aci_l2cap_coc_tx_data(channel_index, data_len, data);
    if(status == BLE_STATUS_INSUFFICIENT_RESOURCES) {
        FURI_LOG_D(TAG, "CoC TX busy (ch=%d), wait for pool event", channel_index);
        return false;
    }
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "CoC TX failed: 0x%02X", status);
        return false;
    }
    return true;
}

bool ble_l2cap_coc_flow_control(uint8_t channel_index, uint16_t credits) {
    tBleStatus status = aci_l2cap_coc_flow_control(channel_index, credits);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "CoC flow control failed: 0x%02X", status);
        return false;
    }
    return true;
}

bool ble_l2cap_coc_disconnect(uint8_t channel_index) {
    tBleStatus status = aci_l2cap_coc_disconnect(channel_index);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "CoC disconnect failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "CoC disconnect requested: ch=%d", channel_index);
    return true;
}
