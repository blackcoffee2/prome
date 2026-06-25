/*
 * App descriptor and self-registration.
 *
 * An app is one translation unit that defines a static app_descriptor_t and
 * registers it with REGISTER_APP. The descriptor names the app, points at its
 * launcher icon, and supplies its run() and optional cleanup() entry points.
 *
 * Self-registration via a linker section is what makes apps additive: dropping
 * a new app component into the build places its descriptor pointer into the
 * .app_registry section, and the launcher enumerates that section at startup.
 * Adding or removing an app is a build-list change with no edit to the shell,
 * the launcher, or any central table.
 *
 * An app includes this single header to gain the descriptor type, the
 * registration macro, the curated API type, and LVGL (for the widget types its
 * run() will use). It does not include shell internals.
 */
#pragma once

#include "app_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One app's registration record.
 *
 *   name    Display name, shown under the launcher icon and used as the app's
 *           storage scope. Kept short.
 *   icon    Launcher tile image, or NULL to fall back to a generated initial.
 *   run     Called once when the app launches: build widgets on root and
 *           register any timers, then return. The app has no loop of its own.
 *   cleanup Called once on exit, before the shell destroys the app screen.
 *           Release anything the shell does not own — timers, app-owned
 *           buffers, running viewfinders. May be NULL when there is nothing to
 *           release beyond what the screen teardown frees.
 */
typedef struct {
    const char *name;
    const lv_image_dsc_t *icon;
    void (*run)(const app_api_t *api, void *ctx);
    void (*cleanup)(const app_api_t *api, void *ctx);
} app_descriptor_t;

/*
 * Place a pointer to a static app_descriptor_t into the .app_registry section
 * so the launcher discovers it at startup. The pointer is marked used so the
 * linker does not discard it, and the section attribute is what gathers all
 * apps into one contiguous, enumerable array.
 *
 * The component's CMakeLists.txt must keep this object's section from being
 * garbage-collected (WHOLE_ARCHIVE on the component), since nothing references
 * the descriptor by name; see the component build files.
 */
#define REGISTER_APP(descriptor)                                              \
    static const app_descriptor_t *const _app_reg_##descriptor                \
        __attribute__((used, aligned(4), section("app_registry"))) = &descriptor

#ifdef __cplusplus
}
#endif