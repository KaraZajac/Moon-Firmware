#include "gap.h"

#include "app_common.h"
#include <core/mutex.h>
#include "furi_ble/event_dispatcher.h"
#include <ble/ble.h>

#include <furi_hal.h>
#include <furi.h>
#include <stdint.h>

#define TAG "BleGap"

#define FAST_ADV_TIMEOUT    30000
#define INITIAL_ADV_TIMEOUT 60000

#define GAP_INTERVAL_TO_MS(x) (uint16_t)((x) * 1.25)

/* Max connection slots — sized for CFG_BLE_NUM_LINK (8).
 * Actual runtime limit set by moon_settings.ble_max_connections */
#define GAP_MAX_CONNECTIONS CFG_BLE_NUM_LINK

typedef struct {
    uint16_t handle;
    bool active;
    bool is_central;
    GapConnectionParams params;
    uint8_t negotiation_round;
} GapConnectionSlot;

typedef struct {
    uint16_t gap_svc_handle;
    uint16_t dev_name_char_handle;
    uint16_t appearance_char_handle;
    uint16_t connection_handle; // legacy: first active handle for backward compat
    GapConnectionSlot connections[GAP_MAX_CONNECTIONS];
    uint8_t adv_svc_uuid_len;
    uint8_t adv_svc_uuid[20];
    uint8_t mfg_data_len;
    uint8_t mfg_data[23];
    char* adv_name;
} GapSvc;

typedef struct {
    GapSvc service;
    GapConfig* config;
    GapConnectionParams connection_params;
    GapState state;
    uint8_t activities; // bitmask of GapActivity flags
    bool adv_fast;      // true = fast advertising, false = low power
    FuriMutex* state_mutex;
    GapEventCallback on_event_cb;
    void* context;
    FuriTimer* advertise_timer;
    FuriThread* thread;
    FuriMessageQueue* command_queue;
    bool enable_adv;
    bool is_secure;
    bool was_advertising; // set when adv stopped for scan/connect, cleared on restart
    GapScanCallback scan_callback;
    void* scan_context;
    FuriTimer* scan_timer;
    GapScanParams pending_scan_params;
    volatile bool scan_result;
    FuriSemaphore* scan_semaphore;
    uint32_t fixed_pin; /**< Non-zero = use this PIN for passkey auth */
} Gap;

typedef enum {
    GapCommandAdvFast,
    GapCommandAdvLowPower,
    GapCommandAdvStop,
    GapCommandScanStart,
    GapCommandScanStop,
    GapCommandVerifyParams, // deferred connection parameter verification
    GapCommandKillThread,
} GapCommand;

static Gap* gap = NULL;

static void gap_advertise_start(GapState new_state);
static int32_t gap_app(void* context);
static void gap_scan_timer_callback(void* context);

/* ── Multi-connection helpers ────────────────────────────────────────── */

static GapConnectionSlot* gap_find_connection(uint16_t handle) {
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active && gap->service.connections[i].handle == handle) {
            return &gap->service.connections[i];
        }
    }
    return NULL;
}

static GapConnectionSlot* gap_alloc_connection(uint16_t handle, bool is_central) {
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(!gap->service.connections[i].active) {
            gap->service.connections[i].handle = handle;
            gap->service.connections[i].active = true;
            gap->service.connections[i].is_central = is_central;
            // Legacy handle: prefer peripheral connections (phone companion compat)
            if(!is_central || gap->service.connection_handle == 0) {
                gap->service.connection_handle = handle;
            }
            return &gap->service.connections[i];
        }
    }
    return NULL; // all slots full
}

static void gap_free_connection(uint16_t handle) {
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active && gap->service.connections[i].handle == handle) {
            gap->service.connections[i].active = false;
            break;
        }
    }
    // Update legacy handle: prefer peripheral connection for phone companion compat
    gap->service.connection_handle = 0;
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active && !gap->service.connections[i].is_central) {
            gap->service.connection_handle = gap->service.connections[i].handle;
            return;
        }
    }
    // No peripheral — fall back to any active connection
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active) {
            gap->service.connection_handle = gap->service.connections[i].handle;
            return;
        }
    }
}

static uint8_t gap_active_connection_count(void) {
    uint8_t count = 0;
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active) count++;
    }
    return count;
}

/* Check if any connection is in central role (used by dual-role apps) */
__attribute__((unused))
static bool gap_has_central_connection(void) {
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active && gap->service.connections[i].is_central) return true;
    }
    return false;
}

static bool gap_is_connection_central(uint16_t handle) {
    GapConnectionSlot* slot = gap_find_connection(handle);
    return slot ? slot->is_central : false;
}

static void gap_verify_connection_parameters(Gap* gap, uint16_t conn_handle) {
    furi_check(gap);

    GapConnectionSlot* slot = gap_find_connection(conn_handle);
    if(!slot || slot->is_central) return; // Only negotiate for peripheral connections

    FURI_LOG_I(
        TAG,
        "Connection parameters: Connection Interval: %d (%d ms), Slave Latency: %d, Supervision Timeout: %d",
        slot->params.conn_interval,
        GAP_INTERVAL_TO_MS(slot->params.conn_interval),
        slot->params.slave_latency,
        slot->params.supervisor_timeout);

    GapConnectionParamsRequest* params = &gap->config->conn_param;

    uint16_t connection_interval_max = slot->negotiation_round ? params->conn_int_max :
                                                                  params->conn_int_min;

    bool negotiation_failed = params->conn_int_min > slot->params.conn_interval;

    if(gap->is_secure) {
        negotiation_failed |= connection_interval_max < slot->params.conn_interval;
    }

    if(negotiation_failed) {
        FURI_LOG_W(
            TAG,
            "Connection interval doesn't suite us. Trying to negotiate, round %u",
            slot->negotiation_round + 1);
        if(aci_l2cap_connection_parameter_update_req(
               conn_handle,
               params->conn_int_min,
               connection_interval_max,
               slot->params.slave_latency,
               slot->params.supervisor_timeout)) {
            FURI_LOG_E(TAG, "Failed to request connection parameters update");
            slot->negotiation_round = 0;
        } else {
            slot->negotiation_round++;
        }
    } else {
        FURI_LOG_I(
            TAG,
            "Connection interval suits us. Spent %u rounds to negotiate",
            slot->negotiation_round);
        slot->negotiation_round = 0;
    }
}

