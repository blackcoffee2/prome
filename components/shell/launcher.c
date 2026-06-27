/*
 * Launcher — implementation.
 *
 * Builds the three-pane home screen, laid out for the LANDSCAPE 320x240
 * panel: a status (title) pane pinned to the top showing the device title and
 * an uptime clock, a softkey (control) pane pinned to the bottom, and a
 * scrollable main pane of icons filling the space between. Every visual value
 * is read from the theme header by role; nothing is styled inline, so the whole
 * look changes by editing theme.h.
 *
 * Main pane: each app is rendered as a bare icon with its name beneath it --
 * there is no per-icon card or container. The icons sit directly on the
 * background (a compiled-in wallpaper image, with a light gradient beneath it
 * as the base). The focused icon gets a soft rounded blue highlight behind it,
 * the one signature accent. The grid is a flex-wrap container sized so four
 * icons span the 320 px width; rows wrap and the pane scrolls vertically when
 * apps exceed the visible two rows.
 *
 * The launcher screen is persistent: built once, never destroyed. App screens
 * are layered over it and torn down on exit, returning here. One tile per
 * registered app is built by walking the app registry; including an app in the
 * build adds its icon here, excluding it removes the icon, with no change to
 * this code. The launcher never names any app.
 *
 * Status clock: there is no RTC wired yet, so the clock shows time elapsed
 * since boot (HH:MM:SS), starting at 00:00:00 each boot. It is driven by a 1 s
 * LVGL timer on the shell's single task; because the launcher screen is never
 * destroyed, the timer needs no teardown. Swapping in an RTC or SNTP source
 * later is a change to the tick callback alone.
 */
#include "launcher.h"
#include "theme.h"
#include "app_host.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "launcher";

static lv_obj_t *s_launcher_screen = NULL;

/* The status-pane clock label and the elapsed-second counter behind it. The
 * label is rewritten once per second by the uptime timer. */
static lv_obj_t *s_clock_label = NULL;
static uint32_t s_uptime_seconds = 0;

/*
 * Status-clock tick. Advances the elapsed-second counter and rewrites the clock
 * label as HH:MM:SS. Runs on the shell's LVGL task once per second. This is an
 * uptime counter, not wall-clock time: with no RTC wired it reads 00:00:00 at
 * boot and counts up. Replacing the source with an RTC/SNTP read is a change
 * confined to this function.
 */
static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_clock_label == NULL) {
        return;
    }
    s_uptime_seconds++;
    uint32_t h = (s_uptime_seconds / 3600) % 100;
    uint32_t m = (s_uptime_seconds / 60) % 60;
    uint32_t sec = s_uptime_seconds % 60;
    char text[16];
    snprintf(text, sizeof(text), "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    lv_label_set_text(s_clock_label, text);
}

/*
 * Apply the light vertical gradient backdrop to a screen-level object, then lay
 * the wallpaper image over it. The gradient is always set so it is the base
 * beneath the image and shows during any partial redraw before the image
 * composites. THEME_WALLPAPER_SRC is a compiled-in descriptor that is always
 * present (never NULL), so it is set unconditionally; the launcher does not
 * branch on its presence. Shared by the launcher and, later, app screens that
 * want the same backdrop.
 */
static void apply_surface(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, THEME_SURFACE_TOP, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, THEME_SURFACE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, THEME_SURFACE_GRAD_DIR, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);

    /* Full-screen wallpaper over the gradient base. */
    lv_obj_set_style_bg_image_src(obj, THEME_WALLPAPER_SRC, LV_PART_MAIN);
}

/*
 * Build one pane (status or softkey) as a light glass strip with a top sheen
 * and dark band text. Returns the pane object so the caller can place labels
 * into it.
 */
