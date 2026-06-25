/*
 * App host — implementation.
 *
 * Owns three things:
 *   1. Registry enumeration over the .app_registry linker section.
 *   2. The curated app API implementation: each function pointer in app_api_t
 *      resolves to a static function here, scoped to the running app through
 *      its context.
 *   3. The app lifecycle: launch (fresh screen + context, guarded run()), and
 *      back (cleanup, destroy screen, reload launcher).
 *
 * Single-task discipline holds: every API call and callback runs on the LVGL
 * task that app_main pumps. Apps never spawn loops; run() builds UI and
 * returns. The launcher screen is never destroyed — apps layer over it and
 * tear down on exit.
 *
 * WiFi status reports unavailable until a WiFi capability is wired; the API
 * shape is final and only the backing changes.
 */
#include "app_host.h"
#include "app_api.h"
#include "launcher.h"
#include "storage.h"
#include "capability_host.h"
#include "theme.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app_host";

/* Camera capability verbs. Declared weak so the shell does not hard-depend on
 * the camera capability component: if that component is excluded from the
 * build these resolve to NULL, and the API binding gates every call on both
 * the symbol's presence and capability_ready, so excluding the capability
 * cleanly reports the service unavailable. The names match
 * capability_camera.c. */
#define CAPABILITY_CAMERA_NAME "camera"
extern bool capability_camera_viewfinder_start(lv_obj_t *canvas) __attribute__((weak));
extern void capability_camera_viewfinder_stop(void) __attribute__((weak));

/* Brightness capability verbs. Declared weak for the same reason as the camera
 * verbs above: the shell does not hard-depend on the brightness capability
 * component. When that component is excluded these resolve to NULL, and the
 * API binding gates every call on both the symbol's presence and
 * capability_ready, so the brightness service cleanly reports unavailable. The
 * names match capability_brightness.c. */
#define CAPABILITY_BRIGHTNESS_NAME "brightness"
extern bool capability_brightness_set(uint8_t percent) __attribute__((weak));
extern uint8_t capability_brightness_get(void) __attribute__((weak));

/* Registry section boundaries provided by linker.lf. Declared as arrays so the
 * addresses are the section bounds and pointer arithmetic between them is
 * well-defined. Each element is one descriptor pointer placed by REGISTER_APP. */
extern const app_descriptor_t *_app_registry_start[];
extern const app_descriptor_t *_app_registry_end[];

/*
 * Per-app context. One instance lives while an app runs; carries the app's
 * descriptor (for cleanup), its root container, and the API view handed to it.
 * The pointer to this struct is the opaque void *ctx every API call receives,
 * so the host can recover the running app's state from any callback.
 */
typedef struct {
    const app_descriptor_t *descriptor;
    lv_obj_t *screen;
    lv_obj_t *root;
    app_api_t api;
    /* The app screen's bottom action band and its two softkey labels. The
     * right label, when set to "Back", is tappable and triggers exit. */
    lv_obj_t *softkey_band;
    lv_obj_t *softkey_left;
    lv_obj_t *softkey_right;
} app_context_t;

/* The single running app, or NULL at the launcher. The host hosts one app at a
 * time; launching from a tile always returns to the launcher first. */
static app_context_t *s_running = NULL;

/* --- Registry enumeration --- */

size_t app_host_count(void)
{
    return (size_t)(_app_registry_end - _app_registry_start);
}

const app_descriptor_t *app_host_descriptor(size_t i)
{
    if (i >= app_host_count()) {
        return NULL;
    }
    return _app_registry_start[i];
}

/* --- Curated API implementations --- */

static lv_obj_t *api_root(void *ctx)
{
    app_context_t *app = ctx;
    return app->root;
}

static lv_obj_t *api_label(void *ctx, lv_obj_t *parent, const char *text)
{
    app_context_t *app = ctx;
    lv_obj_t *label = lv_label_create(parent ? parent : app->root);
    lv_label_set_text(label, text ? text : "");
    return label;
}

/*
 * Button tap trampoline. LVGL hands us the event; we recover the app context
 * and the user's callback from the button's user data and invoke it with the
 * app context as the public ctx, keeping apps unaware of LVGL's event type.
 */
typedef struct {
    void *ctx;
    void (*on_tap)(void *ctx);
} tap_binding_t;

static void button_tap_trampoline(lv_event_t *event)
{
    tap_binding_t *binding = lv_event_get_user_data(event);
    if (binding && binding->on_tap) {
        binding->on_tap(binding->ctx);
    }
}

