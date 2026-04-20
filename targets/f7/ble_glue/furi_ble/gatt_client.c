#include "gatt_client.h"
#include "event_dispatcher.h"

#include "../app_common.h"
#include <ble/ble.h>

#include <furi.h>
#include <string.h>

#define TAG "BleGattClient"

/* Maximum simultaneous GATT client connections.
 * Matches the default dual-connection config; increase if needed. */
#define GATT_CLIENT_MAX_CONNECTIONS 2

typedef enum {
    GattOpNone,
    GattOpDiscoverServices,
    GattOpDiscoverChars,
    GattOpRead,
    GattOpWrite,
    GattOpSubscribe,
} GattPendingOp;

/** Per-connection GATT client state.
 *  Each active central-role connection gets its own discovery buffers,
 *  pending operation tracker, and callback. This prevents two connections
 *  from corrupting each other's in-flight GATT operations. */
typedef struct {
    uint16_t connection_handle;
    bool active;
    BleGattClientCallback callback;
    void* context;
    GattPendingOp pending_op;
    BleGattService discovered_services[BLE_GATT_CLIENT_MAX_SERVICES];
    uint8_t discovered_services_count;
    BleGattCharacteristic discovered_chars[BLE_GATT_CLIENT_MAX_CHARS];
    uint8_t discovered_chars_count;
} GattClientConnection;

static GattClientConnection gatt_connections[GATT_CLIENT_MAX_CONNECTIONS];

static GapSvcEventHandler* gatt_client_handler = NULL;
/* event_dispatcher requires non-NULL context; use this as sentinel */
static uint8_t gatt_client_sentinel = 0;

/* ── Per-connection lookup helpers ──────────────────────────────── */

static GattClientConnection* gatt_find_connection(uint16_t connection_handle) {
    /* Exact match first. */
    for(int i = 0; i < GATT_CLIENT_MAX_CONNECTIONS; i++) {
        if(gatt_connections[i].active &&
           gatt_connections[i].connection_handle == connection_handle) {
            return &gatt_connections[i];
        }
    }
    /* Fall back to the default slot (handle=0) so single-connection apps
     * can register one callback at init without waiting for the BLE
     * connection handle to become known. Apps that need per-connection
     * routing still call set_callback(handle, …) explicitly and that slot
     * takes priority over the default. */
    if(connection_handle != 0) {
        for(int i = 0; i < GATT_CLIENT_MAX_CONNECTIONS; i++) {
            if(gatt_connections[i].active && gatt_connections[i].connection_handle == 0) {
                return &gatt_connections[i];
            }
        }
    }
    return NULL;
}

static GattClientConnection* gatt_alloc_connection(uint16_t connection_handle) {
    /* Check if already registered */
    GattClientConnection* existing = gatt_find_connection(connection_handle);
    if(existing) return existing;

    /* Find a free slot */
    for(int i = 0; i < GATT_CLIENT_MAX_CONNECTIONS; i++) {
        if(!gatt_connections[i].active) {
            memset(&gatt_connections[i], 0, sizeof(GattClientConnection));
            gatt_connections[i].connection_handle = connection_handle;
            gatt_connections[i].active = true;
            return &gatt_connections[i];
        }
    }
    FURI_LOG_E(TAG, "No free GATT client slots (max %d)", GATT_CLIENT_MAX_CONNECTIONS);
    return NULL;
}

static void gatt_free_connection(uint16_t connection_handle) {
    for(int i = 0; i < GATT_CLIENT_MAX_CONNECTIONS; i++) {
        if(gatt_connections[i].active &&
           gatt_connections[i].connection_handle == connection_handle) {
            gatt_connections[i].active = false;
            return;
        }
    }
}

/* ── Event handler ─────────────────────────────────────────────── */

