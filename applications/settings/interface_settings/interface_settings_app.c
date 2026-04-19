#include <furi.h>
#include <loader/loader.h>

/* Thin shim: the "Interface" entry in the system Settings menu launches
 * the main moon_app with a command-line arg that tells it to skip its
 * own Start scene and jump straight into the Interface sub-menu. The
 * real scenes still live under applications/main/moon_app/scenes/; this
 * keeps the Settings menu presentation separate from the underlying
 * code without duplicating the 500-line moon_app runtime. */
int32_t interface_settings_app(void* p) {
    UNUSED(p);
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, "Moon", "Interface");
    furi_record_close(RECORD_LOADER);
    return 0;
}
