/*
 * Clock app — focus timer.
 *
 * The timer starts the instant the app opens and shows how long you have been
 * focused, with no controls: opening the app is "start", leaving it (the global
 * Back softkey) is "stop". The display is a ring that fills smoothly over each
 * minute and resets, with a large HH:MM:SS readout centered inside it. The
 * filling ring reads at a glance as a running timer; the numeric readout carries
 * the long-duration value.
 *
 * Run-then-live model: run() builds the widgets and registers an LVGL timer,
 * then returns. The app has no loop of its own; it lives through the timer
 * callback the shell's single LVGL task drives. cleanup deletes the timer so it
 * stops firing once the app exits.
 *
 * Two rendering constraints are load-bearing here and must not be undone:
 *
 *   1. The filling arc is fine to sweep continuously. An lv_arc invalidates its
 *      whole bounding box on each value change, but at this ring size that
 *      redraw is cheap; sweeping it at ~30 fps looks smooth on this panel.
 *
 *   2. The readout label MUST have a fixed width with center-aligned text. A
 *      default label sizes itself to its content (LV_SIZE_CONTENT) and
 *      re-centers every time the text changes. Because the digits in this font
 *      are not tabular and rollovers change the string width, a content-sized
 *      label's box moves and resizes every second, so LVGL invalidates and
 *      repaints a wide, shifting strip across the middle of the screen -- which
 *      on this partial-buffer SPI panel reads as a once-per-second flash.
 *      Giving the label a FIXED width and center-aligned text pins the object's
 *      box so it never moves or resizes; only the glyphs inside change, so the
 *      invalidated region is a single stable rectangle. This is what keeps the
 *      readout from flashing while the arc keeps sweeping smoothly.
 *
 * Elapsed time is measured from an lv_tick_get() baseline captured at launch
 * rather than by counting timer fires. Counting fires drifts if a tick is ever
 * late or coalesced; reading the tick clock each update keeps both the ring fill
 * and the readout accurate regardless of how punctually the timer runs. This
 * measures time-since-launch, not wall-clock time, since no RTC is wired;
 * swapping in an RTC or SNTP baseline later is a change to the baseline capture
 * and the elapsed computation alone.
 *
 * Theme boundary: the ring's two colors come from the theme (THEME_TIMER_RING
 * for the filled portion, THEME_TIMER_TRACK for the groove), so the timer
 * participates in the device look and follows a re-theme of the accent.
 * Everything specific to this one app -- the ring's pixel diameter and stroke
 * width, the readout font and its fixed box width, the update cadence -- is
 * defined locally below, since no other screen shares those metrics. The app
 * paints no surface of its own: it renders on the shell's transparent root over
 * the screen's black base, so the ring and the light readout read directly
 * against black.
 */
#include "app_descriptor.h"
#include "theme.h"

#include <stdio.h>

/* The launcher tile icon, compiled in from a build-time-converted image. */
extern const lv_image_dsc_t icon_clock_dsc;

/* Update cadence. The ring fills continuously, so the timer fires at ~30 fps to
 * keep the sweep smooth. The readout label is rewritten at most once per second,
 * when the displayed second changes. */
#define CLOCK_TICK_PERIOD_MS 33

/* The ring fills over one minute. The arc's value range is the minute in
 * milliseconds, so the fill advances smoothly within each update rather than
 * stepping once per second. */
#define CLOCK_SWEEP_PERIOD_MS 60000

/* Ring geometry, private to this app. CLOCK_RING_SIZE_PX is the diameter, set
 * to 200 px so the 40 px readout sits inside with margin; the app root is 320 px
 * wide, so a 200 px ring centers comfortably. CLOCK_RING_WIDTH_PX is the arc
 * stroke thickness, applied to both the track and the indicator so the fill
 * runs in a groove of constant width. */
#define CLOCK_RING_SIZE_PX  200
#define CLOCK_RING_WIDTH_PX 10

/* Readout font and box, private to this app. montserrat_40 is the largest
 * built-in Montserrat whose "00:00:00" rendering (about 165 px) fits inside the
 * 200 px ring. It must be enabled in sdkconfig.defaults
 * (CONFIG_LV_FONT_MONTSERRAT_40) or the build cannot resolve the symbol.
 *
 * CLOCK_READOUT_W_PX is a FIXED label width, wide enough to hold the readout at
 * its maximum rendered width with a little slack. Fixing the width (with
 * center-aligned text) is what prevents the per-second flash: the label's box
 * stays put and the same size across every text change, so only a stable
 * rectangle is invalidated rather than a re-centered, resized strip. 180 px
 * comfortably exceeds the ~165 px the widest "00:00:00" needs while still
 * fitting within the 200 px ring. */
#define CLOCK_FONT_READOUT (&lv_font_montserrat_40)
#define CLOCK_READOUT_W_PX 180

