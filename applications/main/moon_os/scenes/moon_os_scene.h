#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) MoonOsScene##id,
typedef enum {
#include "moon_os_scene_config.h"
    MoonOsSceneNum,
} MoonOsScene;
#undef ADD_SCENE

extern const SceneManagerHandlers moon_os_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "moon_os_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "moon_os_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "moon_os_scene_config.h"
#undef ADD_SCENE