static lv_obj_t *build_band(lv_obj_t *parent, lv_align_t align)
{
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_set_size(band, LV_PCT(100), THEME_BAND_HEIGHT_PX);
    lv_obj_align(band, align, 0, 0);
    lv_obj_set_style_bg_color(band, THEME_BAND_FILL, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(band, THEME_BAND_SHEEN, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(band, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(band, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(band, THEME_CELL_PAD_PX, LV_PART_MAIN);
    lv_obj_set_style_text_color(band, THEME_BAND_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(band, THEME_FONT_BAND, LV_PART_MAIN);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    return band;
}

/*
 * Style a cell to the bare-icon look: no fill, border, or radius in the
 * normal state, so the icon art sits directly on the background with no card
 * around it. The cell is a transparent, focusable hit area at the themed cell
 * size with light inner padding. The focused state is the signature: a soft
 * rounded blue highlight is painted behind the icon, with a gentle glow.
 *
 * The cell does NOT clip its content: with the icon constrained to
 * THEME_ICON_SIZE_PX and a label beneath it, the stacked column fits inside the
 * cell, but clearing the clip flag guarantees the label is never cut off at the
 * content edge if a name's text height or a future metric tweak pushes it to
 * the boundary. The grid pane, not the cell, owns scrolling.
 */
static void style_cell(lv_obj_t *cell)
{
    lv_obj_set_size(cell, THEME_CELL_W, THEME_CELL_H);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, THEME_ACCENT_RADIUS_PX, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, THEME_CELL_PAD_PX, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(cell, THEME_CELL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(cell, THEME_FONT_CELL, LV_PART_MAIN);

    /* The cell holds an icon stacked above a label; neither should be clipped
     * by the cell's own bounds, and the cell itself must not scroll. */
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(cell, false, LV_PART_MAIN);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* Signature: a filled rounded accent highlight behind the focused icon,
     * with a soft glow. No border ring -- the highlight itself reads as the
     * selection. */
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(cell, THEME_ACCENT, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_color(cell, THEME_ACCENT_GLOW,
                                  LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(cell, 16, LV_PART_MAIN | LV_STATE_FOCUSED);
}

/*
 * Cell tap handler. The descriptor for the tapped icon is carried in the
 * event's user data; tapping launches that app through the host. The launcher
 * never branches on which app it is — it just hands the descriptor back.
 */
static void cell_tapped_cb(lv_event_t *event)
{
    const app_descriptor_t *descriptor = lv_event_get_user_data(event);
    app_host_launch(descriptor);
}

/*
 * Constrain an icon image object to the themed icon box and scale its source
 * art to fit. The source PNGs are converted at whatever resolution the art
 * happens to be (often larger than the cell), so without this the image renders
 * at native size and overruns the cell, pushing the name labels out of view.
 * Fixing the object size and enabling inner-align scaling makes every icon
 * render at exactly THEME_ICON_SIZE_PX square regardless of its source
 * resolution.
 *
 * lv_image_set_inner_align with LV_IMAGE_ALIGN_CONTAIN scales the source down
 * (or up) to fit the object's set width/height while preserving aspect ratio,
 * so non-square art is letterboxed inside the box rather than stretched.
 */
static void fit_icon(lv_obj_t *icon)
{
    lv_obj_set_size(icon, THEME_ICON_SIZE_PX, THEME_ICON_SIZE_PX);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
}

/*
 * Build one cell for a descriptor: a transparent focusable hit area holding the
 * icon (or a generated initial-on-cell placeholder when the descriptor has
 * none) above the name label, and the tap binding that launches the app. The
 * icon and label are laid out vertically with no container drawn around them.
 */
static void build_cell(lv_obj_t *grid, const app_descriptor_t *descriptor)
{
    lv_obj_t *cell = lv_button_create(grid);
    style_cell(cell);
    lv_obj_add_event_cb(cell, cell_tapped_cb, LV_EVENT_CLICKED,
                        (void *)descriptor);

    /* Stack the icon above the label inside the cell, centered. A column flex
     * keeps them vertically arranged with the icon on top. */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cell, 2, LV_PART_MAIN);

    if (descriptor->icon) {
        lv_obj_t *icon = lv_image_create(cell);
        lv_image_set_src(icon, descriptor->icon);
        /* Pin the icon to the themed box and scale the source into it, so the
         * art never overruns the cell or crowds out the label below. */
        fit_icon(icon);
    } else {
        /* Placeholder: the app's initial, sized to roughly the icon box so the
         * cell keeps a consistent height whether or not art is present. */
        lv_obj_t *initial = lv_label_create(cell);
        char glyph[2] = { descriptor->name ? descriptor->name[0] : '?', '\0' };
        lv_label_set_text(initial, glyph);
        lv_obj_set_style_text_font(initial, THEME_FONT_TITLE, LV_PART_MAIN);
    }

    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, descriptor->name ? descriptor->name : "");
    /* Keep a long name from forcing the cell wider than its themed width: cap
     * the label to the cell's inner width and dot-truncate the overflow, so the
     * four-column layout holds regardless of name length. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, THEME_CELL_W - 2 * THEME_CELL_PAD_PX);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

lv_obj_t *launcher_build(void)
{
    s_launcher_screen = lv_obj_create(NULL);
    apply_surface(s_launcher_screen);

    /* Status (title) pane: device title on the left, uptime clock on the right.
     * The title reads "PROME" (Productivity and Media console). The clock is
     * rewritten once per second by the uptime timer registered below. */
    lv_obj_t *status = build_band(s_launcher_screen, LV_ALIGN_TOP_MID);
    lv_obj_t *title_label = lv_label_create(status);
    lv_label_set_text(title_label, "PROME");
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_clock_label = lv_label_create(status);
    lv_label_set_text(s_clock_label, "00:00:00");
    lv_obj_align(s_clock_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Softkey (control) pane. On the launcher the left softkey reads "Menu" but
     * is inert -- we are already home; the global Menu jump-to-home applies
     * inside apps, not here. The right softkey reads "Back". */
    lv_obj_t *softkeys = build_band(s_launcher_screen, LV_ALIGN_BOTTOM_MID);
    lv_obj_t *softkey_left = lv_label_create(softkeys);
    lv_label_set_text(softkey_left, "Menu");
    lv_obj_align(softkey_left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *softkey_right = lv_label_create(softkeys);
    lv_label_set_text(softkey_right, "Back");
    lv_obj_align(softkey_right, LV_ALIGN_RIGHT_MID, 0, 0);

    /* The main pane fills the space between the two bands. Its size is set in
     * concrete pixels, not LV_PCT(100) - 2 * THEME_BAND_HEIGHT_PX: LV_PCT is an
     * encoded value, so subtracting a pixel constant from it corrupts the
     * encoding and resolves to nearly the full parent height, overflowing the
     * bottom band. The panel size is read from LVGL's default display (so the
     * launcher needs no BSP dependency to learn the screen size, as the app host
     * does), giving a grid of the full panel width by the height minus both
     * bands. It is a flex-wrap container: four 70 px cells plus three 6 px gaps
     * span 298 px inside the 308 px usable width, so four columns fit per row. */
    int32_t panel_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t panel_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t grid_h = panel_h - 2 * THEME_BAND_HEIGHT_PX;

    lv_obj_t *grid = lv_obj_create(s_launcher_screen);
    lv_obj_set_size(grid, panel_w, grid_h);
    /* Anchor between the bands: offset down by exactly the top band's height so
     * the grid starts where the status pane ends and runs to where the softkey
     * pane begins, claiming the whole middle region. */
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, THEME_BAND_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, THEME_CELL_GAP_PX, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(grid, THEME_CELL_GAP_PX, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    /* One icon per registered app. Including an app in the build adds its icon
     * here; excluding it removes the icon with no change to this code. */
    size_t count = app_host_count();
    for (size_t i = 0; i < count; i++) {
        const app_descriptor_t *descriptor = app_host_descriptor(i);
        if (descriptor) {
            build_cell(grid, descriptor);
        }
    }

    /* Uptime clock timer. The launcher screen is persistent (never destroyed),
     * so this timer lives for the device's uptime and needs no teardown. */
    lv_timer_create(clock_tick, 1000, NULL);

    lv_screen_load(s_launcher_screen);
    ESP_LOGI(TAG, "launcher built (%u apps)", (unsigned)count);
    return s_launcher_screen;
}

lv_obj_t *launcher_screen(void)
{
    return s_launcher_screen;
}