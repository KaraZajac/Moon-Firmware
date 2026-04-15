#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_L2CAP_COC_MPS_MAX    248
#define BLE_L2CAP_COC_MTU_DEFAULT 2048
#define BLE_L2CAP_COC_CREDITS_DEFAULT 10

typedef enum {
    BleL2capCocEventConnected,
    BleL2capCocEventDisconnected,
    BleL2capCocEventDataReceived,
    BleL2capCocEventCreditsReceived,
    BleL2capCocEventTxDone,
    BleL2capCocEventError,
} BleL2capCocEventType;

typedef struct {
    BleL2capCocEventType type;
    uint8_t channel_index;
    uint16_t connection_handle; /**< BLE connection this channel belongs to */
    union {
        struct {
            uint16_t peer_mtu;
            uint16_t peer_mps;
            uint16_t initial_credits;
        } connected;
        struct {
            const uint8_t* data;
            uint16_t data_len;
        } data;
        struct {
            uint16_t credits;
        } credits;
        struct {
            uint16_t code;
        } error;
    };
} BleL2capCocEvent;

typedef void (*BleL2capCocCallback)(BleL2capCocEvent* event, void* context);

/** Initialize L2CAP CoC subsystem */
void ble_l2cap_coc_init(void);

/** Deinitialize L2CAP CoC subsystem */
void ble_l2cap_coc_deinit(void);

/** Set event callback for a specific connection.
 *  Each connection can have its own callback and context.
 *  Call with callback=NULL to unregister.
 *
 *  @param connection_handle  BLE connection handle
 *  @param callback           event callback, or NULL to unregister
 *  @param context            user context
 */
void ble_l2cap_coc_set_callback(
    uint16_t connection_handle,
    BleL2capCocCallback callback,
    void* context);

/** Initiate a CoC connection on an established BLE link.
 *  @param conn_handle   BLE connection handle
 *  @param spsm          Simplified Protocol/Service Multiplexer (0x80-0xFF for custom)
 *  @param mtu           Maximum SDU size (use BLE_L2CAP_COC_MTU_DEFAULT)
 *  @param mps           Maximum PDU payload (max BLE_L2CAP_COC_MPS_MAX = 248)
 *  @param initial_credits  Credits to grant the peer initially
 *  @return true if request sent
 */
bool ble_l2cap_coc_connect(
    uint16_t conn_handle,
    uint16_t spsm,
    uint16_t mtu,
    uint16_t mps,
    uint16_t initial_credits);

/** Accept an incoming CoC connection request.
 *  @param conn_handle   BLE connection handle
 *  @param mtu           Our MTU
 *  @param mps           Our MPS (max 248)
 *  @param initial_credits  Credits to grant
 *  @param result        0x0000 = accept, other = reject reason
 *  @return true if response sent
 */
bool ble_l2cap_coc_accept(
    uint16_t conn_handle,
    uint16_t mtu,
    uint16_t mps,
    uint16_t initial_credits,
    uint16_t result);

/** Send data on a CoC channel.
 *  @param channel_index  Channel index from connect/accept event
 *  @param data           Data to send
 *  @param data_len       Length (max 252 bytes per call)
 *  @return true if queued successfully
 */
bool ble_l2cap_coc_send(uint8_t channel_index, const uint8_t* data, uint16_t data_len);

/** Grant additional credits to the peer.
 *  @param channel_index  Channel index
 *  @param credits        Number of credits to grant
 *  @return true if sent
 */
bool ble_l2cap_coc_flow_control(uint8_t channel_index, uint16_t credits);

/** Disconnect a CoC channel. */
bool ble_l2cap_coc_disconnect(uint8_t channel_index);

#ifdef __cplusplus
}
#endif