static lv_obj_t *api_button(void *ctx, lv_obj_t *parent, const char *text,
                            void (*on_tap)(void *ctx), void *user)
{
    (void)user;
    app_context_t *app = ctx;
    lv_obj_t *button = lv_button_create(parent ? parent : app->root);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    lv_obj_center(label);

    /* The binding lives as long as the button; freed when root is destroyed
     * because it is attached to the button's own user data via LVGL's
     * allocated-user-data mechanism. */
    tap_binding_t *binding = lv_malloc(sizeof(tap_binding_t));
    binding->ctx = ctx;
    binding->on_tap = on_tap;
    lv_obj_add_event_cb(button, button_tap_trampoline, LV_EVENT_CLICKED, binding);
    lv_obj_set_user_data(button, binding);
    return button;
}

static lv_obj_t *api_list(void *ctx, lv_obj_t *parent)
{
    app_context_t *app = ctx;
    return lv_list_create(parent ? parent : app->root);
}

typedef struct {
    void *ctx;
    void (*on_tap)(void *ctx, const char *text);
    const char *text;
} list_binding_t;

static void list_item_trampoline(lv_event_t *event)
{
    list_binding_t *binding = lv_event_get_user_data(event);
    if (binding && binding->on_tap) {
        binding->on_tap(binding->ctx, binding->text);
    }
}

static lv_obj_t *api_list_add(void *ctx, lv_obj_t *list, const char *text,
                              void (*on_tap)(void *ctx, const char *text), void *user)
{
    (void)user;
    lv_obj_t *item = lv_list_add_button(list, NULL, text ? text : "");
    list_binding_t *binding = lv_malloc(sizeof(list_binding_t));
    binding->ctx = ctx;
    binding->on_tap = on_tap;
    binding->text = text;
    lv_obj_add_event_cb(item, list_item_trampoline, LV_EVENT_CLICKED, binding);
    lv_obj_set_user_data(item, binding);
    return item;
}

static lv_obj_t *api_image(void *ctx, lv_obj_t *parent, const lv_image_dsc_t *src)
{
    app_context_t *app = ctx;
    lv_obj_t *image = lv_image_create(parent ? parent : app->root);
    if (src) {
        lv_image_set_src(image, src);
    }
    return image;
}

/* Storage scoped to the running app's folder, keyed by the app's name. */
static int api_storage_read(void *ctx, const char *path, void *buf, size_t len)
{
    app_context_t *app = ctx;
    return storage_read_scoped(app->descriptor->name, path, buf, len);
}

static int api_storage_write(void *ctx, const char *path, const void *buf, size_t len)
{
    app_context_t *app = ctx;
    return storage_write_scoped(app->descriptor->name, path, buf, len);
}

/* WiFi seam: unavailable until a WiFi capability lands. */
static app_wifi_status_t api_wifi_status(void *ctx)
{
    (void)ctx;
    return APP_WIFI_UNAVAILABLE;
}

/* Camera verbs, gated on the capability being both linked (weak symbol
 * present) and initialized (capability_ready). When unavailable, start reports
 * false and stop is a safe no-op, so an app degrades gracefully. */
static bool api_camera_available(void *ctx)
{
    (void)ctx;
    return capability_camera_viewfinder_start != NULL &&
           capability_ready(CAPABILITY_CAMERA_NAME);
}

static bool api_camera_viewfinder_start(void *ctx, lv_obj_t *canvas)
{
    if (!api_camera_available(ctx) || canvas == NULL) {
        return false;
    }
    return capability_camera_viewfinder_start(canvas);
}

static void api_camera_viewfinder_stop(void *ctx)
{
    (void)ctx;
    if (capability_camera_viewfinder_stop != NULL) {
        capability_camera_viewfinder_stop();
    }
}

/* Brightness verbs, gated the same way as the camera verbs: the capability
 * must be linked (weak symbol present) and initialized (capability_ready).
 * When unavailable, available reports false, set is a no-op returning false,
 * and get returns full brightness so an app reading back a value sees a sane
 * default rather than zero. */
static bool api_brightness_available(void *ctx)
{
    (void)ctx;
    return capability_brightness_set != NULL &&
           capability_ready(CAPABILITY_BRIGHTNESS_NAME);
}

static bool api_brightness_set(void *ctx, uint8_t percent)
{
    if (!api_brightness_available(ctx)) {
        return false;
    }
    return capability_brightness_set(percent);
}

