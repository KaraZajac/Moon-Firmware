#include "../moon_app.h"

enum VarItemListIndex {
    VarItemListIndexInterface,
    VarItemListIndexProtocols,
    VarItemListIndexMisc,
};

void moon_app_scene_start_var_item_list_callback(void* context, uint32_t index) {
    MoonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void moon_app_scene_start_on_enter(void* context) {
    MoonApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(var_item_list, "Interface", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Protocols", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Misc", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_set_header(var_item_list, furi_string_get_cstr(app->version_tag));

    variable_item_list_set_enter_callback(
        var_item_list, moon_app_scene_start_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(app->scene_manager, MoonAppSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewVarItemList);
}

bool moon_app_scene_start_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MoonAppSceneStart, event.event);
        consumed = true;
        switch(event.event) {
        case VarItemListIndexInterface:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneInterface, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterface);
            break;
        case VarItemListIndexProtocols:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneProtocols, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneProtocols);
            break;
        case VarItemListIndexMisc:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMisc, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneMisc);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void moon_app_scene_start_on_exit(void* context) {
    MoonApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