BleEventFlowStatus ble_event_app_notification(void* pckt) {
    hci_event_pckt* event_pckt;
    evt_le_meta_event* meta_evt;
    evt_blecore_aci* blue_evt;
    hci_le_phy_update_complete_event_rp0* evt_le_phy_update_complete;
    uint8_t tx_phy;
    uint8_t rx_phy;
    tBleStatus ret = BLE_STATUS_INVALID_PARAMS;

    event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;

    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    switch(event_pckt->evt) {
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        hci_disconnection_complete_event_rp0* disconnection_complete_event =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;
        uint16_t disc_handle = disconnection_complete_event->Connection_Handle;
        GapConnectionSlot* slot = gap_find_connection(disc_handle);
        bool was_central = slot ? slot->is_central : false;

        FURI_LOG_I(
            TAG, "Disconnect. Handle: 0x%04X Reason: %02X Role: %s",
            disc_handle, disconnection_complete_event->Reason,
            was_central ? "central" : "peripheral");

        gap_free_connection(disc_handle);

        // Update state and activity flags based on remaining connections
        if(gap_active_connection_count() == 0) {
            gap->activities &= ~GapActivityConnected;
            gap->state = GapStateIdle;
            gap->is_secure = false;
        }
        furi_delay_us(666 + 666);

        /* Restart advertising if needed — queue the command instead of calling
         * gap_advertise_start() directly, because we're on the BLE event thread
         * holding the state_mutex. Direct HCI calls here would block the event
         * thread and starve the TX notification buffer (causing error 100). */
        if(gap->enable_adv && gap_active_connection_count() == 0) {
            gap->was_advertising = false;
            GapCommand adv_cmd = GapCommandAdvFast;
            furi_message_queue_put(gap->command_queue, &adv_cmd, 0);
        } else if(gap->enable_adv && was_central && gap->was_advertising) {
            gap->was_advertising = false;
            GapCommand adv_cmd = GapCommandAdvFast;
            furi_message_queue_put(gap->command_queue, &adv_cmd, 0);
        }

        GapEvent event = {.type = GapEventTypeDisconnected};
        gap->on_event_cb(event, gap->context);
    } break;

    case HCI_LE_META_EVT_CODE:
        meta_evt = (evt_le_meta_event*)event_pckt->data;
        switch(meta_evt->subevent) {
        case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_update_complete_event_rp0* event =
                (hci_le_connection_update_complete_event_rp0*)meta_evt->data;
            // Update per-connection params
            GapConnectionSlot* update_slot = gap_find_connection(event->Connection_Handle);
            if(update_slot) {
                update_slot->params.conn_interval = event->Conn_Interval;
                update_slot->params.slave_latency = event->Conn_Latency;
                update_slot->params.supervisor_timeout = event->Supervision_Timeout;
            }
            // Keep global params in sync for backward compat
            gap->connection_params.conn_interval = event->Conn_Interval;
            gap->connection_params.slave_latency = event->Conn_Latency;
            gap->connection_params.supervisor_timeout = event->Supervision_Timeout;
            FURI_LOG_I(TAG, "Connection parameters event complete");
            // Queue verification to gap_app thread — don't make blocking HCI calls here
            {
                GapCommand cmd = GapCommandVerifyParams;
                furi_message_queue_put(gap->command_queue, &cmd, 0);
            }
            break;
        }

        case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE:
            evt_le_phy_update_complete = (hci_le_phy_update_complete_event_rp0*)meta_evt->data;
            if(evt_le_phy_update_complete->Status) {
                FURI_LOG_E(
                    TAG, "Update PHY failed, status %d", evt_le_phy_update_complete->Status);
            } else {
                // Read PHY directly from the event — no blocking HCI call needed
                FURI_LOG_I(TAG, "Update PHY succeed");
                FURI_LOG_I(TAG, "PHY Params TX = %d, RX = %d",
                    evt_le_phy_update_complete->TX_PHY,
                    evt_le_phy_update_complete->RX_PHY);
            }
            break;

        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_complete_event_rp0* event =
                (hci_le_connection_complete_event_rp0*)meta_evt->data;
            // Use HCI Role field: 0x00=Central, 0x01=Peripheral (authoritative)
            bool is_central = (event->Role == 0x00);

            FURI_LOG_I(
                TAG,
                "Connected: handle=0x%04X role=%s interval=%d",
                event->Connection_Handle,
                is_central ? "central" : "peripheral",
                event->Conn_Interval);

            // Allocate connection slot
            GapConnectionSlot* slot = gap_alloc_connection(
                event->Connection_Handle, is_central);
            if(!slot) {
                FURI_LOG_E(TAG, "No free connection slot!");
                aci_gap_terminate(event->Connection_Handle, 0x13);
                break;
            }

            // Store initial params in the connection slot
            slot->params.conn_interval = event->Conn_Interval;
            slot->params.slave_latency = event->Conn_Latency;
            slot->params.supervisor_timeout = event->Supervision_Timeout;
            slot->negotiation_round = 0;
            // Keep global params in sync for backward compat
            gap->connection_params = slot->params;

            gap->state = GapStateConnected;
            gap->activities |= GapActivityConnected;
            gap->activities &= ~GapActivityConnecting; // connection attempt completed

            if(!is_central) {
                /* Peripheral role: BLE controller auto-stops advertising on connect.
                 * Clear the activity flag to keep state consistent. */
                furi_timer_stop(gap->advertise_timer);
                gap->activities &= ~GapActivityAdvertising;
                gap->adv_fast = false;
                // Queue param verification to gap_app thread (avoid blocking event thread)
                {
                    GapCommand cmd = GapCommandVerifyParams;
                    furi_message_queue_put(gap->command_queue, &cmd, 0);
                }
                if(gap->config->pairing_method != GapPairingNone) {
                    aci_gap_slave_security_req(event->Connection_Handle);
                }
            } else {
                /* Central role: restart advertising so other devices can still
                 * connect to our peripheral services (dual-role support).
                 * Queue the command — don't call gap_advertise_start() directly
                 * from the event thread (blocks TX notifications). */
                if(gap->was_advertising && gap->enable_adv) {
                    FURI_LOG_I(TAG, "Queuing advertising restart after central connect");
                    gap->was_advertising = false;
                    GapCommand adv_cmd = GapCommandAdvFast;
                    furi_message_queue_put(gap->command_queue, &adv_cmd, 0);
                }
            }
        } break;

        case HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE: {
            GapScanCallback callback = gap->scan_callback;
            void* scan_ctx = gap->scan_context;
            if(callback) {
                /* Parse raw HCI buffer directly — the Advertising_Report_t
                 * struct has a pointer field that doesn't match the wire
                 * format (variable-length inline data), so we walk bytes. */
                const uint8_t* raw = meta_evt->data;
                uint8_t num_reports = raw[0];
                const uint8_t* ptr = &raw[1];

                furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
                for(uint8_t i = 0; i < num_reports; i++) {
                    /* Per-report wire layout:
                     * [0]    event_type
                     * [1]    address_type
                     * [2..7] address (6 bytes)
                     * [8]    data_length (N)
                     * [9..9+N-1] advertising data
                     * [9+N]  RSSI (signed) */
                    uint8_t addr_type = ptr[1];
                    const uint8_t* address = &ptr[2];
                    uint8_t data_len = ptr[8];
                    const uint8_t* data = &ptr[9];
                    int8_t rssi = (int8_t)ptr[9 + data_len];

                    GapScanResultData result = {
                        .address_type = addr_type,
                        .rssi = rssi,
                        .data = data,
                        .data_len = data_len,
                    };
                    memcpy(result.address, address, GAP_MAC_ADDR_SIZE);
                    callback(&result, scan_ctx);
                    ptr += 10 + data_len;
                }
                furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
            }
        } break;

        case HCI_LE_EXTENDED_ADVERTISING_REPORT_SUBEVT_CODE: {
            GapScanCallback callback = gap->scan_callback;
            void* scan_ctx = gap->scan_context;
            if(callback) {
                /* Extended advertising report — larger data, additional fields.
                 * Wire format per report:
                 * [0-1]  event_type (uint16)
                 * [2]    address_type
                 * [3-8]  address (6 bytes)
                 * [9]    primary_phy
                 * [10]   secondary_phy
                 * [11]   advertising_sid
                 * [12]   tx_power (int8)
                 * [13]   rssi (int8)
                 * [14-15] periodic_adv_interval (uint16)
                 * [16]   direct_address_type
                 * [17-22] direct_address (6 bytes)
                 * [23]   data_length
                 * [24..] data */
                const uint8_t* raw = meta_evt->data;
                uint8_t num_reports = raw[0];
                const uint8_t* ptr = &raw[1];

                furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
                for(uint8_t i = 0; i < num_reports; i++) {
                    uint8_t addr_type = ptr[2];
                    const uint8_t* address = &ptr[3];
                    int8_t rssi = (int8_t)ptr[13];
                    uint8_t data_len = ptr[23];
                    const uint8_t* data = &ptr[24];

                    GapScanResultData result = {
                        .address_type = addr_type,
                        .rssi = rssi,
                        .data = data,
                        .data_len = data_len,
                    };
                    memcpy(result.address, address, GAP_MAC_ADDR_SIZE);
                    callback(&result, scan_ctx);
                    ptr += 24 + data_len;
                }
                furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
            }
        } break;

        default:
            break;
        }
        break;

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE:
        blue_evt = (evt_blecore_aci*)event_pckt->data;
        switch(blue_evt->ecode) {
            aci_gap_pairing_complete_event_rp0* pairing_complete;

        case ACI_GAP_LIMITED_DISCOVERABLE_VSEVT_CODE:
            FURI_LOG_I(TAG, "Limited discoverable event");
            break;

        case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE: {
            uint16_t pk_handle =
                ((aci_gap_pass_key_req_event_rp0*)blue_evt->data)->Connection_Handle;
            uint32_t pin;
            if(gap->fixed_pin != 0) {
                // Use app-provided fixed PIN
                pin = gap->fixed_pin;
                FURI_LOG_I(TAG, "Pass key request: using fixed pin %06lu", pin);
            } else {
                // Generate random PIN code
                pin = rand() % 999999; //-V1064
            }
            aci_gap_pass_key_resp(pk_handle, pin);
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
                FURI_LOG_I(TAG, "Pass key request event. Pin: ******");
            } else {
                FURI_LOG_I(TAG, "Pass key request event. Pin: %06ld", pin);
            }
            GapEvent event = {.type = GapEventTypePinCodeShow, .data.pin_code = pin};
            gap->on_event_cb(event, gap->context);
        } break;

        case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
            aci_att_exchange_mtu_resp_event_rp0* pr = (void*)blue_evt->data;
            FURI_LOG_I(TAG, "Rx MTU size: %d", pr->Server_RX_MTU);
            // Set maximum packet size given header size is 3 bytes
            GapEvent event = {
                .type = GapEventTypeUpdateMTU, .data.max_packet_size = pr->Server_RX_MTU - 3};
            gap->on_event_cb(event, gap->context);
        } break;

        case ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE:
            FURI_LOG_D(TAG, "Authorization request event");
            break;

        case ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE:
            FURI_LOG_D(TAG, "Slave security initiated");
            gap->is_secure = true;
            break;

        case ACI_GAP_BOND_LOST_VSEVT_CODE:
            FURI_LOG_D(TAG, "Bond lost event. Start rebonding");
            aci_gap_allow_rebond(gap->service.connection_handle);
            break;

        case ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE:
            FURI_LOG_D(TAG, "Address not resolved event");
            break;

        case ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE:
            FURI_LOG_D(TAG, "Key press notification event");
            break;

        case ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE: {
            aci_gap_numeric_comparison_value_event_rp0* nc_evt =
                (aci_gap_numeric_comparison_value_event_rp0*)blue_evt->data;
            uint32_t pin = nc_evt->Numeric_Value;
            FURI_LOG_I(TAG, "Verify numeric comparison: %06lu", pin);
            GapEvent event = {.type = GapEventTypePinCodeVerify, .data.pin_code = pin};
            bool result = gap->on_event_cb(event, gap->context);
            aci_gap_numeric_comparison_value_confirm_yesno(nc_evt->Connection_Handle, result);
            break;
        }

        case ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE:
            pairing_complete = (aci_gap_pairing_complete_event_rp0*)blue_evt->data;
            if(pairing_complete->Status) {
                FURI_LOG_E(
                    TAG,
                    "Pairing failed with status: %d. Terminating connection",
                    pairing_complete->Status);
                aci_gap_terminate(pairing_complete->Connection_Handle, 5);
            } else {
                bool pair_is_central = gap_is_connection_central(
                    pairing_complete->Connection_Handle);
                FURI_LOG_I(TAG, "Pairing complete (central=%d)", pair_is_central);
                if(!pair_is_central) {
                    // Only notify BT service for peripheral connections (phone companion)
                    // Central connections (our app) handle pairing completion internally
                    GapEvent event = {.type = GapEventTypeConnected};
                    gap->on_event_cb(event, gap->context); //-V595
                }
            }
            break;

        case ACI_L2CAP_CONNECTION_UPDATE_RESP_VSEVT_CODE:
            FURI_LOG_D(TAG, "Procedure complete event");
            break;

        case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE: {
            uint16_t result =
                ((aci_l2cap_connection_update_resp_event_rp0*)(blue_evt->data))->Result;
            if(result == 0) {
                FURI_LOG_D(TAG, "Connection parameters accepted");
            } else if(result == 1) {
                FURI_LOG_D(TAG, "Connection parameters denied");
            }
            break;
        }

        case ACI_GAP_PROC_COMPLETE_VSEVT_CODE: {
            aci_gap_proc_complete_event_rp0* proc =
                (aci_gap_proc_complete_event_rp0*)blue_evt->data;
            if(proc->Procedure_Code == GAP_OBSERVATION_PROC) {
                FURI_LOG_I(TAG, "Observation procedure complete");
                gap->activities &= ~GapActivityScanning;
                if(gap->activities == 0) {
                    gap->state = GapStateIdle;
                }
                if(gap->was_advertising && gap->enable_adv) {
                    gap->was_advertising = false;
                    GapCommand adv_cmd = GapCommandAdvFast;
                    furi_message_queue_put(gap->command_queue, &adv_cmd, 0);
                }
            }
            break;
        }
        }
    default:
        break;
    }

    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);

    return BleEventFlowEnable;
}