static BleEventAckStatus gatt_client_event_handler(void* pckt, void* context) {
    UNUSED(context);
    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        return BleEventNotAck;
    }

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;

    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        /* Service discovery response — Connection_Handle is the first uint16 field */
        aci_att_read_by_group_type_resp_event_rp0* resp =
            (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        uint8_t attr_len = resp->Attribute_Data_Length;
        uint8_t num_attr = resp->Data_Length / attr_len;

        for(uint8_t i = 0;
            i < num_attr && conn->discovered_services_count < BLE_GATT_CLIENT_MAX_SERVICES;
            i++) {
            uint8_t* data = &resp->Attribute_Data_List[i * attr_len];
            BleGattService* svc = &conn->discovered_services[conn->discovered_services_count];
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
            conn->discovered_services_count++;
        }
    } break;

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        /* Characteristic discovery response */
        aci_att_read_by_type_resp_event_rp0* resp =
            (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        uint8_t pair_len = resp->Handle_Value_Pair_Length;
        uint8_t num_pairs = resp->Data_Length / pair_len;

        for(uint8_t i = 0;
            i < num_pairs && conn->discovered_chars_count < BLE_GATT_CLIENT_MAX_CHARS;
            i++) {
            uint8_t* data = &resp->Handle_Value_Pair_Data[i * pair_len];
            BleGattCharacteristic* chr = &conn->discovered_chars[conn->discovered_chars_count];
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
            conn->discovered_chars_count++;
        }
    } break;

    case ACI_ATT_READ_RESP_VSEVT_CODE: {
        aci_att_read_resp_event_rp0* resp = (aci_att_read_resp_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        BleGattClientEvent event = {
            .type = BleGattClientEventReadComplete,
            .connection_handle = resp->Connection_Handle,
            .read =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Event_Data_Length,
                },
        };
        conn->callback(&event, conn->context);
    } break;

    case ACI_GATT_NOTIFICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* resp =
            (aci_gatt_notification_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        FURI_LOG_D(TAG, "Notif: conn=0x%04X attr=0x%04X len=%d",
            resp->Connection_Handle, resp->Attribute_Handle,
            resp->Attribute_Value_Length);
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .connection_handle = resp->Connection_Handle,
            .notification =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Attribute_Value_Length,
                    .value_handle = resp->Attribute_Handle,
                },
        };
        conn->callback(&event, conn->context);
    } break;

    case ACI_GATT_NOTIFICATION_EXT_VSEVT_CODE: {
        /* Extended notification — used when MTU > default and data
         * exceeds the standard notification event buffer */
        aci_gatt_notification_ext_event_rp0* resp =
            (aci_gatt_notification_ext_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        FURI_LOG_D(TAG, "Notif EXT: off=0x%04X len=%d",
            resp->Offset, resp->Attribute_Value_Length);
        BleGattClientEvent event = {
            .type = BleGattClientEventNotification,
            .connection_handle = resp->Connection_Handle,
            .notification =
                {
                    .data = resp->Attribute_Value,
                    .data_len = resp->Attribute_Value_Length,
                    .value_handle = resp->Attribute_Handle,
                    .offset = resp->Offset,
                },
        };
        conn->callback(&event, conn->context);
    } break;

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* resp =
            (aci_gatt_proc_complete_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        GattPendingOp completed_op = conn->pending_op;
        conn->pending_op = GattOpNone;

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
                .connection_handle = resp->Connection_Handle,
                .error = {.error_code = resp->Error_Code},
            };
            conn->callback(&event, conn->context);
        } else {
            switch(completed_op) {
            case GattOpDiscoverServices:
                if(conn->discovered_services_count > 0) {
                    BleGattClientEvent event = {
                        .type = BleGattClientEventDiscoverComplete,
                        .connection_handle = resp->Connection_Handle,
                        .discover =
                            {
                                .services = conn->discovered_services,
                                .count = conn->discovered_services_count,
                            },
                    };
                    conn->callback(&event, conn->context);
                    conn->discovered_services_count = 0;
                }
                break;
            case GattOpDiscoverChars:
                if(conn->discovered_chars_count > 0) {
                    BleGattClientEvent event = {
                        .type = BleGattClientEventCharDiscoverComplete,
                        .connection_handle = resp->Connection_Handle,
                        .char_discover =
                            {
                                .chars = conn->discovered_chars,
                                .count = conn->discovered_chars_count,
                            },
                    };
                    conn->callback(&event, conn->context);
                    conn->discovered_chars_count = 0;
                }
                break;
            case GattOpWrite:
            case GattOpSubscribe: {
                BleGattClientEvent event = {
                    .type = BleGattClientEventWriteComplete,
                    .connection_handle = resp->Connection_Handle,
                };
                conn->callback(&event, conn->context);
            } break;
            default:
                break;
            }
        }
    } break;

    case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
        aci_gatt_error_resp_event_rp0* resp = (aci_gatt_error_resp_event_rp0*)blue_evt->data;
        GattClientConnection* conn = gatt_find_connection(resp->Connection_Handle);
        if(!conn || !conn->callback) break;

        /* 0x0A = Attribute Not Found — normal end-of-discovery, not an error */
        if(resp->Error_Code == 0x0A &&
           (conn->pending_op == GattOpDiscoverServices || conn->pending_op == GattOpDiscoverChars)) {
            FURI_LOG_D(TAG, "Discovery end signal at attr=0x%04X (conn=0x%04X)",
                resp->Attribute_Handle, resp->Connection_Handle);
        } else {
            FURI_LOG_W(TAG, "GATT error: conn=0x%04X attr=0x%04X code=0x%02X",
                resp->Connection_Handle, resp->Attribute_Handle, resp->Error_Code);
            BleGattClientEvent event = {
                .type = BleGattClientEventError,
                .connection_handle = resp->Connection_Handle,
                .error = {.error_code = resp->Error_Code},
            };
            conn->callback(&event, conn->context);
        }
    } break;

    default:
        return BleEventNotAck;
    }

    return BleEventNotAck;
}

