#include "../bt_settings_app.h"
#include <furi_hal_bt.h>
#include <power/power_service/power.h>
#include <moon/settings.h>

enum BtSetting {
    BtSettingOff,
    BtSettingOn,
    BtSettingNum,
};

enum BtSettingIndex {
    BtSettingIndexSwitchBt,
    BtSettingIndexMaxConnections,
    BtSettingIndexUtcOffset,
    BtSettingIndexForgetDev,
};

const char* const bt_settings_text[BtSettingNum] = {
    "OFF",
    "ON",
};

static void bt_settings_scene_start_var_list_change_callback(VariableItem* item) {
    BtSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, bt_settings_text[index]);
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static uint32_t original_max_connections = 0;

static void bt_settings_scene_start_max_connections_changed(VariableItem* item) {
    UNUSED(item);
    uint8_t value = variable_item_get_current_value_index(item) + 2; // 2-8
    char str[4];
    snprintf(str, sizeof(str), "%d", value);
    variable_item_set_current_value_text(item, str);
    moon_settings.ble_max_connections = value;
}

static void bt_settings_scene_start_utc_offset_changed(VariableItem* item) {
    int32_t value = (int32_t)variable_item_get_current_value_index(item) - 12; // -12 to +14
    char str[8];
    snprintf(str, sizeof(str), "UTC%+ld", (long)value);
    variable_item_set_current_value_text(item, str);
    moon_settings.utc_offset_hours = value;
}

static void bt_settings_scene_start_var_list_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    BtSettingsApp* app = context;
    if(index == BtSettingIndexForgetDev) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, BtSettingsCustomEventForgetDevices);
    }
}

void bt_settings_scene_start_on_enter(void* context) {
    BtSettingsApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    // Remember original value to detect changes on exit
    original_max_connections = moon_settings.ble_max_connections;

    if(furi_hal_bt_is_gatt_gap_supported()) {
        item = variable_item_list_add(
            var_item_list,
            "Bluetooth",
            BtSettingNum,
            bt_settings_scene_start_var_list_change_callback,
            app);
        if(app->settings.enabled) {
            variable_item_set_current_value_index(item, BtSettingOn);
            variable_item_set_current_value_text(item, bt_settings_text[BtSettingOn]);
        } else {
            variable_item_set_current_value_index(item, BtSettingOff);
            variable_item_set_current_value_text(item, bt_settings_text[BtSettingOff]);
        }

        // Max BLE Connections (2-8, requires reboot)
        item = variable_item_list_add(
            var_item_list,
            "Max Connections",
            7, // 7 options: 2,3,4,5,6,7,8
            bt_settings_scene_start_max_connections_changed,
            app);
        uint32_t conn = moon_settings.ble_max_connections;
        if(conn < 2) conn = 2;
        if(conn > 8) conn = 8;
        variable_item_set_current_value_index(item, conn - 2);
        char conn_str[4];
        snprintf(conn_str, sizeof(conn_str), "%lu", conn);
        variable_item_set_current_value_text(item, conn_str);

        // UTC Offset (-12 to +14, 27 options)
        item = variable_item_list_add(
            var_item_list,
            "UTC Offset",
            27, // -12 to +14 = 27 values
            bt_settings_scene_start_utc_offset_changed,
            app);
        int32_t utc_off = moon_settings.utc_offset_hours;
        if(utc_off < -12) utc_off = -12;
        if(utc_off > 14) utc_off = 14;
        variable_item_set_current_value_index(item, utc_off + 12);
        char utc_str[8];
        snprintf(utc_str, sizeof(utc_str), "UTC%+ld", (long)utc_off);
        variable_item_set_current_value_text(item, utc_str);

        variable_item_list_add(var_item_list, "Unpair All Devices", 1, NULL, NULL);
        variable_item_list_set_enter_callback(
            var_item_list, bt_settings_scene_start_var_list_enter_callback, app);
    } else {
        item = variable_item_list_add(var_item_list, "Bluetooth", 1, NULL, NULL);
        variable_item_set_current_value_text(item, "Broken");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BtSettingsAppViewVarItemList);
}

bool bt_settings_scene_start_on_event(void* context, SceneManagerEvent event) {
    BtSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BtSettingOn) {
            app->settings.enabled = true;
            consumed = true;
        } else if(event.event == BtSettingOff) {
            app->settings.enabled = false;
            consumed = true;
        } else if(event.event == BtSettingsCustomEventForgetDevices) {
            scene_manager_next_scene(app->scene_manager, BtSettingsAppSceneForgetDevConfirm);
            consumed = true;
        }
    }

    return consumed;
}

void bt_settings_scene_start_on_exit(void* context) {
    BtSettingsApp* app = context;
    variable_item_list_reset(app->var_item_list);

    // Always save settings (UTC offset may have changed)
    moon_settings_save();

    // If max connections changed, reboot required
    if(moon_settings.ble_max_connections != original_max_connections) {
        Power* power = furi_record_open(RECORD_POWER);
        power_reboot(power, PowerBootModeNormal);
    }
}
