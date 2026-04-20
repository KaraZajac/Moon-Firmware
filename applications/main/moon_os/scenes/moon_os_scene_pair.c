#include "../moon_os_app.h"

#define PAIR_TIMEOUT_MS    30000
#define PAIR_POLL_PERIOD_MS  300

static uint32_t poll_elapsed_ms;

static void update_popup(MoonOsApp* app, MoonConnectionState state) {
    /* Just-Works pairing: no PIN is exchanged. Keep the header stable
     * instead of flashing a decorative 6-digit placeholder the user can't
     * do anything with. */
    const bool reconnect = moon_companion_is_paired(app->moon);
    const char* header = reconnect ? "Reconnecting" : "Pairing";
    char body[96];

    switch(state) {
    case MoonConnStateConnected:
        snprintf(body, sizeof(body),
            reconnect ? "Connected\nPress back to return." :
            "Connected\nWaiting for phone...");
        break;
    case MoonConnStateConnecting:
        snprintf(body, sizeof(body),
            reconnect ? "Connecting...\nUsing stored pairing." :
            "Connecting...\nApprove pair on phone.");
        break;
    case MoonConnStateScanning:
        snprintf(body, sizeof(body),
            reconnect ? "Searching for\nyour paired phone." :
            "Scanning for phone\nwith pair mode on.");
        break;
    case MoonConnStateYielded:
        snprintf(body, sizeof(body), "Radio yielded\nto another app.");
        break;
    default:
        snprintf(body, sizeof(body),
            reconnect ? "Waiting to reconnect..." :
            "Start pair mode\non the phone.");
        break;
    }
    popup_set_header(app->popup, header, 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, body, 64, 36, AlignCenter, AlignCenter);
}

static void poll_tick(void* ctx) {
    MoonOsApp* app = ctx;
    poll_elapsed_ms += PAIR_POLL_PERIOD_MS;

    MoonConnectionState state = moon_companion_get_state(app->moon);
    update_popup(app, state);

    /* Success: service flipped "paired" to true. We can't observe that
     * from outside, but a transition to Connected is the closest proxy
     * we have — the pair RPC is fired immediately on Connected. Give it
     * 1s to land and then confirm via is_paired. */
    if(moon_companion_is_paired(app->moon)) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MoonOsEventPairDone);
        furi_timer_stop(app->poll_timer);
        return;
    }

    if(poll_elapsed_ms >= PAIR_TIMEOUT_MS) {
        /* Leave the popup in its current state so the user can read it;
         * user still has to press BACK to dismiss. */
        furi_timer_stop(app->poll_timer);
    }
}

void moon_os_scene_pair_on_enter(void* context) {
    MoonOsApp* app = context;
    poll_elapsed_ms = 0;

    if(!moon_companion_begin_pairing(app->moon, app->pin)) {
        snprintf(app->pin, sizeof(app->pin), "ERROR");
    }

    popup_reset(app->popup);
    update_popup(app, moon_companion_get_state(app->moon));
    view_dispatcher_switch_to_view(app->view_dispatcher, MoonOsViewPopup);

    if(!app->poll_timer) {
        app->poll_timer = furi_timer_alloc(poll_tick, FuriTimerTypePeriodic, app);
    }
    furi_timer_start(app->poll_timer, furi_ms_to_ticks(PAIR_POLL_PERIOD_MS));
}

bool moon_os_scene_pair_on_event(void* context, SceneManagerEvent event) {
    MoonOsApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == MoonOsEventPairDone) {
        update_popup(app, moon_companion_get_state(app->moon));
        return true;
    }
    return false;
}

void moon_os_scene_pair_on_exit(void* context) {
    MoonOsApp* app = context;
    if(app->poll_timer) furi_timer_stop(app->poll_timer);
    moon_companion_cancel_pairing(app->moon);
    popup_reset(app->popup);
}