/* ── Public API ────────────────────────────────────────────────── */

void ble_gatt_client_init(void) {
    if(!gatt_client_handler) {
        memset(gatt_connections, 0, sizeof(gatt_connections));

        gatt_client_handler = ble_event_dispatcher_register_svc_handler(
            gatt_client_event_handler, &gatt_client_sentinel);

        /* Enable the full documented GATT event set. aci_gatt_set_event_mask
         * is a *global* mask that applies to both central (client) and
         * peripheral (server) roles — restricting it here would starve the
         * peripheral path of events (e.g. TX_POOL_AVAILABLE for flow control,
         * INDICATION_EVENT for indication-based peers). gap.c also sets this
         * at stack init; this call is defense-in-depth in case a central app
         * runs first. */
        aci_gatt_set_event_mask(BLE_GATT_FULL_EVENT_MASK);
        FURI_LOG_I(TAG, "GATT client init");
    }
}

void ble_gatt_client_deinit(void) {
    if(gatt_client_handler) {
        ble_event_dispatcher_unregister_svc_handler(gatt_client_handler);
        gatt_client_handler = NULL;
    }
    memset(gatt_connections, 0, sizeof(gatt_connections));
}

void ble_gatt_client_set_callback(
    uint16_t connection_handle,
    BleGattClientCallback callback,
    void* context) {
    if(callback) {
        GattClientConnection* conn = gatt_alloc_connection(connection_handle);
        if(conn) {
            conn->callback = callback;
            conn->context = context;
        }
    } else {
        /* NULL callback = unregister this connection */
        gatt_free_connection(connection_handle);
    }
}

