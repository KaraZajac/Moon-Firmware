#include "l2cap_coc.h"
#include "event_dispatcher.h"

#include "../app_common.h"
#include <ble/ble.h>

#include <furi.h>

#define TAG "BleL2capCoC"

/* Maximum simultaneous CoC connections (matches GATT client) */
#define L2CAP_COC_MAX_CONNECTIONS 2

/* Maximum CoC channels we track for channel→connection mapping */
#define L2CAP_COC_MAX_CHANNELS 4

/** Per-connection L2CAP CoC state */
typedef struct {
    uint16_t connection_handle;
    bool active;
    BleL2capCocCallback callback;
    void* context;
} L2capCocConnection;

/** Channel-to-connection mapping entry.
 *  L2CAP CoC data events only carry channel_index, not connection_handle.
 *  We maintain this table so we can route events to the correct callback. */
typedef struct {
    uint8_t channel_index;
    uint16_t connection_handle;
    bool active;
} L2capChannelMap;

static L2capCocConnection coc_connections[L2CAP_COC_MAX_CONNECTIONS];
static L2capChannelMap channel_map[L2CAP_COC_MAX_CHANNELS];
static GapSvcEventHandler* coc_handler = NULL;
static uint8_t coc_sentinel = 0;

/* ── Lookup helpers ────────────────────────────────────────────── */

static L2capCocConnection* coc_find_connection(uint16_t connection_handle) {
    for(int i = 0; i < L2CAP_COC_MAX_CONNECTIONS; i++) {
        if(coc_connections[i].active &&
           coc_connections[i].connection_handle == connection_handle) {
            return &coc_connections[i];
        }
    }
    return NULL;
}

static L2capCocConnection* coc_alloc_connection(uint16_t connection_handle) {
    L2capCocConnection* existing = coc_find_connection(connection_handle);
    if(existing) return existing;

    for(int i = 0; i < L2CAP_COC_MAX_CONNECTIONS; i++) {
        if(!coc_connections[i].active) {
            memset(&coc_connections[i], 0, sizeof(L2capCocConnection));
            coc_connections[i].connection_handle = connection_handle;
            coc_connections[i].active = true;
            return &coc_connections[i];
        }
    }
    FURI_LOG_E(TAG, "No free CoC connection slots (max %d)", L2CAP_COC_MAX_CONNECTIONS);
    return NULL;
}

static void coc_free_connection(uint16_t connection_handle) {
    for(int i = 0; i < L2CAP_COC_MAX_CONNECTIONS; i++) {
        if(coc_connections[i].active &&
           coc_connections[i].connection_handle == connection_handle) {
            coc_connections[i].active = false;
        }
    }
    /* Also remove any channel mappings for this connection */
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(channel_map[i].active &&
           channel_map[i].connection_handle == connection_handle) {
            channel_map[i].active = false;
        }
    }
}

static void coc_register_channel(uint8_t channel_index, uint16_t connection_handle) {
    /* Update existing or find free slot */
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(channel_map[i].active && channel_map[i].channel_index == channel_index) {
            channel_map[i].connection_handle = connection_handle;
            return;
        }
    }
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(!channel_map[i].active) {
            channel_map[i].channel_index = channel_index;
            channel_map[i].connection_handle = connection_handle;
            channel_map[i].active = true;
            return;
        }
    }
    FURI_LOG_E(TAG, "No free channel map slots");
}

static void coc_unregister_channel(uint8_t channel_index) {
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(channel_map[i].active && channel_map[i].channel_index == channel_index) {
            channel_map[i].active = false;
            return;
        }
    }
}

static L2capCocConnection* coc_find_by_channel(uint8_t channel_index) {
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(channel_map[i].active && channel_map[i].channel_index == channel_index) {
            return coc_find_connection(channel_map[i].connection_handle);
        }
    }
    return NULL;
}

static uint16_t coc_get_conn_handle_for_channel(uint8_t channel_index) {
    for(int i = 0; i < L2CAP_COC_MAX_CHANNELS; i++) {
        if(channel_map[i].active && channel_map[i].channel_index == channel_index) {
            return channel_map[i].connection_handle;
        }
    }
    return 0;
}

/* ── Event handler ─────────────────────────────────────────────── */

