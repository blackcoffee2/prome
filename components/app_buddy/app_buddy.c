/*
 * Buddy app — an animated pixel-art panda doing cute positive gestures.
 *
 * Run-then-live model, identical in shape to the clock app: run() builds a
 * canvas and registers one LVGL timer, then returns. The app has no loop of its
 * own; it lives through the timer callback the shell's single LVGL task drives,
 * and cleanup() deletes the timer and frees the app-owned canvas buffer on
 * exit. All the heavy pixel work — compositing the panda each frame — lives in
 * panda_sprite.c behind a coarse render verb; this file owns only the timing
 * and the choreography.
 *
 * Drawing surface: the app owns a PSRAM-backed canvas buffer (the one
 * allocation the curated model leaves to the caller), mirroring app_camera and
 * app_touch. The canvas is sized to the app root's real pixel area, measured
 * after a layout pass, so it fills the usable screen above the softkey band.
 * The panda is centered within it and composited by panda_sprite_render.
 *
 * Backdrop: the buddy paints its own canvas, exactly as the camera app paints
 * its viewfinder, so it is entitled to its own backdrop rather than the
 * device's black app base (that rule governs apps that do NOT paint their own
 * surface). The backdrop is a dark forest green rather than black on purpose:
 * the panda's white face and belly need a darker surface to read against, and a
 * deep green is on-theme for a bamboo-loving panda while being far gentler on
 * the eyes in a dark room than a bright green, which would glow. The forest
 * green sits clearly above the fur black (0x1A1A1E) so the panda's black fur
 * (ears, arms, feet, eye-patch outlines) still keeps a readable silhouette in
 * every gesture — most importantly the wave and bounce, whose whole appeal is
 * the arms changing pose — while the white face, the pink blush, and the
 * theme-accent props all stand out crisply against the dark green.
 *
 * Choreography: a gesture is a short sequence of keyframes, each a pose held for
 * a number of milliseconds. A table of gestures spans calm and playful — idle,
 * blink, wave, happy bounce, wiggle, peek-a-boo, and a heart pop. The state
 * machine plays one gesture to completion, returns to a calm idle pose, waits a
 * randomized beat, then picks another gesture at random. Idle itself blinks
 * periodically so the buddy is never frozen. Tapping the panda interrupts the
 * idle wait and triggers a random gesture immediately.
 *
 * Timing: one LVGL timer at BUDDY_TICK_PERIOD_MS drives everything. Keyframe
 * advancement is measured from an lv_tick baseline with lv_tick_elaps rather
 * than by counting fires, so the animation does not drift if a tick is ever
 * late — the same drift-free approach the clock app uses. The canvas is only
 * recomposited and invalidated when the displayed pose actually changes (a new
 * keyframe, or a per-frame bob offset), so a held pose costs nothing and the
 * partial-buffer SPI panel repaints only when the image really changes.
 */
#include "app_descriptor.h"
#include "panda_sprite.h"
#include "theme.h"

#include "esp_heap_caps.h"
#include "esp_random.h"

/* The launcher tile icon, compiled in from a build-time-converted image. */
extern const lv_image_dsc_t icon_buddy_dsc;

/* No compiled-in launcher icon: the descriptor sets .icon = NULL, so the
 * launcher renders the initial-on-tile placeholder ("B"). A real icon can be
 * added later via the build-time conversion pipeline, as the other apps do. */

/* Timer cadence. ~12 fps (≈83 ms) is ample for chunky pixel art and keeps each
 * recomposite light; bobs and waves read smoothly at this rate. */
#define BUDDY_TICK_PERIOD_MS 83

/* The calm app backdrop the canvas is cleared to behind the panda. A dark
 * forest green rather than black, on-theme for a bamboo-loving panda and gentle
 * on the eyes in a dark room (a bright green would glow). It sits clearly above
 * the fur black (0x1A1A1E) so the panda's black fur (ears, arms, feet, eye-patch
 * outlines) still keeps a readable silhouette in every gesture, while the white
 * face and belly, the pink blush, and the theme-accent props all stand out
 * crisply against the dark green. */