static void set_advertisment_service_uid(uint8_t* uid, uint8_t uid_len) {
    if(uid_len == 2) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_16_BIT_SERV_UUID;
    } else if(uid_len == 4) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_32_BIT_SERV_UUID;
    } else if(uid_len == 16) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_128_BIT_SERV_UUID_CMPLT_LIST;
    }
    memcpy(&gap->service.adv_svc_uuid[gap->service.adv_svc_uuid_len], uid, uid_len);
    gap->service.adv_svc_uuid_len += uid_len;
}

static void set_manufacturer_data(uint8_t* mfg_data, uint8_t mfg_data_len) {
    furi_check(mfg_data_len <= sizeof(gap->service.mfg_data) - 2);
    gap->service.mfg_data[0] = mfg_data_len + 1;
    gap->service.mfg_data[1] = AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
    memcpy(&gap->service.mfg_data[gap->service.mfg_data_len], mfg_data, mfg_data_len);
    gap->service.mfg_data_len += mfg_data_len;
}

static void gap_init_svc(Gap* gap, const GapRootSecurityKeys* root_keys) {
    furi_check(root_keys);

    tBleStatus status;
    uint32_t srd_bd_addr[2];

    // Configure mac address
    aci_hal_write_config_data(
        CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, gap->config->mac_address);

    /* Static random Address
     * The two upper bits shall be set to 1
     * The lowest 32bits is read from the UDN to differentiate between devices
     * The RNG may be used to provide a random number on each power on
     */
    srd_bd_addr[1] = 0x0000ED6E;
    srd_bd_addr[0] = LL_FLASH_GetUDN();
    aci_hal_write_config_data(
        CONFIG_DATA_RANDOM_ADDRESS_OFFSET, CONFIG_DATA_RANDOM_ADDRESS_LEN, (uint8_t*)srd_bd_addr);
    // Set Identity root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_IR_OFFSET, CONFIG_DATA_IR_LEN, root_keys->irk);
    // Set Encryption root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_ER_OFFSET, CONFIG_DATA_ER_LEN, root_keys->erk);
    // Set TX Power to 0 dBm
    aci_hal_set_tx_power_level(1, 0x19);
    // Initialize GATT interface
    aci_gatt_init();
    // Initialize GAP interface
    // Skip fist symbol AD_TYPE_COMPLETE_LOCAL_NAME
    char* name = gap->service.adv_name + 1;
    uint8_t gap_role = GAP_PERIPHERAL_ROLE;
    if(furi_hal_bt_get_radio_stack() == FuriHalBtStackFull) {
        gap_role |= GAP_CENTRAL_ROLE | GAP_OBSERVER_ROLE;
    }
    status = aci_gap_init(
        gap_role,
        0,
        strlen(name),
        &gap->service.gap_svc_handle,
        &gap->service.dev_name_char_handle,
        &gap->service.appearance_char_handle);
    if(status) {
        FURI_LOG_E(TAG, "aci_gap_init FAILED: 0x%02X (role=0x%02X)", status, gap_role);
        if(gap_role != GAP_PERIPHERAL_ROLE) {
            /* Fall back to peripheral-only so BLE still works */
            gap_role = GAP_PERIPHERAL_ROLE;
            status = aci_gap_init(
                gap_role,
                0,
                strlen(name),
                &gap->service.gap_svc_handle,
                &gap->service.dev_name_char_handle,
                &gap->service.appearance_char_handle);
            FURI_LOG_I(TAG, "aci_gap_init peripheral-only fallback: 0x%02X", status);
        }
    } else {
        FURI_LOG_I(
            TAG,
            "aci_gap_init OK role=0x%02X (%s)",
            gap_role,
            (gap_role & GAP_CENTRAL_ROLE) ? "peripheral+central" : "peripheral");
    }

    // Set GAP characteristics
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.dev_name_char_handle,
        0,
        strlen(name),
        (uint8_t*)name);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating name characteristic: %d", status);
    }

    uint8_t gap_appearence_char_uuid[2] = {
        gap->config->appearance_char & 0xff, gap->config->appearance_char >> 8};
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.appearance_char_handle,
        0,
        2,
        gap_appearence_char_uuid);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating appearence characteristic: %d", status);
    }
    // Set default PHY
    hci_le_set_default_phy(ALL_PHYS_PREFERENCE, TX_2M_PREFERRED, RX_2M_PREFERRED);
    // Set I/O capability
    uint8_t auth_req_mitm_mode = MITM_PROTECTION_REQUIRED;
    uint8_t auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_FORBIDDEN;
    bool keypress_supported = false;
    if(gap->config->pairing_method == GapPairingPinCodeShow) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_ONLY);
    } else if(gap->config->pairing_method == GapPairingPinCodeVerifyYesNo) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
        keypress_supported = true;
    } else if(gap->config->pairing_method == GapPairingNone) {
        // "Just works" pairing method (iOS accepts it, it seems Android and Linux don't)
        auth_req_mitm_mode = MITM_PROTECTION_NOT_REQUIRED;
        auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_ALLOWED;
        // If "just works" isn't supported, we want the numeric comparaison method
        aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
        keypress_supported = true;
    }
    // Setup  authentication
    aci_gap_set_authentication_requirement(
        gap->config->bonding_mode,
        auth_req_mitm_mode,
        CFG_SC_SUPPORT,
        keypress_supported,
        CFG_ENCRYPTION_KEY_SIZE_MIN,
        CFG_ENCRYPTION_KEY_SIZE_MAX,
        auth_req_use_fixed_pin,
        0,
        CFG_IDENTITY_ADDRESS);
    // Configure whitelist
    aci_gap_configure_whitelist();
}

