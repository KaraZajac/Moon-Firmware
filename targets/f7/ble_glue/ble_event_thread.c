#include "app_common.h"

#include <core/check.h>
#include <core/thread.h>
#include <core/log.h>

#include <interface/patterns/ble_thread/tl/shci_tl.h>
#include <interface/patterns/ble_thread/tl/hci_tl.h>

#include <furi_hal_cortex.h>
#include <furi.h>

#define TAG "BleEvt"

#define BLE_EVENT_THREAD_FLAG_SHCI_EVENT  (1UL << 0)
#define BLE_EVENT_THREAD_FLAG_HCI_EVENT   (1UL << 1)
#define BLE_EVENT_THREAD_FLAG_KILL_THREAD (1UL << 2)

#define BLE_EVENT_THREAD_FLAG_ALL                                         \
    (BLE_EVENT_THREAD_FLAG_SHCI_EVENT | BLE_EVENT_THREAD_FLAG_HCI_EVENT | \
     BLE_EVENT_THREAD_FLAG_KILL_THREAD)

static FuriThread* event_thread = NULL;

static int32_t ble_event_thread(void* context) {
    UNUSED(context);
    uint32_t flags = 0;

    while((flags & BLE_EVENT_THREAD_FLAG_KILL_THREAD) == 0) {
        flags =
            furi_thread_flags_wait(BLE_EVENT_THREAD_FLAG_ALL, FuriFlagWaitAny, FuriWaitForever);
        if(flags & BLE_EVENT_THREAD_FLAG_SHCI_EVENT) {
#ifdef FURI_BLE_EXTRA_LOG
            FURI_LOG_W(TAG, "shci_user_evt_proc");
#endif
            shci_user_evt_proc();
        }
        if(flags & BLE_EVENT_THREAD_FLAG_HCI_EVENT) {
#ifdef FURI_BLE_EXTRA_LOG
            FURI_LOG_W(TAG, "hci_user_evt_proc");
#endif
            hci_user_evt_proc();
        }
    }

    return 0;
}

void shci_notify_asynch_evt(void* pdata) {
    UNUSED(pdata);
    if(!event_thread) {
#ifdef FURI_BLE_EXTRA_LOG
        FURI_LOG_E(TAG, "shci: event_thread is NULL");
#endif
        return;
    }

    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_assert(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_SHCI_EVENT);
}

void hci_notify_asynch_evt(void* pdata) {
    UNUSED(pdata);
    if(!event_thread) {
#ifdef FURI_BLE_EXTRA_LOG
        FURI_LOG_E(TAG, "hci: event_thread is NULL");
#endif
        return;
    }

    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_assert(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_HCI_EVENT);
}

void ble_event_thread_stop(void) {
    if(!event_thread) {
#ifdef FURI_BLE_EXTRA_LOG
        FURI_LOG_E(TAG, "thread_stop: event_thread is NULL");
#endif
        return;
    }

    FuriThreadId thread_id = furi_thread_get_id(event_thread);
    furi_check(thread_id);
    furi_thread_flags_set(thread_id, BLE_EVENT_THREAD_FLAG_KILL_THREAD);
    furi_thread_join(event_thread);
    furi_thread_free(event_thread);
    event_thread = NULL;
}

void ble_event_thread_start(void) {
    furi_check(event_thread == NULL);

    event_thread = furi_thread_alloc_ex("BleEventWorker", 1280, ble_event_thread, NULL);
    furi_thread_set_priority(event_thread, FuriThreadPriorityHigh);
    furi_thread_start(event_thread);
}

/* Override ST's weak shci_cmd_resp_wait/release with versions that have
 * a real timeout.  ST's default shci_cmd_resp_wait is a bare while()
 * spinloop that ignores the timeout parameter entirely — if M0+ never
 * responds (FUS mid-reboot, C2 crash from bad SBRV, etc.) the MCU
 * locks up completely: no interrupts, no charging LED, no DFU.
 *
 * CmdRspStatusFlag in shci_tl.c is static, so we use our own flag
 * and override both wait and release. */
static volatile bool shci_cmd_resp_done = false;

void shci_cmd_resp_wait(uint32_t timeout) {
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(timeout * 1000);

    while(!shci_cmd_resp_done) {
        if(furi_hal_cortex_timer_is_expired(timer)) {
            FURI_LOG_E(TAG, "shci_cmd_resp_wait TIMEOUT (%lums)", timeout);
            break;
        }
        furi_delay_us(100);
    }
    shci_cmd_resp_done = false;
}

void shci_cmd_resp_release(uint32_t flag) {
    UNUSED(flag);
    shci_cmd_resp_done = true;
}
