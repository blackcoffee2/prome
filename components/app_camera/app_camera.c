/*
 * Camera app.
 *
 * Written entirely against the curated app API. It builds a canvas and starts
 * the viewfinder through api->camera_viewfinder_start; the camera capability
 * runs the whole pipeline in C and blits frames onto the canvas. The app never
 * touches a frame, a pixel, or the sensor.
 *
 * If the camera capability is unavailable (no sensor, or the capability
 * excluded from the build), the app shows a message instead of a viewfinder.
 * On exit, cleanup stops the viewfinder; the shell then destroys the app's
 * screen, freeing the canvas.
 */
#include "app_descriptor.h"

#include "esp_heap_caps.h"

/* The launcher tile icon, compiled in from a build-time-converted image. */
extern const lv_image_dsc_t icon_camera_dsc;

/* Viewfinder canvas geometry. The panel runs LANDSCAPE (320x240), which matches
 * the sensor's native landscape QVGA capture (320x240) one-to-one, so the
 * canvas is landscape too and fills the screen edge to edge. No per-frame
 * rotation is needed: the camera capability blits the QVGA frame straight into
 * this canvas. The canvas is the full panel size: its top aligns to the top of
 * the app's root and its bottom runs behind the softkey band, so the live
 * preview is full-bleed with the Back bar sitting over its bottom edge. */
#define CAMERA_APP_W 320
#define CAMERA_APP_H 240

/* Per-app working state. The canvas buffer is owned by the app (the one
 * allocation the curated model leaves to the caller) and freed on cleanup. */
typedef struct {
    uint8_t *canvas_buf;
    lv_obj_t *canvas;
} camera_app_state_t;

static camera_app_state_t s_state;

static void camera_app_run(const app_api_t *api, void *ctx)
{
    lv_obj_t *root = api->root(ctx);
    api->set_softkeys(ctx, "Menu", "Back");

    if (!api->camera_available(ctx)) {
        api->label(ctx, root, "Camera unavailable");
        return;
    }

    const size_t buf_bytes = LV_CANVAS_BUF_SIZE(CAMERA_APP_W, CAMERA_APP_H,
                                                16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_state.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (s_state.canvas_buf == NULL) {
        api->label(ctx, root, "Out of memory");
        return;
    }

    s_state.canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_state.canvas, s_state.canvas_buf,
                         CAMERA_APP_W, CAMERA_APP_H, LV_COLOR_FORMAT_RGB565);
    /* Top-align rather than center: the canvas is taller than root (root loses
     * the softkey band's height), so centering would push the image off the
     * top and bottom. Aligning to the top starts the preview at the top of the
     * screen and lets the band overlap only the bottom strip. */
    lv_obj_align(s_state.canvas, LV_ALIGN_TOP_MID, 0, 0);

    api->camera_viewfinder_start(ctx, s_state.canvas);
}

static void camera_app_cleanup(const app_api_t *api, void *ctx)
{
    api->camera_viewfinder_stop(ctx);
    /* The canvas itself is freed when the shell destroys the app screen; only
     * the app-owned backing buffer is released here. */
    if (s_state.canvas_buf) {
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = NULL;
    }
    s_state.canvas = NULL;
}

static const app_descriptor_t camera_app = {
    .name = "Camera",
    .icon = &icon_camera_dsc,
    .run = camera_app_run,
    .cleanup = camera_app_cleanup,
};

REGISTER_APP(camera_app);