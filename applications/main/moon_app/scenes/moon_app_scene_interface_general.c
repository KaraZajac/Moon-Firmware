#include "../moon_app.h"

enum VarItemListIndex {
    VarItemListIndexScrollType,
    VarItemListIndexMidnightFormat,
};

void moon_app_scene_interface_general_var_item_list_callback(void* context, uint32_t index) {
    MoonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void moon_app_scene_interface_general_scroll_marquee_changed(VariableItem* item) {
    MoonApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "Marquee" : "Standard");
    moon_settings.scroll_marquee = value;
    app->save_settings = true;
}

static void moon_app_scene_interface_general_midnight_format_changed(VariableItem* item) {
    MoonApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "00:XX" : "12:00");
    moon_settings.midnight_format_00 = value;
    app->save_settings = true;
}

static void moon_app_scene_interface_general_popup_overlay_changed(VariableItem* item) {
    MoonApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    moon_settings.popup_overlay = value;
    app->save_settings = true;
}

void moon_app_scene_interface_general_on_enter(void* context) {
    MoonApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(
        var_item_list,
        "Text Scroll",
        2,
        moon_app_scene_interface_general_scroll_marquee_changed,
        app);
    variable_item_set_current_value_index(item, moon_settings.scroll_marquee);
    variable_item_set_current_value_text(
        item, moon_settings.scroll_marquee ? "Marquee" : "Standard");

    item = variable_item_list_add(
        var_item_list,
        "Clock Midnight Format",
        2,
        moon_app_scene_interface_general_midnight_format_changed,
        app);
    variable_item_set_current_value_index(item, moon_settings.midnight_format_00);
    variable_item_set_current_value_text(
        item, moon_settings.midnight_format_00 ? "00:XX" : "12:XX");

    item = variable_item_list_add(
        var_item_list,
        "Popup Overlay",
        2,
        moon_app_scene_interface_general_popup_overlay_changed,
        app);
    variable_item_set_current_value_index(item, moon_settings.popup_overlay);
    variable_item_set_current_value_text(item, moon_settings.popup_overlay ? "ON" : "OFF");

    variable_item_list_set_enter_callback(
        var_item_list, moon_app_scene_interface_general_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(app->scene_manager, MoonAppSceneInterfaceGeneral));

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewVarItemList);
}

bool moon_app_scene_interface_general_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            app->scene_manager, MoonAppSceneInterfaceGeneral, event.event);
        consumed = true;
    }

    return consumed;
}

void moon_app_scene_interface_general_on_exit(void* context) {
    MoonApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
