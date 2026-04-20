#include "../moon_os_app.h"

enum StartIndex {
    StartIndexStatus,
    StartIndexPair,
    StartIndexForget,
};

static const char* connection_state_text(MoonConnectionState s) {
    switch(s) {
    case MoonConnStateDisconnected: return "Disconnected";
    case MoonConnStateScanning:     return "Scanning";
    case MoonConnStateConnecting:   return "Connecting";
    case MoonConnStateConnected:    return "Connected";
    case MoonConnStateYielded:      return "Yielded";
    }
    return "?";
}

static void on_enter_cb(void* context, uint32_t index) {
    MoonOsApp* app = context;
    switch(index) {
    case StartIndexPair:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MoonOsEventPair);
        break;
    case StartIndexForget:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MoonOsEventForget);
        break;
    default:
        break;
    }
}

void moon_os_scene_start_on_enter(void* context) {
    MoonOsApp* app = context;
    VariableItemList* list = app->var_item_list;
    VariableItem* item;

    /* Status row — read-only summary of the service's current state. */
    item = variable_item_list_add(list, "Status", 1, NULL, NULL);
    variable_item_set_current_value_text(
        item, connection_state_text(moon_companion_get_state(app->moon)));

    /* Pair / Forget are fired via enter callback. */
    variable_item_list_add(
        list, moon_companion_is_paired(app->moon) ? "Re-pair Phone" : "Pair Phone",
        1, NULL, NULL);
    variable_item_list_add(list, "Forget Phone", 1, NULL, NULL);

    variable_item_list_set_enter_callback(list, on_enter_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonOsViewList);
}

bool moon_os_scene_start_on_event(void* context, SceneManagerEvent event) {
    MoonOsApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == MoonOsEventPair) {
        scene_manager_next_scene(app->scene_manager, MoonOsScenePair);
        return true;
    }
    if(event.event == MoonOsEventForget) {
        moon_companion_forget(app->moon);
        /* Rebuild the list so "Pair Phone" label flips back. */
        variable_item_list_reset(app->var_item_list);
        moon_os_scene_start_on_enter(app);
        return true;
    }
    return false;
}

void moon_os_scene_start_on_exit(void* context) {
    MoonOsApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
