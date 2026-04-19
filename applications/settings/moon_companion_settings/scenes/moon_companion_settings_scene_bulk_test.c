#include "../moon_companion_settings_app.h"

/* Bring-up fixture for Phase 3. Kicks off a BULK_ECHO_TEST transfer
 * against the paired phone and displays whether the 10 KB deterministic
 * payload arrived with the expected byte pattern (byte[i] = i & 0xFF).
 *
 * The transfer runs on a worker thread because moon_companion_bulk_open_blocking
 * is synchronous and we don't want to freeze the GUI. Result is posted
 * back to the scene via a custom event. */

#define BULK_TEST_TIMEOUT_MS 30000

typedef struct {
    MoonCompSettingsApp* app;
    /* Next expected byte for pattern verification. bytes_received would
     * work too but keeping an explicit cursor makes the expected/got
     * mismatch printable if it ever shows up. */
    uint32_t cursor;
} BulkTestCtx;

static void on_chunk(const uint8_t* data, size_t len, void* context) {
    BulkTestCtx* ctx = context;
    MoonCompSettingsApp* app = ctx->app;
    for(size_t i = 0; i < len; i++) {
        uint8_t expected = (uint8_t)((ctx->cursor + i) & 0xFF);
        if(data[i] != expected) {
            app->bulk_test_pattern_ok = false;
            /* Keep going — byte count still tells us how much got through. */
        }
    }
    ctx->cursor += (uint32_t)len;
}

static int32_t bulk_test_worker(void* p) {
    MoonCompSettingsApp* app = p;
    BulkTestCtx ctx = {.app = app, .cursor = 0};

    app->bulk_test_pattern_ok = true;
    app->bulk_test_success = moon_companion_bulk_open_blocking(
        app->moon,
        MoonBulkKindEchoTest,
        NULL,
        BULK_TEST_TIMEOUT_MS,
        on_chunk,
        &ctx,
        &app->bulk_test_bytes,
        &app->bulk_test_error);

    view_dispatcher_send_custom_event(
        app->view_dispatcher, MoonCompSettingsEventBulkTestDone);
    return 0;
}

static void render_result(MoonCompSettingsApp* app) {
    char body[96];
    if(app->bulk_test_success && app->bulk_test_pattern_ok) {
        snprintf(body, sizeof(body), "OK\n%lu bytes received",
                 (unsigned long)app->bulk_test_bytes);
    } else if(app->bulk_test_success && !app->bulk_test_pattern_ok) {
        snprintf(body, sizeof(body),
                 "Pattern mismatch\n%lu bytes (bad contents)",
                 (unsigned long)app->bulk_test_bytes);
    } else {
        snprintf(body, sizeof(body), "Failed err=0x%04x\n%lu bytes received",
                 app->bulk_test_error, (unsigned long)app->bulk_test_bytes);
    }
    popup_set_header(app->popup, "Bulk echo test", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, body, 64, 36, AlignCenter, AlignCenter);
}

void moon_companion_settings_scene_bulk_test_on_enter(void* context) {
    MoonCompSettingsApp* app = context;
    popup_reset(app->popup);
    popup_set_header(app->popup, "Bulk echo test", 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Running...\nConnecting CoC", 64, 36,
                   AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, MoonCompSettingsViewPopup);

    app->bulk_test_bytes = 0;
    app->bulk_test_error = 0;
    app->bulk_test_success = false;
    app->bulk_test_pattern_ok = true;

    /* Spawn the worker so the GUI thread stays responsive. Stack is
     * generous because bulk_open_blocking allocates its session on the
     * heap but still touches a fair amount of ST stack plumbing below
     * it. */
    app->bulk_test_thread =
        furi_thread_alloc_ex("MoonBulkTest", 2048, bulk_test_worker, app);
    furi_thread_start(app->bulk_test_thread);
}

bool moon_companion_settings_scene_bulk_test_on_event(
    void* context,
    SceneManagerEvent event) {
    MoonCompSettingsApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == MoonCompSettingsEventBulkTestDone) {
        render_result(app);
        return true;
    }
    return false;
}

void moon_companion_settings_scene_bulk_test_on_exit(void* context) {
    MoonCompSettingsApp* app = context;
    if(app->bulk_test_thread) {
        furi_thread_join(app->bulk_test_thread);
        furi_thread_free(app->bulk_test_thread);
        app->bulk_test_thread = NULL;
    }
    popup_reset(app->popup);
}
