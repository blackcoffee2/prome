/*
 * Camera subsystem — public interface.
 *
 * A standalone C subsystem whose surface is deliberately coarse and
 * verb-shaped, matching the capability API that wraps it: callers start and
 * stop a viewfinder and request a still, and never touch a pixel. All frame
 * handling — sensor DMA, buffer recycling, blitting — stays inside this
 * component, in C.
 *
 * The viewfinder does NOT run its own task or loop. It registers an LVGL timer
 * on the single LVGL task that app_main pumps, so the shell's loop ownership
 * and ability to preempt are preserved. Frame buffers live in C-owned PSRAM
 * inside the sensor driver, never on a per-app heap.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the camera sensor over its DVP interface using the pins in the
 * BSP config. Targets RGB565 frames so they blit straight to an LVGL canvas
 * with no decode. Safe to call once at startup; returns an error if no sensor
 * is detected so the caller can degrade gracefully.
 */
esp_err_t camera_init(void);

/*
 * Start the viewfinder onto the given LVGL canvas. The canvas must already
 * have a backing buffer sized for its dimensions. From this call until
 * camera_viewfinder_stop, a registered LVGL timer pulls the freshest frame and
 * blits it into the canvas. The caller owns the canvas and its buffer; the
 * camera owns the frame buffers behind the verb.
 */
esp_err_t camera_viewfinder_start(lv_obj_t *canvas);

/*
 * Stop the viewfinder. Deletes the LVGL timer and releases any frame held for
 * blitting. The canvas is left showing its last frame; the caller may clear or
 * destroy it. Idempotent.
 */
void camera_viewfinder_stop(void);

/*
 * Capture a single still. On success, out_buf points to a C-owned RGB565
 * buffer valid until the next capture or viewfinder frame; out_len is its byte
 * length, and width/height describe its geometry. The caller must not free it.
 * Intended as a coarse "request capture" verb; persistence/encoding is a
 * higher-layer concern handled later.
 */
esp_err_t camera_capture(const uint8_t **out_buf, size_t *out_len,
                         uint16_t *out_width, uint16_t *out_height);

#ifdef __cplusplus
}
#endif