static BleEventAckStatus l2cap_coc_event_handler(void* event, void* context) {
    UNUSED(context);
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;

    switch(blue_evt->ecode) {
    case ACI_L2CAP_COC_CONNECT_VSEVT_CODE: {
        /* Incoming CoC connection request — carries Connection_Handle */
        aci_l2cap_coc_connect_event_rp0* evt =
            (aci_l2cap_coc_connect_event_rp0*)blue_evt->data;
        L2capCocConnection* conn = coc_find_connection(evt->Connection_Handle);
        if(!conn || !conn->callback) break;

        FURI_LOG_I(TAG, "CoC connect request: conn=0x%04X SPSM=0x%04X MTU=%d MPS=%d credits=%d",
            evt->Connection_Handle, evt->SPSM, evt->MTU, evt->MPS, evt->Initial_Credits);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventConnected,
            .channel_index = 0,
            .connection_handle = evt->Connection_Handle,
            .connected = {
                .peer_mtu = evt->MTU,
                .peer_mps = evt->MPS,
                .initial_credits = evt->Initial_Credits,
            },
        };
        conn->callback(&coc_evt, conn->context);
    } break;

    case ACI_L2CAP_COC_CONNECT_CONFIRM_VSEVT_CODE: {
        /* Response to our CoC connect request — carries Connection_Handle */
        aci_l2cap_coc_connect_confirm_event_rp0* evt =
            (aci_l2cap_coc_connect_confirm_event_rp0*)blue_evt->data;
        L2capCocConnection* conn = coc_find_connection(evt->Connection_Handle);
        if(!conn || !conn->callback) break;

        if(evt->Result == 0x0000 && evt->Channel_Number > 0) {
            uint8_t ch_idx = evt->Channel_Index_List[0];
            coc_register_channel(ch_idx, evt->Connection_Handle);
            FURI_LOG_I(TAG, "CoC connected: conn=0x%04X ch=%d MTU=%d MPS=%d credits=%d",
                evt->Connection_Handle, ch_idx, evt->MTU, evt->MPS, evt->Initial_Credits);
            BleL2capCocEvent coc_evt = {
                .type = BleL2capCocEventConnected,
                .channel_index = ch_idx,
                .connection_handle = evt->Connection_Handle,
                .connected = {
                    .peer_mtu = evt->MTU,
                    .peer_mps = evt->MPS,
                    .initial_credits = evt->Initial_Credits,
                },
            };
            conn->callback(&coc_evt, conn->context);
        } else {
            FURI_LOG_E(TAG, "CoC connect rejected: result=0x%04X", evt->Result);
            BleL2capCocEvent coc_evt = {
                .type = BleL2capCocEventError,
                .connection_handle = evt->Connection_Handle,
                .error = {.code = evt->Result},
            };
            conn->callback(&coc_evt, conn->context);
        }
    } break;

    case ACI_L2CAP_COC_DISCONNECT_VSEVT_CODE: {
        /* Channel disconnect — uses channel_index, look up connection */
        aci_l2cap_coc_disconnect_event_rp0* evt =
            (aci_l2cap_coc_disconnect_event_rp0*)blue_evt->data;
        uint16_t conn_handle = coc_get_conn_handle_for_channel(evt->Channel_Index);
        L2capCocConnection* conn = coc_find_by_channel(evt->Channel_Index);

        coc_unregister_channel(evt->Channel_Index);

        if(!conn || !conn->callback) break;
        FURI_LOG_I(TAG, "CoC disconnected: ch=%d conn=0x%04X", evt->Channel_Index, conn_handle);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventDisconnected,
            .channel_index = evt->Channel_Index,
            .connection_handle = conn_handle,
        };
        conn->callback(&coc_evt, conn->context);
    } break;

    case ACI_L2CAP_COC_FLOW_CONTROL_VSEVT_CODE: {
        aci_l2cap_coc_flow_control_event_rp0* evt =
            (aci_l2cap_coc_flow_control_event_rp0*)blue_evt->data;
        L2capCocConnection* conn = coc_find_by_channel(evt->Channel_Index);
        if(!conn || !conn->callback) break;

        FURI_LOG_D(TAG, "CoC credits: ch=%d +%d", evt->Channel_Index, evt->Credits);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventCreditsReceived,
            .channel_index = evt->Channel_Index,
            .connection_handle = conn->connection_handle,
            .credits = {.credits = evt->Credits},
        };
        conn->callback(&coc_evt, conn->context);
    } break;

    case ACI_L2CAP_COC_RX_DATA_VSEVT_CODE: {
        aci_l2cap_coc_rx_data_event_rp0* evt =
            (aci_l2cap_coc_rx_data_event_rp0*)blue_evt->data;
        L2capCocConnection* conn = coc_find_by_channel(evt->Channel_Index);
        if(!conn || !conn->callback) break;

        FURI_LOG_D(TAG, "CoC RX: ch=%d len=%d", evt->Channel_Index, evt->Length);
        BleL2capCocEvent coc_evt = {
            .type = BleL2capCocEventDataReceived,
            .channel_index = evt->Channel_Index,
            .connection_handle = conn->connection_handle,
            .data = {
                .data = evt->Data,
                .data_len = evt->Length,
            },
        };
        conn->callback(&coc_evt, conn->context);
    } break;

    case ACI_L2CAP_COC_TX_POOL_AVAILABLE_VSEVT_CODE: {
        /* Global TX pool event — notify all active connections */
        FURI_LOG_D(TAG, "CoC TX pool available");
        for(int i = 0; i < L2CAP_COC_MAX_CONNECTIONS; i++) {
            if(coc_connections[i].active && coc_connections[i].callback) {
                BleL2capCocEvent coc_evt = {
                    .type = BleL2capCocEventTxDone,
                    .connection_handle = coc_connections[i].connection_handle,
                };
                coc_connections[i].callback(&coc_evt, coc_connections[i].context);
            }
        }
    } break;

    default:
        return BleEventNotAck;
    }

    return BleEventNotAck;
}

/* ── Public API ────────────────────────────────────────────────── */

void ble_l2cap_coc_init(void) {
    if(!coc_handler) {
        memset(coc_connections, 0, sizeof(coc_connections));
        memset(channel_map, 0, sizeof(channel_map));

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
    memset(coc_connections, 0, sizeof(coc_connections));
    memset(channel_map, 0, sizeof(channel_map));
}

void ble_l2cap_coc_set_callback(
    uint16_t connection_handle,
    BleL2capCocCallback callback,
    void* context) {
    if(callback) {
        L2capCocConnection* conn = coc_alloc_connection(connection_handle);
        if(conn) {
            conn->callback = callback;
            conn->context = context;
        }
    } else {
        coc_free_connection(connection_handle);
    }
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
    FURI_LOG_I(TAG, "CoC connect requested: conn=0x%04X SPSM=0x%04X MTU=%d MPS=%d",
        conn_handle, spsm, mtu, mps);
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
        coc_register_channel(channel_index_list[0], conn_handle);
        FURI_LOG_I(TAG, "CoC accepted: conn=0x%04X ch_idx=%d", conn_handle, channel_index_list[0]);
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
