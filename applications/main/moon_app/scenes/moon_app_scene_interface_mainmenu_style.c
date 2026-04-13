#include "../moon_app.h"

// Reference the menu style names from mainmenu scene
extern const char* const menu_style_names[MenuStyleCount];

void moon_app_scene_interface_mainmenu_style_submenu_callback(void* context, uint32_t index) {
    MoonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void moon_app_scene_interface_mainmenu_style_on_enter(void* context) {
    MoonApp* app = context;
    Submenu* submenu = app->submenu;

    for(size_t i = 0; i < MenuStyleCount; i++) {
        submenu_add_item(
            submenu,
            menu_style_names[i],
            i,
            moon_app_scene_interface_mainmenu_style_submenu_callback,
            app);
    }

    submenu_set_header(submenu, "Choose Menu Style:");
    submenu_set_selected_item(submenu, moon_settings.menu_style);
    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewSubmenu);
}

bool moon_app_scene_interface_mainmenu_style_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        moon_settings.menu_style = event.event;
        app->save_settings = true;
        scene_manager_previous_scene(app->scene_manager);
    }

    return consumed;
}

void moon_app_scene_interface_mainmenu_style_on_exit(void* context) {
    MoonApp* app = context;
    submenu_reset(app->submenu);
}
