#include "../moon_app.h"

enum VarItemListIndex {
    VarItemListIndexGraphics,
    VarItemListIndexMainmenu,
    VarItemListIndexLockscreen,
    VarItemListIndexStatusbar,
    VarItemListIndexFileBrowser,
    VarItemListIndexGeneral,
};

void moon_app_scene_interface_var_item_list_callback(void* context, uint32_t index) {
    MoonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void moon_app_scene_interface_on_enter(void* context) {
    MoonApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(var_item_list, "Graphics", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Mainmenu", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Lockscreen", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Statusbar", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "File Browser", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "General", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_set_enter_callback(
        var_item_list, moon_app_scene_interface_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(app->scene_manager, MoonAppSceneInterface));

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewVarItemList);
}

bool moon_app_scene_interface_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MoonAppSceneInterface, event.event);
        consumed = true;
        switch(event.event) {
        case VarItemListIndexGraphics:
            scene_manager_set_scene_state(
                app->scene_manager, MoonAppSceneInterfaceGraphics, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceGraphics);
            break;
        case VarItemListIndexMainmenu:
            scene_manager_set_scene_state(
                app->scene_manager, MoonAppSceneInterfaceMainmenu, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceMainmenu);
            break;
        case VarItemListIndexLockscreen:
            scene_manager_set_scene_state(
                app->scene_manager, MoonAppSceneInterfaceLockscreen, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceLockscreen);
            break;
        case VarItemListIndexStatusbar:
            scene_manager_set_scene_state(
                app->scene_manager, MoonAppSceneInterfaceStatusbar, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceStatusbar);
            break;
        case VarItemListIndexFileBrowser:
            scene_manager_set_scene_state(
                app->scene_manager, MoonAppSceneInterfaceFilebrowser, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceFilebrowser);
            break;
        case VarItemListIndexGeneral:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneInterfaceGeneral, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneInterfaceGeneral);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void moon_app_scene_interface_on_exit(void* context) {
    MoonApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
