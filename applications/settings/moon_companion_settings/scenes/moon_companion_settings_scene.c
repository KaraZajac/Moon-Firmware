#include "moon_companion_settings_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const moon_companion_settings_on_enter_handlers[])(void*) = {
#include "moon_companion_settings_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const moon_companion_settings_on_event_handlers[])(void*, SceneManagerEvent) = {
#include "moon_companion_settings_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const moon_companion_settings_on_exit_handlers[])(void*) = {
#include "moon_companion_settings_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers moon_companion_settings_scene_handlers = {
    .on_enter_handlers = moon_companion_settings_on_enter_handlers,
    .on_event_handlers = moon_companion_settings_on_event_handlers,
    .on_exit_handlers = moon_companion_settings_on_exit_handlers,
    .scene_num = MoonCompSettingsSceneNum,
};
