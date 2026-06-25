/*
 * Settings app.
 *
 * A list-heavy screen, so it uses the curated list widget rather than the tile
 * grid. It demonstrates several services through the curated API:
 *
 *   - A "Brightness" row reads a persisted step on entry, applies it to the
 *     panel backlight through the brightness capability, and advances it on
 *     tap, persisting across reboots in the app's own storage folder. When the
 *     brightness capability is unavailable (a board with a hardwired
 *     backlight), the row still cycles and persists the preference but reports
 *     that dimming is unavailable, so the UI degrades gracefully.
 *
 *   - An "About" row swaps the list for an inline info panel showing device
 *     identity and live runtime stats (firmware version, uptime, free heap),
 *     and back to the list when dismissed.
 *
 * All access is through the curated API; the app never names a filesystem or a
 * path outside its folder, and never touches the backlight hardware directly.
 *
 * Visual model: the curated list widget carries the LVGL default theme's own
 * light surface and dark text, which does not match the device's black app
 * base. The list and its items are restyled here to a black surface with white
 * text and the signature accent on the pressed/focused state, so the Settings
 * screen reads like the other apps (black background, white text) rather than
 * a white panel. The styling uses theme tokens, so a device re-theme carries
 * Settings with it.
 */
#include "app_descriptor.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"

/* The launcher tile icon, compiled in from a build-time-converted image. */
extern const lv_image_dsc_t icon_settings_dsc;

/* Firmware version shown on the About panel. Defined locally rather than via a
 * shell-wide version service because Settings is the only consumer; promote it
 * to a shared constant if another screen ever needs it. */
#define SETTINGS_FW_VERSION "1.0.0"

/* Brightness model. The preference persists as a step index 0..(STEPS-1); each
 * step maps to a percentage applied to the backlight. The lowest step is a
 * usable dim rather than fully off, so a user cannot accidentally cycle the
 * panel dark and lose the screen. */
#define BRIGHTNESS_PREF_FILE "brightness"
#define BRIGHTNESS_STEPS 5

/* Step-to-percent map. Index i is the backlight percentage for step i. The
 * floor is 20% so the dimmest setting is still readable. */
static const uint8_t BRIGHTNESS_PERCENTS[BRIGHTNESS_STEPS] = { 20, 40, 60, 80, 100 };

/* How often the About panel refreshes its live runtime stats (uptime, free
 * heap). One second is frequent enough for a readable uptime tick without
 * churning the display. */
#define SETTINGS_ABOUT_REFRESH_MS 1000

/*
 * Per-app working state. The API pointer and context are stashed at run() so
 * the list-item and timer callbacks (which receive only ctx, or nothing) can
 * reach storage, brightness, and the widgets.
 */
typedef struct {
    const app_api_t *api;
    void *ctx;

    /* The settings list and the brightness row within it. */
    lv_obj_t *list;
    lv_obj_t *brightness_item;
    int brightness_step;

    /* The About panel: a container shown in place of the list, refreshed by a
     * timer while visible. NULL when About is not open. */
    lv_obj_t *about_panel;
    lv_obj_t *about_uptime_label;
    lv_obj_t *about_heap_label;
    lv_timer_t *about_timer;
} settings_app_state_t;

static settings_app_state_t s_state;

/*
 * Style the list container to the device's black app look: an opaque black
 * surface, no border or radius, white text. Without this the list inherits the
 * LVGL default theme's light panel.
 */
static void style_list(lv_obj_t *list)
{
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, THEME_APP_TEXT, LV_PART_MAIN);
}

/*
 * Style one list item (a button) to match: transparent over the black list so
 * the black shows through in the normal state, white text, no border, and the
 * signature accent as the pressed-state fill so a tap reads clearly.
 */