static void gap_advertise_start(GapState new_state) {
    tBleStatus status;
    uint16_t min_interval;
    uint16_t max_interval;

    FURI_LOG_D(TAG, "Start: %d", new_state);

    if(new_state == GapStateAdvFast) {
        min_interval = 0x80; // 80 ms
        max_interval = 0xa0; // 100 ms
    } else {
        min_interval = 0x0640; // 1 s
        max_interval = 0x0fa0; // 2.5 s
    }
    // Stop advertising timer
    furi_timer_stop(gap->advertise_timer);

    if((new_state == GapStateAdvLowPower) &&
       ((gap->state == GapStateAdvFast) || (gap->state == GapStateAdvLowPower))) {
        // Stop advertising
        status = aci_gap_set_non_discoverable();
        if(status) {
            FURI_LOG_E(TAG, "set_non_discoverable failed %d", status);
        } else {
            FURI_LOG_D(TAG, "set_non_discoverable success");
        }
    }

    if(gap->service.mfg_data_len > 0) {
        hci_le_set_scan_response_data(gap->service.mfg_data_len, gap->service.mfg_data);
    }

    // Configure advertising
    status = aci_gap_set_discoverable(
        ADV_IND,
        min_interval,
        max_interval,
        CFG_IDENTITY_ADDRESS,
        0,
        strlen(gap->service.adv_name),
        (uint8_t*)gap->service.adv_name,
        gap->service.adv_svc_uuid_len,
        gap->service.adv_svc_uuid,
        0,
        0);
    if(status) {
        FURI_LOG_E(TAG, "set_discoverable failed %d", status);
    }
    // Track advertising in activity flags
    gap->activities |= GapActivityAdvertising;
    gap->adv_fast = (new_state == GapStateAdvFast);
    // Only update legacy state if no active connections — if connected, stay Connected
    if(gap_active_connection_count() == 0) {
        gap->state = new_state;
    }
    GapEvent event = {.type = GapEventTypeStartAdvertising};
    gap->on_event_cb(event, gap->context);
    furi_timer_start(gap->advertise_timer, INITIAL_ADV_TIMEOUT);
}