#define BUDDY_BG_COLOR lv_color_hex(0x243D24)

/* Idle blink cadence bounds, in milliseconds. While idling (between gestures)
 * the buddy blinks at a randomized interval in this range so the ambient life
 * never looks metronomic. */
#define BUDDY_IDLE_BLINK_MIN_MS 2200
#define BUDDY_IDLE_BLINK_MAX_MS 4800

/* Pause between gestures, in milliseconds. After a gesture finishes the buddy
 * holds idle for a randomized beat in this range before auto-playing the next,
 * so the show paces itself naturally. A tap shortcuts this wait. */
#define BUDDY_IDLE_PAUSE_MIN_MS 1400
#define BUDDY_IDLE_PAUSE_MAX_MS 3600

/* --- Gesture data --- */
/*
 * One keyframe: a pose's part selection and vertical offset, held for
 * hold_ms milliseconds. A gesture is an array of these played in order. The
 * backdrop color is constant across the app, so it is applied at render time
 * rather than stored per keyframe.
 */
typedef struct {
    panda_eyes_t eyes;
    panda_arms_t arms;
    panda_prop_t prop;
    int32_t      y_offset;
    uint32_t     hold_ms;
} buddy_keyframe_t;

/*
 * One gesture: a named sequence of keyframes. The name is for logging and
 * clarity only; the scheduler picks gestures by index.
 */
typedef struct {
    const char             *name;
    const buddy_keyframe_t *frames;
    uint8_t                 frame_count;
} buddy_gesture_t;

/* The calm resting pose, also the frame the buddy returns to between gestures
 * and the baseline the idle blink animates from. Open eyes, resting paws, no
 * prop, no offset. */
static const buddy_keyframe_t IDLE_POSE = {
    PANDA_EYES_OPEN, PANDA_ARMS_REST, PANDA_PROP_NONE, 0, 0
};

/* Wave: the right arm lifts through a mid-raise to fully up, gives a couple of
 * wave beats at the top, then lowers back through the mid-raise to rest. Going
 * through PANDA_ARMS_WAVE_MID on the way up and down is what makes the arm
 * travel rather than snap between down and fully-raised. Eyes turn happy once
 * the arm is on its way up. */
static const buddy_keyframe_t WAVE_FRAMES[] = {
    { PANDA_EYES_OPEN,  PANDA_ARMS_WAVE_MID, PANDA_PROP_NONE, 0, 110 },  /* lift starts */
    { PANDA_EYES_HAPPY, PANDA_ARMS_WAVE,     PANDA_PROP_NONE, 0, 240 },  /* up */
    { PANDA_EYES_HAPPY, PANDA_ARMS_WAVE_MID, PANDA_PROP_NONE, 0, 180 },  /* wave down-beat */
    { PANDA_EYES_HAPPY, PANDA_ARMS_WAVE,     PANDA_PROP_NONE, 0, 240 },  /* up again */
    { PANDA_EYES_HAPPY, PANDA_ARMS_WAVE_MID, PANDA_PROP_NONE, 0, 180 },  /* wave down-beat */
    { PANDA_EYES_HAPPY, PANDA_ARMS_WAVE,     PANDA_PROP_NONE, 0, 240 },  /* up again */
    { PANDA_EYES_OPEN,  PANDA_ARMS_WAVE_MID, PANDA_PROP_NONE, 0, 110 },  /* lower */
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST,     PANDA_PROP_NONE, 0, 160 },  /* back to rest */
};

/* Happy bounce: both paws up, hopping up and down a few times. The negative
 * offsets lift the whole panda for the hop. */
