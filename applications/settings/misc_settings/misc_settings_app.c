#include <furi.h>
#include <loader/loader.h>

/* Thin shim: see interface_settings_app.c for rationale. Launches the
 * main moon_app into its Misc sub-menu (screen/backlight, Dolphin
 * stats, name spoof, VGM colour, etc.) so those knobs appear as a
 * first-class entry in the Settings menu. */
int32_t misc_settings_app(void* p) {
    UNUSED(p);
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, "Moon", "Misc");
    furi_record_close(RECORD_LOADER);
    return 0;
}
