#include "../moon_app.h"

enum VarItemListIndex {
    VarItemListIndexScreen,
    VarItemListIndexDolphin,
    VarItemListIndexSpoof,
    VarItemListIndexVgm,
    VarItemListIndexShowMoonIntro,
};

void moon_app_scene_misc_var_item_list_callback(void* context, uint32_t index) {
    MoonApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void moon_app_scene_misc_on_enter(void* context) {
    MoonApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(var_item_list, "Screen", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Dolphin", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Spoofing Options", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "VGM Options", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_add(var_item_list, "Show Moon Intro", 0, NULL, app);

    variable_item_list_set_enter_callback(
        var_item_list, moon_app_scene_misc_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(app->scene_manager, MoonAppSceneMisc));

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewVarItemList);
}

bool moon_app_scene_misc_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMisc, event.event);
        consumed = true;
        switch(event.event) {
        case VarItemListIndexScreen:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMiscScreen, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneMiscScreen);
            break;
        case VarItemListIndexDolphin:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMiscDolphin, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneMiscDolphin);
            break;
        case VarItemListIndexSpoof:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMiscSpoof, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneMiscSpoof);
            break;
        case VarItemListIndexVgm:
            scene_manager_set_scene_state(app->scene_manager, MoonAppSceneMiscVgm, 0);
            scene_manager_next_scene(app->scene_manager, MoonAppSceneMiscVgm);
            break;
        case VarItemListIndexShowMoonIntro: {
            for(int i = 0; i < 10; i++) {
                if(storage_common_copy(
                       app->storage, EXT_PATH("dolphin/firstboot.bin"), SLIDESHOW_FS_PATH)) {
                    app->show_slideshow = true;
                    moon_app_apply(app);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    return consumed;
}

void moon_app_scene_misc_on_exit(void* context) {
    MoonApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