static void style_list_item(lv_obj_t *item)
{
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(item, THEME_APP_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(item, 0, LV_PART_MAIN);

    /* A thin separator below each row, in the dim track slate, so rows are
     * distinguishable against the flat black without a heavy border. */
    lv_obj_set_style_border_color(item, THEME_TIMER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);

    /* Pressed state: fill with the signature accent so a tap is visible. */
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(item, THEME_ACCENT, LV_PART_MAIN | LV_STATE_PRESSED);
}

/*
 * Rewrite the brightness row's label to show the current step. The list item
 * is a button whose last child is its text label.
 */
static void render_brightness_label(void)
{
    if (s_state.brightness_item == NULL) {
        return;
    }
    char text[40];
    snprintf(text, sizeof(text), "Brightness: %d/%d",
             s_state.brightness_step + 1, BRIGHTNESS_STEPS);
    lv_obj_t *label = lv_obj_get_child(s_state.brightness_item, -1);
    if (label) {
        lv_label_set_text(label, text);
    }
}

/*
 * Apply the current brightness step to the backlight through the capability.
 * No-op (other than the label, handled by the caller) when the capability is
 * unavailable, so the preference still cycles and persists on a board that
 * cannot dim.
 */
static void apply_brightness(void)
{
    if (s_state.api->brightness_available(s_state.ctx)) {
        uint8_t percent = BRIGHTNESS_PERCENTS[s_state.brightness_step];
        s_state.api->brightness_set(s_state.ctx, percent);
    }
}

/*
 * Advance the brightness step, apply it to the backlight, and persist it.
 * Wraps at the top. The new value is written through the curated storage
 * service into the app's folder.
 */
static void brightness_tapped(void *ctx, const char *text)
{
    (void)ctx;
    (void)text;
    s_state.brightness_step = (s_state.brightness_step + 1) % BRIGHTNESS_STEPS;

    apply_brightness();

    uint8_t value = (uint8_t)s_state.brightness_step;
    s_state.api->storage_write(s_state.ctx, BRIGHTNESS_PREF_FILE,
                               &value, sizeof(value));
    render_brightness_label();
}

/*
 * Refresh the About panel's live stats. Uptime comes from the system tick
 * (microseconds since boot) and free heap from the heap allocator. Rewritten
 * once per second by the About timer while the panel is visible.
 */
static void about_refresh(void)
{
    if (s_state.about_uptime_label) {
        uint64_t us = (uint64_t)esp_timer_get_time();
        uint32_t total_seconds = (uint32_t)(us / 1000000ULL);
        uint32_t h = (total_seconds / 3600) % 100;
        uint32_t m = (total_seconds / 60) % 60;
        uint32_t sec = total_seconds % 60;
        char text[40];
        snprintf(text, sizeof(text), "Uptime: %02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)sec);
        lv_label_set_text(s_state.about_uptime_label, text);
    }
    if (s_state.about_heap_label) {
        uint32_t free_bytes = (uint32_t)esp_get_free_heap_size();
        char text[40];
        snprintf(text, sizeof(text), "Free memory: %lu KB",
                 (unsigned long)(free_bytes / 1024));
        lv_label_set_text(s_state.about_heap_label, text);
    }
}

static void about_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    about_refresh();
}

/* Forward declaration: the About panel's Close row returns to the list. */
static void close_about(void);

/*
 * LVGL click handler for the About panel's Close button. The button is created
 * directly (not through the curated api->button, which parents under root), so
 * it carries a plain LVGL event callback that returns to the settings list.
 */
static void about_close_event_cb(lv_event_t *event)
{
    (void)event;
    close_about();
}

/*
 * Build and show the About panel in place of the list. The list is hidden
 * rather than destroyed so it can be restored cheaply on close. The panel
 * stacks device-identity labels and live-stat labels vertically on the black
 * app base, with a Close row at the bottom. A one-second timer refreshes the
 * live stats while the panel is open.
 */
static void open_about(void)
{
    if (s_state.about_panel != NULL) {
        return;
    }

    /* Hide the list while About is shown. */
    if (s_state.list) {
        lv_obj_add_flag(s_state.list, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *root = s_state.api->root(s_state.ctx);
    lv_obj_t *panel = lv_obj_create(root);
    s_state.about_panel = panel;
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, THEME_CELL_PAD_PX * 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel, THEME_APP_TEXT, LV_PART_MAIN);

    /* Stack the rows top-to-bottom with a small gap. */
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, THEME_CELL_PAD_PX, LV_PART_MAIN);

    /* Device identity: name in the title font, then the full product name and
     * firmware version in body text. */
    lv_obj_t *name = lv_label_create(panel);
    lv_label_set_text(name, "PROME");
    lv_obj_set_style_text_font(name, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *desc = lv_label_create(panel);
    lv_label_set_text(desc, "Productivity and Media console");

    lv_obj_t *version = lv_label_create(panel);
    lv_label_set_text(version, "Firmware: " SETTINGS_FW_VERSION);

    /* Live runtime stats, refreshed by the timer below. */
    s_state.about_uptime_label = lv_label_create(panel);
    s_state.about_heap_label = lv_label_create(panel);
    about_refresh();

    /* Close row: a button styled like a list item, returning to the list. It
     * is created directly under the panel (the curated api->button parents
     * under root, not the panel), so it carries a plain LVGL click callback. */
    lv_obj_t *close = lv_button_create(panel);
    lv_obj_set_width(close, LV_PCT(100));
    style_list_item(close);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_add_event_cb(close, about_close_event_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh timer while the panel is open. */
    s_state.about_timer = lv_timer_create(about_timer_cb,
                                          SETTINGS_ABOUT_REFRESH_MS, NULL);

    /* Keep the softkeys consistent while About is open: Back exits the app as
     * usual, and the Close row dismisses the panel back to the list. */
    s_state.api->set_softkeys(s_state.ctx, "Menu", "Back");
}

/*
 * Close the About panel and restore the list. Deletes the refresh timer and
 * the panel, unhides the list, and clears the About state.
 */
static void close_about(void)
{
    if (s_state.about_timer) {
        lv_timer_delete(s_state.about_timer);
        s_state.about_timer = NULL;
    }
    if (s_state.about_panel) {
        lv_obj_delete(s_state.about_panel);
        s_state.about_panel = NULL;
    }
    s_state.about_uptime_label = NULL;
    s_state.about_heap_label = NULL;

    if (s_state.list) {
        lv_obj_remove_flag(s_state.list, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * About row tap: open the inline About panel.
 */
static void about_tapped(void *ctx, const char *text)
{
    (void)ctx;
    (void)text;
    open_about();
}

static void settings_app_run(const app_api_t *api, void *ctx)
{
    s_state.api = api;
    s_state.ctx = ctx;
    s_state.about_panel = NULL;
    s_state.about_timer = NULL;
    s_state.about_uptime_label = NULL;
    s_state.about_heap_label = NULL;

    api->set_softkeys(ctx, "Menu", "Back");

    lv_obj_t *root = api->root(ctx);
    lv_obj_t *list = api->list(ctx, root);
    s_state.list = list;
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    style_list(list);

    /* Load the persisted brightness step, defaulting to full (top step) when
     * absent so a fresh device comes up bright. */
    uint8_t value = 0;
    int got = api->storage_read(ctx, BRIGHTNESS_PREF_FILE, &value, sizeof(value));
    s_state.brightness_step = (got == (int)sizeof(value) && value < BRIGHTNESS_STEPS)
                                  ? value : (BRIGHTNESS_STEPS - 1);

    /* Apply the loaded brightness to the backlight on entry so the panel
     * matches the persisted preference. */
    apply_brightness();

    s_state.brightness_item = api->list_add(ctx, list, "Brightness",
                                            brightness_tapped, NULL);
    style_list_item(s_state.brightness_item);
    render_brightness_label();

    lv_obj_t *about_item = api->list_add(ctx, list, "About", about_tapped, NULL);
    style_list_item(about_item);
}

/*
 * Cleanup on exit. The list and About panel are children of the app root and
 * are freed when the shell destroys the app screen; only the About refresh
 * timer is an independent resource that must be deleted here so it does not
 * fire after the app is gone.
 */
static void settings_app_cleanup(const app_api_t *api, void *ctx)
{
    (void)api;
    (void)ctx;
    if (s_state.about_timer) {
        lv_timer_delete(s_state.about_timer);
        s_state.about_timer = NULL;
    }
    s_state.about_panel = NULL;
    s_state.about_uptime_label = NULL;
    s_state.about_heap_label = NULL;
    s_state.list = NULL;
    s_state.brightness_item = NULL;
}

static const app_descriptor_t settings_app = {
    .name = "Settings",
    .icon = &icon_settings_dsc,
    .run = settings_app_run,
    .cleanup = settings_app_cleanup,
};

REGISTER_APP(settings_app);