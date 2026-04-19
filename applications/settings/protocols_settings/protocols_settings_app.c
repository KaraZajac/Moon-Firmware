#include <furi.h>
#include <loader/loader.h>

/* Thin shim: see interface_settings_app.c for rationale. Launches the
 * main moon_app into its Protocols sub-menu (SubGHz frequency lists,
 * GPIO, etc.) via the Loader so the Settings-side entry looks like a
 * first-class setting to the user. */
int32_t protocols_settings_app(void* p) {
    UNUSED(p);
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, "Moon", "Protocols");
    furi_record_close(RECORD_LOADER);
    return 0;
}