static void gap_advertise_stop(void) {
    FURI_LOG_D(TAG, "Stop advertising");
    furi_timer_stop(gap->advertise_timer);

    /* Only call set_non_discoverable when we're pausing advertising for
     * scan/connect (enable_adv still true). During full shutdown (enable_adv
     * false), furi_hal_bt does SHCI reset which wipes everything — calling
     * HCI commands during teardown races with the shutdown and causes error 12. */
    if(gap->enable_adv && gap_active_connection_count() == 0 &&
       (gap->activities & GapActivityAdvertising)) {
        tBleStatus ret = aci_gap_set_non_discoverable();
        if(ret != BLE_STATUS_SUCCESS) {
            FURI_LOG_W(TAG, "set_non_discoverable returned %d (non-fatal)", ret);
        }
    }

    gap->activities &= ~GapActivityAdvertising;
    gap->adv_fast = false;
    if(gap->activities == 0) {
        gap->state = GapStateIdle;
    }

    GapEvent event = {.type = GapEventTypeStopAdvertising};
    gap->on_event_cb(event, gap->context);
}

/* Disconnect all active connections (for dual-role apps) */
__attribute__((unused))
static void gap_disconnect_all(void) {
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active) {
            aci_gap_terminate(gap->service.connections[i].handle, 0x13);
        }
    }
}

