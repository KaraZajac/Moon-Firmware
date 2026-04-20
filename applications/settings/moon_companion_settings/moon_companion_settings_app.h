#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>

#include <moon_companion/moon_companion.h>

#include "scenes/moon_companion_settings_scene.h"

typedef enum {
    MoonCompSettingsViewList,
    MoonCompSettingsViewPopup,
} MoonCompSettingsView;

typedef enum {
    /* First 10 reserved for list indices. */
    MoonCompSettingsEventReserved = 10,
    MoonCompSettingsEventPair,
    MoonCompSettingsEventForget,
    MoonCompSettingsEventPairDone,
} MoonCompSettingsCustomEvent;

typedef struct {
    Gui* gui;
    MoonCompanion* moon;

    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    VariableItemList* var_item_list;
    Popup* popup;

    char pin[7];
    FuriTimer* poll_timer; /* polls moon_companion_get_state on the pair popup */
} MoonCompSettingsApp;
