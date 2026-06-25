/*
 * Curated app API — the sandbox surface.
 *
 * Apps never touch LVGL, the filesystem, or hardware directly. The shell hands
 * each app this struct of function pointers plus an opaque per-app context.
 * The curated surface IS the sandbox: an app can reach exactly what the API
 * exposes and nothing more. The shell can restyle, re-layout, or re-back any
 * widget or service without touching app code, so long as this contract holds.
 *
 * Per-app state travels through the void *ctx argument the shell passes to
 * run() and cleanup(); the API functions take that ctx so the shell can scope
 * each call to the calling app (storage folders, the app's root, and so on).
 *
 * The surface starts small and grows from real app demand, not speculation.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration; the concrete struct is defined below. */
typedef struct app_api app_api_t;

/*
 * WiFi connectivity state reported to apps. A coarse, hardware-agnostic enum;
 * the underlying capability fills it in. This is the seam to the capability
 * layer — apps read status, they do not drive the radio.
 */
typedef enum {
    APP_WIFI_DISCONNECTED = 0,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_UNAVAILABLE,
} app_wifi_status_t;

struct app_api {
    /* --- Root --- */
    /* The container the app builds into. The shell creates it before run() and
     * destroys it on exit, freeing every widget the app parented to it. */
    lv_obj_t *(*root)(void *ctx);

    /* --- Widgets --- */
    /* Each returns the created widget, parented under root by default. Callers
     * may re-parent within their own tree but never outside root. */
    lv_obj_t *(*label)(void *ctx, lv_obj_t *parent, const char *text);
    lv_obj_t *(*button)(void *ctx, lv_obj_t *parent, const char *text,
                        void (*on_tap)(void *ctx), void *user);
    lv_obj_t *(*list)(void *ctx, lv_obj_t *parent);
    lv_obj_t *(*list_add)(void *ctx, lv_obj_t *list, const char *text,
                          void (*on_tap)(void *ctx, const char *text), void *user);
    lv_obj_t *(*image)(void *ctx, lv_obj_t *parent, const lv_image_dsc_t *src);

    /* --- Services --- */
    /* Storage scoped to the app's own folder under the storage root; an app
     * cannot name a path outside its subdirectory. Returns bytes read/written,
     * or negative on error. */
    int (*storage_read)(void *ctx, const char *path, void *buf, size_t len);
    int (*storage_write)(void *ctx, const char *path, const void *buf, size_t len);
    /* WiFi status: the seam to the capability layer. */
    app_wifi_status_t (*wifi_status)(void *ctx);

    /* Camera: coarse verbs backed by the camera capability. The app supplies a
     * canvas widget; the capability runs the whole viewfinder pipeline in C and
     * blits frames onto it. The app starts and stops the viewfinder and never
     * touches a pixel. Returns false if the camera capability is unavailable on
     * this device, so the app can hide or disable its camera UI. */
    bool (*camera_available)(void *ctx);
    bool (*camera_viewfinder_start)(void *ctx, lv_obj_t *canvas);
    void (*camera_viewfinder_stop)(void *ctx);

    /* Brightness: coarse verbs backed by the brightness capability, which
     * drives the panel backlight over LEDC PWM. The app sets a percentage and
     * may read back the applied value; it never touches the timer or duty.
     * brightness_available returns false when the capability is absent or the
     * board's backlight cannot be dimmed (hardwired on), so the app can hide or
     * disable its brightness control. brightness_set returns false if the set
     * did not take effect; brightness_get returns the last applied percentage
     * (0..100), or 100 when the service is unavailable. */
    bool (*brightness_available)(void *ctx);
    bool (*brightness_set)(void *ctx, uint8_t percent);
    uint8_t (*brightness_get)(void *ctx);

    /* --- System --- */
    /* Set the two softkey labels shown on the action band. Either may be NULL
     * to clear that slot. */
    void (*set_softkeys)(void *ctx, const char *left, const char *right);
    /* Exit to the launcher. The shell runs cleanup(), destroys root, and
     * reloads the launcher screen. */
    void (*back)(void *ctx);
};

#ifdef __cplusplus
}
#endif