void gap_start_advertising(void) {
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    if(gap->state == GapStateIdle) {
        gap->state = GapStateStartingAdv;
        FURI_LOG_I(TAG, "Start advertising");
        gap->enable_adv = true;
        GapCommand command = GapCommandAdvFast;
        furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

void gap_stop_advertising(void) {
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    if(gap->activities || gap_active_connection_count() > 0 ||
       gap->state > GapStateIdle) {
        FURI_LOG_I(TAG, "Stop advertising (full shutdown)");
        gap->enable_adv = false;
        GapCommand command = GapCommandAdvStop;
        furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

static void gap_advetise_timer_callback(void* context) {
    UNUSED(context);
    GapCommand command = GapCommandAdvLowPower;
    furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
}

bool gap_init(
    GapConfig* config,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback on_event_cb,
    void* context) {
    if(!ble_glue_is_radio_stack_ready()) {
        return false;
    }

    furi_check(gap == NULL);

    gap = malloc(sizeof(Gap));
    gap->config = config;
    // Create advertising timer
    gap->advertise_timer = furi_timer_alloc(gap_advetise_timer_callback, FuriTimerTypeOnce, NULL);
    gap->scan_timer = furi_timer_alloc(gap_scan_timer_callback, FuriTimerTypeOnce, NULL);
    gap->scan_semaphore = furi_semaphore_alloc(1, 0);
    // Initialization of GATT & GAP layer
    gap->service.adv_name = config->adv_name;
    gap_init_svc(gap, root_keys);
    ble_event_dispatcher_init();
    // Initialization of the GAP state
    gap->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gap->state = GapStateIdle;
    gap->service.connection_handle = 0xFFFF;
    gap->enable_adv = true;

    // Command queue allocation
    gap->command_queue = furi_message_queue_alloc(8, sizeof(GapCommand));

    // Thread configuration
    gap->thread = furi_thread_alloc_ex("BleGapDriver", 1024, gap_app, gap);
    furi_thread_start(gap->thread);

    // Set initial state
    gap->is_secure = false;

    if(gap->config->mfg_data_len > 0) {
        // Offset by 2 for length + AD_TYPE_MANUFACTURER_SPECIFIC_DATA
        gap->service.mfg_data_len = 2;
        set_manufacturer_data(gap->config->mfg_data, gap->config->mfg_data_len);
    }

    gap->service.adv_svc_uuid_len = 1;
    if(gap->config->adv_service.UUID_Type == UUID_TYPE_16) {
        uint8_t adv_service_uid[2];
        adv_service_uid[0] = gap->config->adv_service.Service_UUID_16 & 0xff;
        adv_service_uid[1] = gap->config->adv_service.Service_UUID_16 >> 8;
        set_advertisment_service_uid(adv_service_uid, sizeof(adv_service_uid));
    } else if(gap->config->adv_service.UUID_Type == UUID_TYPE_128) {
        set_advertisment_service_uid(
            gap->config->adv_service.Service_UUID_128,
            sizeof(gap->config->adv_service.Service_UUID_128));
    } else {
        furi_crash("Invalid UUID type");
    }

    // Set callback
    gap->on_event_cb = on_event_cb;
    gap->context = context;

    return true;
}

GapState gap_get_state(void) {
    if(!gap) return GapStateUninitialized;

    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    // Synthesize legacy GapState from activity flags for backward compatibility.
    // Priority: Connected > Connecting > Scanning > Advertising > Idle
    GapState state;
    if(gap->activities & GapActivityConnected) {
        state = GapStateConnected;
    } else if(gap->activities & GapActivityConnecting) {
        state = GapStateConnecting;
    } else if(gap->activities & GapActivityScanning) {
        state = GapStateScanning;
    } else if(gap->activities & GapActivityAdvertising) {
        state = gap->adv_fast ? GapStateAdvFast : GapStateAdvLowPower;
    } else {
        state = GapStateIdle;
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return state;
}

void gap_thread_stop(void) {
    if(gap) {
        furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        gap->enable_adv = false;
        GapCommand command = GapCommandKillThread;
        furi_message_queue_put(gap->command_queue, &command, FuriWaitForever);
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
        furi_thread_join(gap->thread);
        furi_thread_free(gap->thread);
        gap->thread = NULL;
        // Free resources
        furi_mutex_free(gap->state_mutex);
        gap->state_mutex = NULL;
        furi_message_queue_free(gap->command_queue);
        gap->command_queue = NULL;
        furi_timer_free(gap->advertise_timer);
        gap->advertise_timer = NULL;
        furi_timer_free(gap->scan_timer);
        gap->scan_timer = NULL;
        furi_semaphore_free(gap->scan_semaphore);
        gap->scan_semaphore = NULL;

        ble_event_dispatcher_reset();
        free(gap);
        gap = NULL;
    }
}

static int32_t gap_app(void* context) {
    UNUSED(context);
    GapCommand command;
    while(1) {
        FuriStatus status = furi_message_queue_get(gap->command_queue, &command, FuriWaitForever);
        if(status != FuriStatusOk) {
            FURI_LOG_E(TAG, "Message queue get error: %d", status);
            continue;
        }
        furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        if(command == GapCommandKillThread) {
            break;
        }
        if(command == GapCommandAdvFast) {
            /* Only start advertising if state allows it (not scanning/connecting/connected) */
            if(gap->state == GapStateIdle || gap->state == GapStateStartingAdv ||
               gap->state == GapStateAdvFast || gap->state == GapStateAdvLowPower) {
                gap_advertise_start(GapStateAdvFast);
            }
        } else if(command == GapCommandAdvLowPower) {
            if(gap->state == GapStateAdvFast || gap->state == GapStateAdvLowPower ||
               gap->state == GapStateStartingAdv) {
                gap_advertise_start(GapStateAdvLowPower);
            }
        } else if(command == GapCommandAdvStop) {
            /* Stop advertising. Connection teardown is handled by furi_hal_bt
             * directly — don't fight its shutdown sequence. */
            gap_advertise_stop();
        } else if(command == GapCommandScanStart) {
            /* Try scanning without stopping advertising — the STM32WB55 Full stack
             * should support concurrent observation + advertising. If it fails,
             * fall back to stopping advertising first. */
            bool ok = false;
            if(!(gap->activities & GapActivityConnecting)) {
                uint16_t interval = gap->pending_scan_params.interval;
                uint16_t window = gap->pending_scan_params.window;
                uint8_t scan_type = gap->pending_scan_params.active ? 1 : 0;
                tBleStatus scan_status = aci_gap_start_observation_proc(
                    interval, window, scan_type, 0x00, 0, 0x00);

                /* Fallback: if scan fails while advertising, stop adv and retry */
                if(scan_status != BLE_STATUS_SUCCESS &&
                   (gap->activities & GapActivityAdvertising)) {
                    FURI_LOG_W(TAG, "Scan failed with adv active (0x%02X), stopping adv", scan_status);
                    furi_timer_stop(gap->advertise_timer);
                    aci_gap_set_non_discoverable();
                    gap->activities &= ~GapActivityAdvertising;
                    gap->was_advertising = true;
                    scan_status = aci_gap_start_observation_proc(
                        interval, window, scan_type, 0x00, 0, 0x00);
                }

                if(scan_status == BLE_STATUS_SUCCESS) {
                    gap->activities |= GapActivityScanning;
                    if(gap->pending_scan_params.timeout_ms > 0) {
                        furi_timer_start(gap->scan_timer,
                            gap->pending_scan_params.timeout_ms);
                    }
                    FURI_LOG_I(TAG, "Scanning started (interval=%d window=%d active=%d timeout=%d adv=%d)",
                        interval, window, scan_type, gap->pending_scan_params.timeout_ms,
                        !!(gap->activities & GapActivityAdvertising));
                    ok = true;
                } else {
                    FURI_LOG_E(TAG, "Start scanning failed: 0x%02X", scan_status);
                    if(gap->was_advertising && gap->enable_adv) {
                        gap->was_advertising = false;
                        gap_advertise_start(GapStateAdvFast);
                    }
                }
            } else {
                FURI_LOG_E(TAG, "Cannot scan while connecting");
            }
            gap->scan_result = ok;
            furi_semaphore_release(gap->scan_semaphore);
        } else if(command == GapCommandScanStop) {
            if(gap->activities & GapActivityScanning) {
                aci_gap_terminate_gap_proc(GAP_OBSERVATION_PROC);
                gap->activities &= ~GapActivityScanning;
                FURI_LOG_I(TAG, "Scan stopped");
                // Restart advertising if we had to stop it for scanning
                if(gap->was_advertising && gap->enable_adv &&
                   !(gap->activities & GapActivityAdvertising)) {
                    gap->was_advertising = false;
                    gap_advertise_start(GapStateAdvFast);
                }
            }
        } else if(command == GapCommandVerifyParams) {
            // Deferred connection parameter verification — safe to make HCI calls here
            // Use internal slot search (we already hold the mutex, can't call public API)
            for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
                if(gap->service.connections[i].active &&
                   !gap->service.connections[i].is_central) {
                    gap_verify_connection_parameters(
                        gap, gap->service.connections[i].handle);
                    break;
                }
            }
        }
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    }

    return 0;
}

void gap_emit_ble_beacon_status_event(bool active) {
    GapEvent event = {.type = active ? GapEventTypeBeaconStart : GapEventTypeBeaconStop};
    gap->on_event_cb(event, gap->context);
    FURI_LOG_I(TAG, "Beacon status event: %d", active);
}

/*
 * Scanning (observer role)
 */

static void gap_scan_timer_callback(void* context) {
    UNUSED(context);
    GapCommand command = GapCommandScanStop;
    furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
}

void gap_set_scan_callback(GapScanCallback callback, void* context) {
    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    gap->scan_callback = callback;
    gap->scan_context = context;
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

bool gap_start_scanning(const GapScanParams* params) {
    furi_check(gap);
    furi_check(params);

    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    /* Queue scan start for the gap_app thread which handles the full
     * advertising-stop → observation-start transition. */
    gap->pending_scan_params = *params;
    GapCommand cmd = GapCommandScanStart;
    furi_check(furi_message_queue_put(gap->command_queue, &cmd, 0) == FuriStatusOk);
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);

    /* Block until gap_app processes the scan start */
    furi_check(furi_semaphore_acquire(gap->scan_semaphore, 5000) == FuriStatusOk);
    return gap->scan_result;
}

void gap_stop_scanning(void) {
    furi_check(gap);

    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    furi_timer_stop(gap->scan_timer);
    aci_gap_terminate_gap_proc(GAP_OBSERVATION_PROC);
    gap->activities &= ~GapActivityScanning;
    if(gap->activities == 0) {
        gap->state = GapStateIdle;
    }
    FURI_LOG_I(TAG, "Scanning stopped");

    /* Restart advertising if we stopped it for scanning */
    if(gap->was_advertising && gap->enable_adv) {
        gap->was_advertising = false;
        GapCommand command = GapCommandAdvFast;
        furi_check(furi_message_queue_put(gap->command_queue, &command, 0) == FuriStatusOk);
    }

    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

/*
 * Central role connections
 */

bool gap_connect(uint8_t address_type, const uint8_t* address) {
    furi_check(gap);
    furi_check(address);

    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    /* Check if we have a free connection slot */
    if(gap_active_connection_count() >= GAP_MAX_CONNECTIONS) {
        FURI_LOG_E(TAG, "Cannot connect: all %d slots in use", GAP_MAX_CONNECTIONS);
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
        return false;
    }

    /* Don't stop advertising — the STM32WB55 BLE controller can time-slice
     * advertising and connection initiation. If aci_gap_create_connection fails
     * with BLE_STATUS_BUSY, we'll stop advertising in the retry loop. */

    /* Stop scanning before connecting (radio can't scan and create connection simultaneously) */
    if(gap->activities & GapActivityScanning) {
        furi_timer_stop(gap->scan_timer);
        aci_gap_terminate_gap_proc(GAP_OBSERVATION_PROC);
        gap->activities &= ~GapActivityScanning;
        if(gap->activities == 0) {
            gap->state = GapStateIdle;
        }
    }

    /* Allow connecting when not already connecting */
    if(gap->activities & GapActivityConnecting) {
        FURI_LOG_E(TAG, "Already connecting");
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
        return false;
    }

    /* Retry connection — if BLE controller is busy (e.g., advertising), retry with backoff.
     * On persistent failure, stop advertising as fallback and retry once more. */
    tBleStatus status = BLE_STATUS_FAILED;
    for(int retry = 0; retry < 10; retry++) {
        if(retry > 0) {
            /* If we've retried 5 times and advertising is still on, stop it as fallback */
            if(retry == 5 && (gap->activities & GapActivityAdvertising)) {
                FURI_LOG_W(TAG, "Stopping advertising as fallback for connect");
                furi_timer_stop(gap->advertise_timer);
                aci_gap_set_non_discoverable();
                gap->activities &= ~GapActivityAdvertising;
                gap->was_advertising = true;
            }
            furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
            furi_delay_ms(25);
            furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
        }
        status = aci_gap_create_connection(
            0x0060, /* scan interval 60ms */
            0x0030, /* scan window 30ms */
            address_type,
            address,
            0x00, /* public own address */
            0x0028, /* conn interval min 50ms */
            0x0038, /* conn interval max 70ms */
            0x0000, /* slave latency */
            0x03E8, /* supervision timeout 10s */
            0x0010, /* min CE length */
            0x0020 /* max CE length */);
        if(status == BLE_STATUS_SUCCESS) break;
        FURI_LOG_W(TAG, "Connect retry %d (status 0x%02X)", retry + 1, status);
    }

    if(status == BLE_STATUS_SUCCESS) {
        gap->state = GapStateConnecting;
        gap->activities |= GapActivityConnecting;
        FURI_LOG_I(
            TAG,
            "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
            address[5], address[4], address[3], address[2], address[1], address[0]);
    } else {
        FURI_LOG_E(TAG, "Connection failed: 0x%02X", status);
        /* If we stopped advertising for this connection attempt, restart it */
        if(gap->was_advertising && gap->enable_adv) {
            gap->was_advertising = false;
            GapCommand adv_cmd = GapCommandAdvFast;
            furi_message_queue_put(gap->command_queue, &adv_cmd, 0);
        }
    }

    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return status == BLE_STATUS_SUCCESS;
}

bool gap_disconnect(uint16_t connection_handle) {
    furi_check(gap);

    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);

    GapConnectionSlot* slot = gap_find_connection(connection_handle);
    if(!slot) {
        FURI_LOG_E(TAG, "Cannot disconnect: handle 0x%04X not found", connection_handle);
        furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
        return false;
    }

    tBleStatus status = aci_gap_terminate(connection_handle, 0x13 /* remote user terminated */);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Disconnect failed: 0x%02X", status);
    }

    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return status == BLE_STATUS_SUCCESS;
}

uint16_t gap_get_connection_handle(void) {
    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    uint16_t handle = gap->service.connection_handle;
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return handle;
}

uint16_t gap_get_connection_handle_by_role(bool central) {
    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    uint16_t handle = 0;
    for(int i = 0; i < GAP_MAX_CONNECTIONS; i++) {
        if(gap->service.connections[i].active &&
           gap->service.connections[i].is_central == central) {
            handle = gap->service.connections[i].handle;
            break;
        }
    }
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return handle;
}

uint8_t gap_get_connection_count(void) {
    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    uint8_t count = gap_active_connection_count();
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return count;
}

uint8_t gap_get_activities(void) {
    if(!gap) return 0;
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    uint8_t act = gap->activities;
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return act;
}

bool gap_is_activity_active(GapActivity activity) {
    if(!gap) return false;
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    bool active = (gap->activities & activity) != 0;
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
    return active;
}

void gap_set_pairing_method(uint32_t fixed_pin) {
    furi_check(gap);

    if(fixed_pin > 0) {
        // Configure for fixed PIN entry (central connecting to device with known PIN)
        // Use legacy pairing (SC unsupported) for maximum compatibility
        gap->fixed_pin = fixed_pin;
        aci_gap_set_io_capability(IO_CAP_KEYBOARD_ONLY);
        aci_gap_set_authentication_requirement(
            1, // bonding
            MITM_PROTECTION_REQUIRED, // Force passkey exchange for Security Level 3
            SC_PAIRING_UNSUPPORTED,   // Legacy pairing for Meshtastic compat
            0, // no keypress
            CFG_ENCRYPTION_KEY_SIZE_MIN,
            CFG_ENCRYPTION_KEY_SIZE_MAX,
            USE_FIXED_PIN_FOR_PAIRING_ALLOWED,
            fixed_pin,
            CFG_IDENTITY_ADDRESS);
        FURI_LOG_I(TAG, "Auth configured: fixed PIN %06lu, legacy+MITM, IO=KEYBOARD_ONLY", fixed_pin);
    } else {
        // Restore default peripheral pairing config
        gap->fixed_pin = 0;
        if(gap->config) {
            uint8_t auth_req_mitm_mode = MITM_PROTECTION_REQUIRED;
            uint8_t auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_FORBIDDEN;
            if(gap->config->pairing_method == GapPairingPinCodeShow) {
                aci_gap_set_io_capability(IO_CAP_DISPLAY_ONLY);
            } else if(gap->config->pairing_method == GapPairingPinCodeVerifyYesNo) {
                aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
            } else {
                auth_req_mitm_mode = MITM_PROTECTION_NOT_REQUIRED;
                auth_req_use_fixed_pin = USE_FIXED_PIN_FOR_PAIRING_ALLOWED;
                aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
            }
            aci_gap_set_authentication_requirement(
                gap->config->bonding_mode,
                auth_req_mitm_mode,
                CFG_SC_SUPPORT,
                0,
                CFG_ENCRYPTION_KEY_SIZE_MIN,
                CFG_ENCRYPTION_KEY_SIZE_MAX,
                auth_req_use_fixed_pin,
                0,
                CFG_IDENTITY_ADDRESS);
            FURI_LOG_I(TAG, "Auth restored to default peripheral config");
        }
    }
}

bool gap_pair(uint16_t connection_handle, bool force_rebond) {
    furi_check(gap);
    tBleStatus status = aci_gap_send_pairing_req(connection_handle, force_rebond ? 0x01 : 0x00);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Pairing request failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "Pairing request sent (handle=%d, rebond=%d)", connection_handle, force_rebond);
    return true;
}

void gap_set_fixed_pin(uint32_t pin) {
    furi_check(gap);
    furi_check(furi_mutex_acquire(gap->state_mutex, FuriWaitForever) == FuriStatusOk);
    gap->fixed_pin = pin;
    FURI_LOG_I(TAG, "Fixed PIN %s", pin ? "set" : "cleared");
    furi_check(furi_mutex_release(gap->state_mutex) == FuriStatusOk);
}

/*
 * PHY preference
 */

bool gap_set_phy_preference(uint16_t conn_handle, uint8_t tx_phy, uint8_t rx_phy) {
    furi_check(gap);
    /* ALL_PHYS=0x00: use provided TX/RX preferences
     * PHY_options=0x0000: no coded PHY preference (not supported on STM32WB) */
    tBleStatus status = hci_le_set_phy(conn_handle, 0x00, tx_phy, rx_phy, 0x0000);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Set PHY failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "PHY preference set: TX=0x%02X RX=0x%02X", tx_phy, rx_phy);
    return true;
}

bool gap_get_phy(uint16_t conn_handle, uint8_t* tx_phy, uint8_t* rx_phy) {
    furi_check(gap);
    furi_check(tx_phy);
    furi_check(rx_phy);
    tBleStatus status = hci_le_read_phy(conn_handle, tx_phy, rx_phy);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Read PHY failed: 0x%02X", status);
        return false;
    }
    return true;
}

