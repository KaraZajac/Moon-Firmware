#include "../moon_app.h"

static void moon_app_scene_interface_mainmenu_reset_dialog_callback(
    DialogExResult result,
    void* context) {
    MoonApp* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void moon_app_scene_interface_mainmenu_reset_on_enter(void* context) {
    MoonApp* app = context;
    DialogEx* dialog_ex = app->dialog_ex;

    dialog_ex_set_header(dialog_ex, "Reset Menu Items?", 64, 10, AlignCenter, AlignCenter);
    dialog_ex_set_text(dialog_ex, "Your edits will be lost!", 64, 32, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(dialog_ex, "Cancel");
    dialog_ex_set_right_button_text(dialog_ex, "Reset");

    dialog_ex_set_context(dialog_ex, app);
    dialog_ex_set_result_callback(
        dialog_ex, moon_app_scene_interface_mainmenu_reset_dialog_callback);

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewDialogEx);
}

bool moon_app_scene_interface_mainmenu_reset_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DialogExResultRight:
            storage_common_remove(app->storage, MAINMENU_APPS_PATH);
            moon_app_empty_mainmenu_apps(app);
            moon_app_load_mainmenu_apps(app);
            app->mainmenu_app_index = 0;
            app->save_mainmenu_apps = false;
            /* fall through */
        case DialogExResultLeft:
            scene_manager_previous_scene(app->scene_manager);
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = true;
    }

    return consumed;
}

void moon_app_scene_interface_mainmenu_reset_on_exit(void* context) {
    MoonApp* app = context;
    DialogEx* dialog_ex = app->dialog_ex;

    dialog_ex_reset(dialog_ex);
}