typedef struct {
    lv_obj_t *ring;
    lv_obj_t *time_label;
    lv_timer_t *tick;
    uint32_t start_tick;
    uint32_t last_shown_second;
} clock_app_state_t;

static clock_app_state_t s_state;

/*
 * Timer callback. Reads the elapsed time from the tick baseline, advances the
 * ring fill every fire for smooth motion, and rewrites the HH:MM:SS readout only
 * when the whole-second value changes (the label text is identical between
 * sub-second fires, so rewriting it every fire would be a wasted invalidation).
 */
static void clock_tick(lv_timer_t *timer)
{
    (void)timer;

    uint32_t elapsed_ms = lv_tick_elaps(s_state.start_tick);

    /* Smooth ring fill: position within the current minute, in milliseconds. */
    if (s_state.ring) {
        lv_arc_set_value(s_state.ring, (int32_t)(elapsed_ms % CLOCK_SWEEP_PERIOD_MS));
    }

    /* Readout: only touch the label when the displayed second changes. */
    uint32_t total_seconds = elapsed_ms / 1000;
    if (total_seconds != s_state.last_shown_second && s_state.time_label) {
        s_state.last_shown_second = total_seconds;
        uint32_t h = (total_seconds / 3600) % 100;
        uint32_t m = (total_seconds / 60) % 60;
        uint32_t sec = total_seconds % 60;
        char text[16];
        snprintf(text, sizeof(text), "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)sec);
        lv_label_set_text(s_state.time_label, text);
    }
}

/*
 * Configure the filling ring. An lv_arc styled as a non-interactive indicator:
 * its knob is hidden and click response removed so it is a pure display, the
 * background arc is the dim track and the indicator arc is the accent fill, and
 * the value range is the minute in milliseconds. The arc sweeps a full 360
 * degrees starting at the top (12 o'clock) so a full minute is a full circle.
 */
static void configure_ring(lv_obj_t *ring)
{
    lv_obj_set_size(ring, CLOCK_RING_SIZE_PX, CLOCK_RING_SIZE_PX);
    lv_obj_center(ring);

    /* Full-circle sweep beginning at the top. */
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_range(ring, 0, CLOCK_SWEEP_PERIOD_MS);
    lv_arc_set_value(ring, 0);

    /* Non-interactive: hide the adjustment knob and stop the arc from reacting
     * to presses, so it is a display element rather than a control. */
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    /* Track (unfilled remainder) and indicator (filled portion). The two colors
     * are theme tokens; the width is the app-local stroke metric. Rounded caps
     * give the fill a soft leading edge. */
    lv_obj_set_style_arc_color(ring, THEME_TIMER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, CLOCK_RING_WIDTH_PX, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, THEME_TIMER_RING, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, CLOCK_RING_WIDTH_PX, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
}

static void clock_app_run(const app_api_t *api, void *ctx)
{
    api->set_softkeys(ctx, "Menu", "Back");
    lv_obj_t *root = api->root(ctx);

    s_state.start_tick = lv_tick_get();
    /* Force the first tick to render the readout: no second has been shown yet,
     * and a value the elapsed second can never equal guarantees the first fire
     * writes the label. */
    s_state.last_shown_second = (uint32_t)-1;

    /* The filling ring, centered in the app root. */
    s_state.ring = lv_arc_create(root);
    configure_ring(s_state.ring);

    /* The large HH:MM:SS readout, centered over the ring. The label is given a
     * FIXED width with center-aligned text so its box never moves or resizes as
     * the text changes -- this is what stops the per-second flash. With a fixed
     * width the label no longer shrinks to its content, so it is centered as a
     * fixed-size box over the ring. */
    s_state.time_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_state.time_label, CLOCK_FONT_READOUT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.time_label, THEME_APP_TEXT, LV_PART_MAIN);
    lv_obj_set_width(s_state.time_label, CLOCK_READOUT_W_PX);
    lv_obj_set_style_text_align(s_state.time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_state.time_label, "00:00:00");
    lv_obj_center(s_state.time_label);

    /* Register the update timer; the app lives through it after run() returns. */
    s_state.tick = lv_timer_create(clock_tick, CLOCK_TICK_PERIOD_MS, NULL);
}

static void clock_app_cleanup(const app_api_t *api, void *ctx)
{
    (void)api;
    (void)ctx;
    if (s_state.tick) {
        lv_timer_delete(s_state.tick);
        s_state.tick = NULL;
    }
    s_state.ring = NULL;
    s_state.time_label = NULL;
}

static const app_descriptor_t clock_app = {
    .name = "Clock",
    .icon = &icon_clock_dsc,
    .run = clock_app_run,
    .cleanup = clock_app_cleanup,
};

REGISTER_APP(clock_app);