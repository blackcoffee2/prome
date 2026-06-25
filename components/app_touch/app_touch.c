/*
 * Touch calibration app.
 *
 * A diagnostic app for verifying and tuning the touch path. It fills the app
 * root with a black canvas and paints a white dot wherever the user presses,
 * so the operator can see at a glance whether a press lands where the finger
 * actually touched. Each press is also logged with its canvas-local pixel
 * coordinates, which are the values to compare against the physical touch
 * point when tuning the BSP_TOUCH_* calibration in bsp_config.h: if a press at
 * a known corner lands on the opposite side, flip a BSP_TOUCH_MIRROR_* flag;
 * if the axes are crossed, flip BSP_TOUCH_SWAP_XY; if the dot is offset but in
 * the right region, adjust the BSP_TOUCH_RAW_* bounds.
 *
 * Painted dots persist for the lifetime of the app screen, so a full sweep of
 * the panel builds up a visible coverage map. Exiting (the global Menu or Back
 * softkey) tears the screen down and frees the canvas.
 *
 * Drawing model: the app owns the canvas backing buffer (the one allocation
 * the curated model leaves to the caller), mirroring the camera app. The
 * canvas itself is created through LVGL directly, the same way app_camera and
 * app_clock reach LVGL for widgets and timers the curated API does not wrap.
 * A press event callback on the canvas reads the active pointer's coordinates,
 * translates them into canvas-local space, and paints a small square so a
 * single touch is visible rather than one hard-to-see pixel.
 *
 * Geometry: unlike the camera app, the canvas is sized to the app root's real
 * pixel dimensions (measured after a layout pass) rather than the full panel.
 * The root is shorter than the panel by the softkey band height, so deriving
 * the canvas size from the root keeps the painted coordinates aligned with
 * where the canvas actually sits on screen.
 */
#include "app_descriptor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/* The launcher tile icon, compiled in from a build-time-converted image. */
extern const lv_image_dsc_t icon_touch_dsc;

static const char *TAG = "app_touch";

/* Half-width of the painted square, in pixels. A press paints a
 * (2 * DOT_RADIUS + 1) square centered on the touch point, clamped to the
 * canvas bounds, so a touch is clearly visible. */
#define TOUCH_DOT_RADIUS 3

/* White and black in the canvas color format (RGB565). The canvas is filled
 * black on entry; presses paint white. */
#define TOUCH_COLOR_BLACK lv_color_black()
#define TOUCH_COLOR_WHITE lv_color_white()

/*
 * Per-app working state. The canvas backing buffer is owned by the app and
 * freed on cleanup; the canvas widget is freed when the shell destroys the
 * app screen. The cached width/height are the canvas dimensions, used to clamp
 * the painted square so it never writes outside the buffer.
 */
typedef struct {
    uint8_t *canvas_buf;
    lv_obj_t *canvas;
    int32_t canvas_w;
    int32_t canvas_h;
} touch_app_state_t;

static touch_app_state_t s_state;

/*
 * Paint a filled white square centered on a canvas-local point, clamped to the
 * canvas bounds. Writing pixel by pixel through lv_canvas_set_px keeps the dot
 * within whatever stride the canvas draw buffer uses, so this does not assume a
 * packed layout.
 */
static void paint_dot(int32_t cx, int32_t cy)
{
    int32_t x0 = cx - TOUCH_DOT_RADIUS;
    int32_t y0 = cy - TOUCH_DOT_RADIUS;
    int32_t x1 = cx + TOUCH_DOT_RADIUS;
    int32_t y1 = cy + TOUCH_DOT_RADIUS;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > s_state.canvas_w - 1) {
        x1 = s_state.canvas_w - 1;
    }
    if (y1 > s_state.canvas_h - 1) {
        y1 = s_state.canvas_h - 1;
    }

    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            lv_canvas_set_px(s_state.canvas, x, y, TOUCH_COLOR_WHITE, LV_OPA_COVER);
        }
    }
}

/*
 * Canvas press handler. Fires on LV_EVENT_PRESSED (the start of a touch). The
 * active input device carries the pointer coordinates in screen space; the
 * canvas's absolute position is subtracted to convert them into canvas-local
 * pixels before painting and logging. Logging on press only (not on every
 * move) keeps the log readable while still recording each distinct tap.
 */
static void canvas_pressed_cb(lv_event_t *event)
{
    (void)event;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_state.canvas == NULL) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    /* Translate the screen-space touch point into canvas-local coordinates by
     * subtracting the canvas's absolute top-left. With the canvas top-aligned
     * and full width this offset is typically (0, 0), but subtracting it keeps
     * the dot correct regardless of where the canvas is placed. */
    lv_area_t coords;
    lv_obj_get_coords(s_state.canvas, &coords);
    int32_t cx = point.x - coords.x1;
    int32_t cy = point.y - coords.y1;

    ESP_LOGI(TAG, "touch x=%d y=%d", (int)cx, (int)cy);

    if (cx >= 0 && cy >= 0 && cx < s_state.canvas_w && cy < s_state.canvas_h) {
        paint_dot(cx, cy);
        lv_obj_invalidate(s_state.canvas);
    }
}

static void touch_app_run(const app_api_t *api, void *ctx)
{
    lv_obj_t *root = api->root(ctx);
    api->set_softkeys(ctx, "Menu", "Back");

    /* Measure the root's real pixel size so the canvas matches the area it
     * occupies. The root is laid out by the shell before run() is called, but
     * a layout refresh makes the measured size reliable. */
    lv_obj_update_layout(root);
    s_state.canvas_w = lv_obj_get_content_width(root);
    s_state.canvas_h = lv_obj_get_content_height(root);
    if (s_state.canvas_w <= 0 || s_state.canvas_h <= 0) {
        api->label(ctx, root, "Bad geometry");
        return;
    }

    const size_t buf_bytes = LV_CANVAS_BUF_SIZE(s_state.canvas_w, s_state.canvas_h,
                                                16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_state.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (s_state.canvas_buf == NULL) {
        api->label(ctx, root, "Out of memory");
        return;
    }

    s_state.canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_state.canvas, s_state.canvas_buf,
                         s_state.canvas_w, s_state.canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_state.canvas, LV_ALIGN_TOP_MID, 0, 0);

    /* Start fully black; presses paint white over it. */
    lv_canvas_fill_bg(s_state.canvas, TOUCH_COLOR_BLACK, LV_OPA_COVER);

    /* Receive the start of each touch. The canvas is clickable so it captures
     * presses; the handler paints and logs. */
    lv_obj_add_flag(s_state.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_state.canvas, canvas_pressed_cb, LV_EVENT_PRESSED, NULL);
}

static void touch_app_cleanup(const app_api_t *api, void *ctx)
{
    (void)api;
    (void)ctx;
    /* The canvas widget is freed when the shell destroys the app screen; only
     * the app-owned backing buffer is released here. */
    if (s_state.canvas_buf) {
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = NULL;
    }
    s_state.canvas = NULL;
    s_state.canvas_w = 0;
    s_state.canvas_h = 0;
}

static const app_descriptor_t touch_app = {
    .name = "Touch",
    .icon = &icon_touch_dsc,
    .run = touch_app_run,
    .cleanup = touch_app_cleanup,
};

REGISTER_APP(touch_app);