static const buddy_keyframe_t BOUNCE_FRAMES[] = {
    { PANDA_EYES_HAPPY, PANDA_ARMS_UP,   PANDA_PROP_NONE,   0, 150 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_UP,   PANDA_PROP_NONE, -10, 180 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_UP,   PANDA_PROP_NONE,   0, 150 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_UP,   PANDA_PROP_NONE, -10, 180 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_UP,   PANDA_PROP_NONE,   0, 150 },
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_NONE,   0, 200 },
};

/* Heart pop: happy eyes and a theme-accent heart that appears beside the head,
 * with a gentle bob. */
static const buddy_keyframe_t HEART_FRAMES[] = {
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_NONE,   0, 200 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_HEART, -2, 360 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_HEART, -5, 360 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_HEART, -2, 360 },
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_NONE,   0, 200 },
};

/* Wiggle: a small side-to-side feel via a quick bob, with a sparkle shine. The
 * grid has no horizontal offset, so the wiggle reads through the bob and the
 * sparkle appearing/disappearing. */
static const buddy_keyframe_t WIGGLE_FRAMES[] = {
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_SPARKLE, -2, 200 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_NONE,     0, 200 },
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_SPARKLE, -2, 200 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_NONE,     0, 200 },
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_NONE,     0, 180 },
};

/* Peek-a-boo: paws cover the eyes (no eyes drawn), then reveal with a happy
 * face. */
static const buddy_keyframe_t PEEK_FRAMES[] = {
    { PANDA_EYES_NONE,  PANDA_ARMS_PEEK, PANDA_PROP_NONE, 0, 500 },
    { PANDA_EYES_NONE,  PANDA_ARMS_PEEK, PANDA_PROP_NONE, 0, 500 },
    { PANDA_EYES_HAPPY, PANDA_ARMS_REST, PANDA_PROP_NONE, 0, 450 },
    { PANDA_EYES_OPEN,  PANDA_ARMS_REST, PANDA_PROP_NONE, 0, 200 },
};

/* The gesture table the scheduler picks from at random. Every entry is a
 * positive, cute beat; the mix spans calm (wave, heart, wiggle) and playful
 * (bounce, peek-a-boo). */
#define GESTURE(arr) { #arr, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0])) }
static const buddy_gesture_t GESTURES[] = {
    GESTURE(WAVE_FRAMES),
    GESTURE(BOUNCE_FRAMES),
    GESTURE(HEART_FRAMES),
    GESTURE(WIGGLE_FRAMES),
    GESTURE(PEEK_FRAMES),
};
#undef GESTURE
#define GESTURE_COUNT (sizeof(GESTURES) / sizeof(GESTURES[0]))

/* --- App state --- */
/*
 * The scheduler is a two-phase machine: either playing a gesture (PLAYING) or
 * idling between gestures (IDLE). In PLAYING it advances through the active
 * gesture's keyframes; in IDLE it holds the resting pose, blinks on a
 * randomized interval, and waits a randomized pause before launching the next
 * gesture.
 */
typedef enum {
    BUDDY_PLAYING,
    BUDDY_IDLE,
} buddy_phase_t;

typedef struct {
    uint8_t *canvas_buf;
    lv_obj_t *canvas;

    lv_timer_t *tick;

    buddy_phase_t phase;

    /* PLAYING: the active gesture and where we are in it. */
    const buddy_gesture_t *gesture;
    uint8_t frame_index;
    uint32_t frame_start_tick;   /* baseline for the current keyframe's hold */

    /* IDLE: when the pause ends and the next gesture launches, and the blink
     * schedule within the pause. */
    uint32_t idle_start_tick;
    uint32_t idle_pause_ms;
    uint32_t next_blink_tick;
    bool blinking;

    /* The pose currently shown, kept so a tick that changes nothing skips the
     * recomposite-and-invalidate entirely. */
    panda_pose_t shown;
    bool has_shown;
} buddy_app_state_t;

static buddy_app_state_t s_state;

