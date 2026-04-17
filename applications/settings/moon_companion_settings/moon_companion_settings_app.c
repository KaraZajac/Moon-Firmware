#include "moon_companion_settings_app.h"

static bool custom_event_cb(void* context, uint32_t event) {
    MoonCompSettingsApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool back_event_cb(void* context) {
    MoonCompSettingsApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static MoonCompSettingsApp* app_alloc(void) {
    MoonCompSettingsApp* app = malloc(sizeof(MoonCompSettingsApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->moon = furi_record_open(RECORD_MOON_COMPANION);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&moon_companion_settings_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, back_event_cb);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MoonCompSettingsViewList,
        variable_item_list_get_view(app->var_item_list));

    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, MoonCompSettingsViewPopup, popup_get_view(app->popup));

    app->poll_timer = NULL;
    memset(app->pin, 0, sizeof(app->pin));

    scene_manager_next_scene(app->scene_manager, MoonCompSettingsSceneStart);
    return app;
}

static void app_free(MoonCompSettingsApp* app) {
    if(app->poll_timer) {
        furi_timer_stop(app->poll_timer);
        furi_timer_free(app->poll_timer);
    }

    view_dispatcher_remove_view(app->view_dispatcher, MoonCompSettingsViewList);
    variable_item_list_free(app->var_item_list);

    view_dispatcher_remove_view(app->view_dispatcher, MoonCompSettingsViewPopup);
    popup_free(app->popup);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_MOON_COMPANION);
    free(app);
}

int32_t moon_companion_settings_app(void* p) {
    UNUSED(p);
    MoonCompSettingsApp* app = app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