/*
 * Extended Advertising
 */

bool gap_ext_adv_configure(
    uint8_t adv_handle,
    uint16_t adv_event_props,
    uint32_t interval_min,
    uint32_t interval_max,
    uint8_t secondary_phy,
    uint8_t adv_sid) {
    furi_check(gap);

    uint8_t peer_addr[6] = {0};
    tBleStatus status = aci_gap_adv_set_configuration(
        0x00,                    // Adv_Mode: normal
        adv_handle,
        adv_event_props,
        interval_min,
        interval_max,
        0x07,                    // All 3 advertising channels
        CFG_IDENTITY_ADDRESS,    // Own_Address_Type
        0x00,                    // Peer_Address_Type: public
        peer_addr,               // Peer_Address: unused for non-directed
        0x00,                    // Adv_Filter_Policy: process all
        127,                     // Adv_TX_Power: no preference
        0x00,                    // Secondary_Adv_Max_Skip
        secondary_phy,
        adv_sid,
        0x00                     // Scan_Req_Notification: disabled
    );
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv configure failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "Ext adv set %d configured (props=0x%04X sid=%d)", adv_handle, adv_event_props, adv_sid);
    return true;
}

bool gap_ext_adv_set_data(uint8_t adv_handle, const uint8_t* data, uint8_t data_len) {
    furi_check(gap);
    tBleStatus status = aci_gap_adv_set_adv_data(
        adv_handle,
        0x03,           // Operation: complete data
        0x01,           // Fragment_Preference: don't fragment
        data_len,
        data);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv set data failed: 0x%02X", status);
        return false;
    }
    return true;
}

