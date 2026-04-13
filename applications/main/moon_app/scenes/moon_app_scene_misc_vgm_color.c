#include "../moon_app.h"

enum ByteInputResult {
    ByteInputResultOk,
};

void moon_app_scene_misc_vgm_color_byte_input_callback(void* context) {
    MoonApp* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, ByteInputResultOk);
}

void moon_app_scene_misc_vgm_color_on_enter(void* context) {
    MoonApp* app = context;
    ByteInput* byte_input = app->byte_input;

    byte_input_set_header_text(byte_input, "Set VGM Color (#RRGGBB)");

    if(scene_manager_get_scene_state(app->scene_manager, MoonAppSceneMiscVgmColor)) {
        app->vgm_color = moon_settings.rpc_color_bg.rgb;
    } else {
        app->vgm_color = moon_settings.rpc_color_fg.rgb;
    }

    byte_input_set_result_callback(
        byte_input,
        moon_app_scene_misc_vgm_color_byte_input_callback,
        NULL,
        app,
        (void*)&app->vgm_color,
        sizeof(app->vgm_color));

    view_dispatcher_switch_to_view(app->view_dispatcher, MoonAppViewByteInput);
}

bool moon_app_scene_misc_vgm_color_on_event(void* context, SceneManagerEvent event) {
    MoonApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case ByteInputResultOk:
            if(scene_manager_get_scene_state(app->scene_manager, MoonAppSceneMiscVgmColor)) {
                moon_settings.rpc_color_bg.rgb = app->vgm_color;
            } else {
                moon_settings.rpc_color_fg.rgb = app->vgm_color;
            }
            app->save_settings = true;
            scene_manager_previous_scene(app->scene_manager);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void moon_app_scene_misc_vgm_color_on_exit(void* context) {
    MoonApp* app = context;
    byte_input_set_result_callback(app->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(app->byte_input, "");
}