/* Uniformly random uint32 in [min, max], using the hardware RNG. */
static uint32_t rand_range(uint32_t min, uint32_t max)
{
    if (max <= min) {
        return min;
    }
    return min + (esp_random() % (max - min + 1));
}

/*
 * Render a pose only if it differs from what is already on screen. The pose is
 * small plain data, so a field-by-field compare is cheap and saves the much
 * costlier recomposite and panel flush when a keyframe is being held across
 * several ticks. The backdrop color is constant and applied here.
 */
static void show_pose(panda_eyes_t eyes, panda_arms_t arms, panda_prop_t prop,
                      int32_t y_offset)
{
    panda_pose_t pose = {
        .eyes = eyes,
        .arms = arms,
        .prop = prop,
        .y_offset = y_offset,
        .bg = BUDDY_BG_COLOR,
    };

    if (s_state.has_shown &&
        s_state.shown.eyes == pose.eyes &&
        s_state.shown.arms == pose.arms &&
        s_state.shown.prop == pose.prop &&
        s_state.shown.y_offset == pose.y_offset) {
        return;
    }

    panda_sprite_render(s_state.canvas, &pose);
    lv_obj_invalidate(s_state.canvas);
    s_state.shown = pose;
    s_state.has_shown = true;
}

/* Enter the idle phase: show the resting pose and schedule both the next blink
 * and the end of the idle pause, each at a randomized time. */
static void enter_idle(void)
{
    s_state.phase = BUDDY_IDLE;
    s_state.idle_start_tick = lv_tick_get();
    s_state.idle_pause_ms = rand_range(BUDDY_IDLE_PAUSE_MIN_MS,
                                       BUDDY_IDLE_PAUSE_MAX_MS);
    s_state.next_blink_tick = lv_tick_get() +
        rand_range(BUDDY_IDLE_BLINK_MIN_MS, BUDDY_IDLE_BLINK_MAX_MS);
    s_state.blinking = false;
    show_pose(IDLE_POSE.eyes, IDLE_POSE.arms, IDLE_POSE.prop, IDLE_POSE.y_offset);
}

/* Enter the playing phase with a specific gesture: reset to its first keyframe
 * and start its hold clock. */
static void start_gesture(const buddy_gesture_t *gesture)
{
    s_state.phase = BUDDY_PLAYING;
    s_state.gesture = gesture;
    s_state.frame_index = 0;
    s_state.frame_start_tick = lv_tick_get();

    const buddy_keyframe_t *f = &gesture->frames[0];
    show_pose(f->eyes, f->arms, f->prop, f->y_offset);
}

/* Launch a randomly chosen gesture. Used by both the idle auto-advance and the
 * tap handler. */
static void start_random_gesture(void)
{
    uint32_t i = esp_random() % GESTURE_COUNT;
    start_gesture(&GESTURES[i]);
}

/* Advance the playing gesture: when the current keyframe's hold has elapsed,
 * step to the next; at the end of the sequence, fall back to idle. */
static void tick_playing(void)
{
    const buddy_gesture_t *g = s_state.gesture;
    const buddy_keyframe_t *f = &g->frames[s_state.frame_index];

    if (lv_tick_elaps(s_state.frame_start_tick) < f->hold_ms) {
        return;
    }

    s_state.frame_index++;
    if (s_state.frame_index >= g->frame_count) {
        enter_idle();
        return;
    }

    s_state.frame_start_tick = lv_tick_get();
    f = &g->frames[s_state.frame_index];
    show_pose(f->eyes, f->arms, f->prop, f->y_offset);
}

/* Run the idle phase: blink on schedule, and when the pause is over launch the
 * next random gesture. The blink is a brief closed-eyes flash that returns to
 * open eyes, layered over the otherwise-static resting pose. */