bool gap_ext_adv_set_scan_resp(uint8_t adv_handle, const uint8_t* data, uint8_t data_len) {
    furi_check(gap);
    tBleStatus status = aci_gap_adv_set_scan_resp_data(
        adv_handle,
        0x03,           // Operation: complete data
        0x01,           // Fragment_Preference: don't fragment
        data_len,
        data);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv set scan resp failed: 0x%02X", status);
        return false;
    }
    return true;
}

bool gap_ext_adv_start(uint8_t adv_handle, uint16_t duration_ms) {
    furi_check(gap);
    Adv_Set_t adv_set = {
        .Advertising_Handle = adv_handle,
        .Duration = duration_ms / 10,  // Convert ms to 10ms units
        .Max_Extended_Advertising_Events = 0,  // No max
    };
    tBleStatus status = aci_gap_adv_set_enable(0x01, 1, &adv_set);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv start failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "Ext adv set %d started", adv_handle);
    return true;
}

bool gap_ext_adv_stop(uint8_t adv_handle) {
    furi_check(gap);
    Adv_Set_t adv_set = {
        .Advertising_Handle = adv_handle,
        .Duration = 0,
        .Max_Extended_Advertising_Events = 0,
    };
    tBleStatus status = aci_gap_adv_set_enable(0x00, 1, &adv_set);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv stop failed: 0x%02X", status);
        return false;
    }
    FURI_LOG_I(TAG, "Ext adv set %d stopped", adv_handle);
    return true;
}

bool gap_ext_adv_remove(uint8_t adv_handle) {
    furi_check(gap);
    tBleStatus status = aci_gap_adv_remove_set(adv_handle);
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "Ext adv remove failed: 0x%02X", status);
        return false;
    }
    return true;
}