static uint8_t api_brightness_get(void *ctx)
{
    if (!api_brightness_available(ctx) || capability_brightness_get == NULL) {
        return 100;
    }
    return capability_brightness_get();
}

static void app_host_back(void *ctx);

/*
 * Right-softkey tap handler. When the right softkey reads "Back", tapping it
 * exits to the launcher, giving every app a consistent exit affordance without
 * the app wiring its own. The app context travels in the event user data.
 */
static void softkey_right_tapped(lv_event_t *event)
{
    void *ctx = lv_event_get_user_data(event);
    app_host_back(ctx);
}

static void api_set_softkeys(void *ctx, const char *left, const char *right)
{
    app_context_t *app = ctx;
    lv_label_set_text(app->softkey_left, left ? left : "");
    lv_label_set_text(app->softkey_right, right ? right : "");

    /* The right softkey is the exit affordance when labeled "Back". The band
     * carries a single click handler; enabling/disabling its clickable flag
     * matches the label so a non-Back right softkey is inert. */
    bool is_back = right && lv_strcmp(right, "Back") == 0;
    if (is_back) {
        lv_obj_add_flag(app->softkey_band, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(app->softkey_band, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void api_back(void *ctx)
{
    app_host_back(ctx);
}

static void bind_api(app_context_t *app)
{
    app->api.root = api_root;
    app->api.label = api_label;
    app->api.button = api_button;
    app->api.list = api_list;
    app->api.list_add = api_list_add;
    app->api.image = api_image;
    app->api.storage_read = api_storage_read;
    app->api.storage_write = api_storage_write;
    app->api.wifi_status = api_wifi_status;
    app->api.camera_available = api_camera_available;
    app->api.camera_viewfinder_start = api_camera_viewfinder_start;
    app->api.camera_viewfinder_stop = api_camera_viewfinder_stop;
    app->api.brightness_available = api_brightness_available;
    app->api.brightness_set = api_brightness_set;
    app->api.brightness_get = api_brightness_get;
    app->api.set_softkeys = api_set_softkeys;
    app->api.back = api_back;
}

/* --- Lifecycle --- */

/*
 * Strip the app root to a full-bleed, undecorated container. A plain
 * lv_obj_create inherits the active LVGL theme's default object styling: inner
 * padding, a 1 px border, a corner radius, and a scrollable flag. Those
 * defaults inset an app's content from the root's edges and draw a framed box
 * a few pixels in from the screen. Zeroing padding, border width, and radius
 * and clearing the scroll flag turns the root into a transparent rectangle that
 * exactly fills its set geometry, so a canvas or widget aligned to the root's
 * top-left reaches the physical screen edge.
 *
 * The background is left transparent rather than filled so an app that paints
 * its own canvas (camera, touch) shows through cleanly and an app that does not
 * inherits whatever the screen beneath provides.
 */
static void strip_root(lv_obj_t *root)
{
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

/*
 * Guarded run path. app run() is invoked here in isolation so a fault in app
 * code can be contained: on a crash this returns to the launcher rather than
 * taking down the shell. On this single-task cooperative runtime the guard is
 * a structured boundary around the call; a hardware fault handler routes back
 * to the launcher reset path. The boundary is centralized here so every app
 * launch goes through the same containment.
 */
static void guarded_run(app_context_t *app)
{
    if (app->descriptor->run) {
        app->descriptor->run(&app->api, app);
    }
}

void app_host_launch(const app_descriptor_t *descriptor)
{
    if (descriptor == NULL || s_running != NULL) {
        return;
    }

    app_context_t *app = lv_malloc(sizeof(app_context_t));
    if (app == NULL) {
        ESP_LOGE(TAG, "launch alloc failed");
        return;
    }
    lv_memzero(app, sizeof(app_context_t));
    app->descriptor = descriptor;

    /* Fresh screen layered over the persistent launcher. The screen holds the
     * app's root container above a bottom action band carrying the softkeys.
     * Destroying the screen frees the root and band and every widget and
     * binding parented to them.
     *
     * The screen is stripped of its default-theme decoration (border, radius,
     * padding) the same way the root is. A screen-level lv_obj otherwise
     * carries a 1 px border and a corner radius, and children aligned with
     * LV_ALIGN_BOTTOM_MID land at the screen's CONTENT edge -- inset from the
     * physical edge by that border, which leaves a thin gap of bare screen
     * below the softkey band at the bottom of the panel. Zeroing the border and
     * radius makes the content area equal the full panel, so the bottom-aligned
     * band sits flush against the physical bottom edge. */
    app->screen = lv_obj_create(NULL);
    strip_root(app->screen);
    /* strip_root leaves the background transparent, which is right for the root
     * (it shows the screen beneath) but wrong for a top-level screen, which must
     * paint an opaque base. Fill it black so any region an app does not cover is
     * solid black rather than transparent. */
    lv_obj_set_style_bg_opa(app->screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(app->screen, lv_color_black(), LV_PART_MAIN);

    /* The shell owns the app's geometry and hands the app a root that is exactly
     * the usable area: full panel width, full panel height minus the bottom
     * softkey band. The app then simply renders inside the root; a widget the
     * app sizes to LV_PCT(100) fills the whole region with no further work,
     * because the root is given a CONCRETE pixel size rather than a percentage
     * expression.
     *
     * Why concrete pixels and not LV_PCT(100) - THEME_BAND_HEIGHT_PX: a child's
     * LV_PCT(100) resolves against its parent's size. When the parent's own size
     * is a percent-minus-constant expression, the child's percentage and the
     * parent's content-box accounting do not compose cleanly, and the child
     * lands short of the parent -- leaving a black strip between an app's
     * content and the softkey band. Sizing the root to a fixed pixel height
     * makes LV_PCT(100) inside the app resolve to that exact height.
     *
     * The panel resolution comes from LVGL's default display, not from the BSP
     * config, so the shell does not take a build dependency on the board layer
     * just to learn the screen size. */
    int32_t panel_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t panel_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t root_h = panel_h - THEME_BAND_HEIGHT_PX;

    app->root = lv_obj_create(app->screen);
    lv_obj_set_size(app->root, panel_w, root_h);
    lv_obj_align(app->root, LV_ALIGN_TOP_MID, 0, 0);
    strip_root(app->root);
    /* Default text color for content an app renders on its root. The root is
     * transparent over the screen's black base, so labels need a light color to
     * read; without this they fall back to the LVGL default theme's grey, which
     * is meant for a light background and is barely visible on black. An app
     * that paints its own light surface, or a widget like the list that carries
     * its own text color, overrides this locally. The color is a theme token,
     * not a literal here. */
    lv_obj_set_style_text_color(app->root, THEME_APP_TEXT, LV_PART_MAIN);

    app->softkey_band = lv_obj_create(app->screen);
    lv_obj_set_size(app->softkey_band, panel_w, THEME_BAND_HEIGHT_PX);
    lv_obj_align(app->softkey_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(app->softkey_band, THEME_BAND_FILL, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(app->softkey_band, THEME_BAND_SHEEN, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(app->softkey_band, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(app->softkey_band, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(app->softkey_band, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(app->softkey_band, THEME_CELL_PAD_PX, LV_PART_MAIN);
    lv_obj_set_style_text_color(app->softkey_band, THEME_BAND_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(app->softkey_band, THEME_FONT_BAND, LV_PART_MAIN);
    lv_obj_remove_flag(app->softkey_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(app->softkey_band, softkey_right_tapped, LV_EVENT_CLICKED, app);

    app->softkey_left = lv_label_create(app->softkey_band);
    lv_label_set_text(app->softkey_left, "");
    lv_obj_align(app->softkey_left, LV_ALIGN_LEFT_MID, 0, 0);
    app->softkey_right = lv_label_create(app->softkey_band);
    lv_label_set_text(app->softkey_right, "");
    lv_obj_align(app->softkey_right, LV_ALIGN_RIGHT_MID, 0, 0);

    bind_api(app);
    s_running = app;

    /* Lay the screen out before run() so an app that measures its root (for
     * example, the touch app sizing a canvas to lv_obj_get_content_width/height)
     * reads the final concrete geometry rather than a pre-layout size. */
    lv_screen_load(app->screen);
    lv_obj_update_layout(app->screen);
    guarded_run(app);
    ESP_LOGI(TAG, "launched '%s'", descriptor->name ? descriptor->name : "?");
}

static void app_host_back(void *ctx)
{
    app_context_t *app = ctx ? ctx : s_running;
    if (app == NULL) {
        return;
    }

    if (app->descriptor->cleanup) {
        app->descriptor->cleanup(&app->api, app);
    }

    lv_obj_t *screen = app->screen;
    s_running = NULL;
    lv_free(app);

    /* Reload the persistent launcher, then destroy the app screen. Loading
     * first ensures LVGL is never left without an active screen. */
    lv_screen_load(launcher_screen());
    lv_obj_delete(screen);
    ESP_LOGI(TAG, "returned to launcher");
}