static void tick_idle(void)
{
    uint32_t now = lv_tick_get();

    /* Blink handling: a closed-eyes flash, then reopen and reschedule. */
    if (s_state.blinking) {
        /* Hold the closed eyes briefly, then reopen. */
        if (lv_tick_elaps(s_state.next_blink_tick) >= 120) {
            s_state.blinking = false;
            s_state.next_blink_tick = now +
                rand_range(BUDDY_IDLE_BLINK_MIN_MS, BUDDY_IDLE_BLINK_MAX_MS);
            show_pose(PANDA_EYES_OPEN, PANDA_ARMS_REST, PANDA_PROP_NONE, 0);
        }
    } else if ((int32_t)(now - s_state.next_blink_tick) >= 0) {
        s_state.blinking = true;
        s_state.next_blink_tick = now;  /* reuse as the blink-start baseline */
        show_pose(PANDA_EYES_CLOSED, PANDA_ARMS_REST, PANDA_PROP_NONE, 0);
    }

    /* End of the idle pause: launch the next gesture. */
    if (lv_tick_elaps(s_state.idle_start_tick) >= s_state.idle_pause_ms) {
        start_random_gesture();
    }
}

static void buddy_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_state.canvas == NULL) {
        return;
    }
    if (s_state.phase == BUDDY_PLAYING) {
        tick_playing();
    } else {
        tick_idle();
    }
}

/*
 * Canvas tap handler. A press interrupts whatever the buddy is doing and plays
 * a random gesture at once, so the panda feels responsive to touch. Uses the
 * same press-event mechanism as the touch app's canvas.
 */
static void canvas_pressed_cb(lv_event_t *event)
{
    (void)event;
    start_random_gesture();
}

static void buddy_app_run(const app_api_t *api, void *ctx)
{
    lv_obj_t *root = api->root(ctx);
    api->set_softkeys(ctx, "Menu", "Back");

    panda_sprite_init();

    /* Size the canvas to the root's real pixel area, measured after a layout
     * pass, so it fills the usable screen above the softkey band. */
    lv_obj_update_layout(root);
    int32_t cw = lv_obj_get_content_width(root);
    int32_t ch = lv_obj_get_content_height(root);
    if (cw <= 0 || ch <= 0) {
        api->label(ctx, root, "Bad geometry");
        return;
    }

    const size_t buf_bytes = LV_CANVAS_BUF_SIZE(cw, ch, 16,
                                                LV_DRAW_BUF_STRIDE_ALIGN);
    s_state.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (s_state.canvas_buf == NULL) {
        api->label(ctx, root, "Out of memory");
        return;
    }

    s_state.canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_state.canvas, s_state.canvas_buf, cw, ch,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_state.canvas, LV_ALIGN_TOP_MID, 0, 0);
    lv_canvas_fill_bg(s_state.canvas, BUDDY_BG_COLOR, LV_OPA_COVER);

    /* Tap-to-trigger: the canvas captures presses and plays a random gesture. */
    lv_obj_add_flag(s_state.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_state.canvas, canvas_pressed_cb, LV_EVENT_PRESSED, NULL);

    /* Start in idle with the resting pose shown, then register the driver
     * timer; the app lives through it after run() returns. */
    s_state.has_shown = false;
    enter_idle();
    s_state.tick = lv_timer_create(buddy_tick, BUDDY_TICK_PERIOD_MS, NULL);
}

static void buddy_app_cleanup(const app_api_t *api, void *ctx)
{
    (void)api;
    (void)ctx;
    if (s_state.tick) {
        lv_timer_delete(s_state.tick);
        s_state.tick = NULL;
    }
    /* The canvas widget is freed when the shell destroys the app screen; only
     * the app-owned backing buffer is released here. */
    if (s_state.canvas_buf) {
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = NULL;
    }
    s_state.canvas = NULL;
    s_state.has_shown = false;
}

static const app_descriptor_t buddy_app = {
    .name = "Buddy",
    .icon = &icon_buddy_dsc,
    .run = buddy_app_run,
    .cleanup = buddy_app_cleanup,
};

REGISTER_APP(buddy_app);