bool ble_gatt_client_discover_services(uint16_t connection_handle) {
    GattClientConnection* conn = gatt_find_connection(connection_handle);
    if(!conn) {
        FURI_LOG_E(TAG, "Discover services: no callback registered for conn 0x%04X", connection_handle);
        return false;
    }

    /* Already in flight — re-issuing the same op clobbers
     * discovered_services_count and, if it returns 0x0C (the ATT slot is
     * busy because of the one we just kicked off), used to reset
     * pending_op back to None, stranding the running procedure's
     * PROC_COMPLETE with no state. Treat as idempotent success. */
    if(conn->pending_op == GattOpDiscoverServices) {
        return true;
    }

    conn->discovered_services_count = 0;
    GattPendingOp prev_op = conn->pending_op;
    conn->pending_op = GattOpDiscoverServices;
    tBleStatus status = aci_gatt_disc_all_primary_services(connection_handle);
    if(status != BLE_STATUS_SUCCESS) {
        /* 0x0C (COMMAND_DISALLOWED) means some other ATT procedure is in
         * flight — often Android's system GATT client walking our
         * peripheral tree on a fresh pair. Leave any prior pending_op
         * alone so its PROC_COMPLETE still dispatches correctly; the
         * caller will retry. */
        conn->pending_op = prev_op;
        FURI_LOG_E(TAG, "Discover services failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_discover_characteristics(
    uint16_t connection_handle,
    const BleGattService* service) {
    GattClientConnection* conn = gatt_find_connection(connection_handle);
    if(!conn) {
        FURI_LOG_E(TAG, "Discover chars: no callback registered for conn 0x%04X", connection_handle);
        return false;
    }

    /* Idempotent re-issue — see rationale in ble_gatt_client_discover_services. */
    if(conn->pending_op == GattOpDiscoverChars) {
        return true;
    }

    conn->discovered_chars_count = 0;
    GattPendingOp prev_op = conn->pending_op;
    conn->pending_op = GattOpDiscoverChars;
    tBleStatus status = aci_gatt_disc_all_char_of_service(
        connection_handle, service->start_handle, service->end_handle);
    if(status != BLE_STATUS_SUCCESS) {
        /* Don't clobber a prior in-flight op's pending_op on 0x0C. */
        conn->pending_op = prev_op;
        FURI_LOG_E(TAG, "Discover chars failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_read(uint16_t connection_handle, uint16_t value_handle) {
    GattClientConnection* conn = gatt_find_connection(connection_handle);
    if(!conn) {
        FURI_LOG_E(TAG, "Read: no callback registered for conn 0x%04X", connection_handle);
        return false;
    }

    conn->pending_op = GattOpRead;
    tBleStatus status = aci_gatt_read_char_value(connection_handle, value_handle);
    if(status != BLE_STATUS_SUCCESS) {
        conn->pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Read failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}

bool ble_gatt_client_write(
    uint16_t connection_handle,
    uint16_t value_handle,
    const uint8_t* data,
    uint16_t data_len) {
    GattClientConnection* conn = gatt_find_connection(connection_handle);
    if(!conn) {
        FURI_LOG_E(TAG, "Write: no callback registered for conn 0x%04X", connection_handle);
        return false;
    }

    conn->pending_op = GattOpWrite;
    tBleStatus status =
        aci_gatt_write_char_value(connection_handle, value_handle, data_len, data);
    if(status != BLE_STATUS_SUCCESS) {
        conn->pending_op = GattOpNone;
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
    GattClientConnection* conn = gatt_find_connection(connection_handle);
    if(!conn) {
        FURI_LOG_E(TAG, "Subscribe: no callback registered for conn 0x%04X", connection_handle);
        return false;
    }

    /* Write to Client Characteristic Configuration Descriptor (CCCD).
     * CCCD handle is typically value_handle + 1. This assumption holds
     * for most standard BLE services. */
    conn->pending_op = GattOpSubscribe;
    uint16_t cccd_handle = value_handle + 1;
    uint8_t cccd_val[2] = {enable ? 0x01 : 0x00, 0x00};
    tBleStatus status =
        aci_gatt_write_char_desc(connection_handle, cccd_handle, 2, cccd_val);
    if(status != BLE_STATUS_SUCCESS) {
        conn->pending_op = GattOpNone;
        FURI_LOG_E(TAG, "Subscribe notifications failed: 0x%02X", status);
    }
    return status == BLE_STATUS_SUCCESS;
}
