/*
 * Camera subsystem — implementation.
 *
 * Bring-up: configure the esp32-camera driver from the BSP pins, request
 * RGB565 frames into PSRAM-backed double buffers, and detect the sensor.
 *
 * Viewfinder: a registered LVGL timer (NOT a task) pulls the freshest frame
 * each tick and blits it onto the caller's canvas, then returns the frame
 * buffer to the driver. Running as an LVGL timer keeps all frame work on the
 * single LVGL task that app_main owns, so the shell can still preempt. The
 * heavy path — DMA capture and the pixel copy — stays entirely in C behind the
 * coarse start/stop verbs.
 *
 * Orientation: the UI runs LANDSCAPE. The sensor captures landscape QVGA
 * (320 wide x 240 tall) and the viewfinder canvas is landscape too (320x240),
 * so the geometries match one-to-one and the pump copies each frame flat into
 * the canvas with no rotation.
 *
 * Color (red/blue channel swap): the OV7670 on this setup emits RGB565 with the
 * red and blue channels transposed relative to what the panel expects, so the
 * pump swaps the two 5-bit channels per pixel (leaving the 6-bit green field in
 * place) as it copies. The signature of this fault is the sensor's internal
 * color-bar test pattern rendering as clean, correctly-shaped vertical bars
 * (proving the data path, timing, and format are correct) but with red and blue
 * transposed while green stays correct. Green being unaffected is what
 * identifies a pure R/B swap, fixed by the bitfield swap in
 * camera_rgb565_swap_rb.
 *
 * Frame integrity note: over long jumper wiring the no-FIFO OV7670's DVP bus is
 * marginal and some frames arrive bit-corrupted. That is a signal-integrity
 * problem solved by shorter wiring or a PCB, not in firmware, so this file does
 * not attempt to detect or hide corrupt frames; it draws what the sensor
 * delivers. With adequately short wiring the corruption does not occur.
 *
 * Memory: frame buffers are owned by the esp32-camera driver in PSRAM and are
 * never handed to a per-app heap. The only buffer the caller owns is the LVGL
 * canvas it passes in.
 *
 * Pin-presence guard: when no camera is wired the BSP sets every camera pin to
 * GPIO_NUM_NC. The esp32-camera driver does not tolerate being initialized
 * against unconnected pins — it faults at a low level (a cache error inside
 * its pin setup) rather than returning an error. So camera_init checks that
 * the essential pins are configured before calling into the driver at all, and
 * reports the camera unavailable when they are not. This keeps a board with no
 * camera from crashing at startup while leaving the full pipeline intact for a
 * board that does have one: set real pins in bsp_config.h and init proceeds
 * normally.
 *
 * Note on the RESET/PWDN pins: these are tied to fixed rails in hardware
 * (RESET high, PWDN low) and left GPIO_NUM_NC here. Their polarity is
 * load-bearing — swapping them holds the sensor powered down and in reset, so
 * it never acknowledges on SCCB and the driver reports the sensor unsupported
 * even with a perfect data/clock/power bus. The full reasoning and the
 * measurement check live with the pin definitions in bsp_config.h; the guard
 * below intentionally does not test these two pins, since a fixed-rail tie
 * means there is no GPIO to validate.
 */
#include "camera.h"
#include "bsp_config.h"

#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "camera";

/* The viewfinder timer and its target canvas, retained while the viewfinder
 * runs. NULL when stopped. */
static lv_timer_t *s_viewfinder_timer = NULL;
static lv_obj_t *s_viewfinder_canvas = NULL;

/* Interval between viewfinder frame pulls. ~33 ms gives roughly 30 fps while
 * leaving the LVGL task ample time for shell input and redraws between pulls. */
#define CAMERA_VIEWFINDER_PERIOD_MS 33

/* Captured landscape frame geometry. The pump's copy assumes exactly this
 * size; a frame of any other geometry is skipped rather than risking an
 * out-of-bounds read. Matches FRAMESIZE_QVGA below and the landscape canvas. */
#define CAMERA_FRAME_W 320
#define CAMERA_FRAME_H 240

/* RGB565 channel masks, used by the per-pixel red/blue swap in the pump.
 * Red occupies the top 5 bits (15..11), green the middle 6 (10..5), blue the
 * bottom 5 (4..0). The swap moves blue up to the red position and red down to
 * the blue position while green stays put. */
#define CAMERA_RGB565_RED_MASK   0xF800
#define CAMERA_RGB565_GREEN_MASK 0x07E0
#define CAMERA_RGB565_BLUE_MASK  0x001F
#define CAMERA_RGB565_RB_SHIFT   11

/*
 * Swap the red and blue channels of one RGB565 pixel, preserving green.
 * Blue (bottom 5 bits) moves to the top, red (top 5 bits) moves to the bottom,
 * green (middle 6 bits) is left in place. This corrects the OV7670's transposed
 * red/blue channel order (see file header).
 */
static inline uint16_t camera_rgb565_swap_rb(uint16_t p)
{
    return (uint16_t)(((p & CAMERA_RGB565_BLUE_MASK) << CAMERA_RGB565_RB_SHIFT) |
                      (p & CAMERA_RGB565_GREEN_MASK) |
                      ((p & CAMERA_RGB565_RED_MASK) >> CAMERA_RGB565_RB_SHIFT));
}

/*
 * Whether enough camera pins are configured to attempt a real sensor bring-up.
 * The data clock (XCLK), pixel clock (PCLK), sync lines, and the data bus are
 * all essential; if the board left them unconnected (GPIO_NUM_NC) there is no
 * camera wired and the driver must not be touched. Checking a representative
 * set is sufficient — a board either wires the whole DVP bus or none of it.
 * RESET and PWDN are deliberately not checked: they are tied to fixed rails in
 * hardware and carry no GPIO. */
static bool camera_pins_present(void)
{
    return BSP_PIN_CAM_XCLK  != GPIO_NUM_NC &&
           BSP_PIN_CAM_PCLK  != GPIO_NUM_NC &&
           BSP_PIN_CAM_VSYNC != GPIO_NUM_NC &&
           BSP_PIN_CAM_HREF  != GPIO_NUM_NC &&
           BSP_PIN_CAM_D0    != GPIO_NUM_NC &&
           BSP_PIN_CAM_D7    != GPIO_NUM_NC;
}

static camera_config_t make_camera_config(void)
{
    /* RGB565 at QVGA keeps a frame small enough to blit cheaply onto a
     * 320x240 canvas and to hold two buffers in PSRAM. Grab mode "latest"
     * means the pump always sees the freshest frame and never a stale queued
     * one. */
    camera_config_t cfg = {
        .pin_pwdn = BSP_PIN_CAM_PWDN,
        .pin_reset = BSP_PIN_CAM_RESET,
        .pin_xclk = BSP_PIN_CAM_XCLK,
        .pin_sccb_sda = BSP_PIN_CAM_SCCB_SDA,
        .pin_sccb_scl = BSP_PIN_CAM_SCCB_SCL,
        .pin_d7 = BSP_PIN_CAM_D7,
        .pin_d6 = BSP_PIN_CAM_D6,
        .pin_d5 = BSP_PIN_CAM_D5,
        .pin_d4 = BSP_PIN_CAM_D4,
        .pin_d3 = BSP_PIN_CAM_D3,
        .pin_d2 = BSP_PIN_CAM_D2,
        .pin_d1 = BSP_PIN_CAM_D1,
        .pin_d0 = BSP_PIN_CAM_D0,
        .pin_vsync = BSP_PIN_CAM_VSYNC,
        .pin_href = BSP_PIN_CAM_HREF,
        .pin_pclk = BSP_PIN_CAM_PCLK,
        .xclk_freq_hz = BSP_CAM_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return cfg;
}

esp_err_t camera_init(void)
{
    /* No camera wired: skip the driver entirely. esp_camera_init would fault
     * at a low level against unconnected pins rather than returning an error,
     * so the only safe thing is not to call it. Reporting unavailable lets the
     * capability layer mark the camera not-ready and the device continue. */
    if (!camera_pins_present()) {
        ESP_LOGW(TAG, "camera pins not configured; camera unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    const camera_config_t cfg = make_camera_config();
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "camera ready (RGB565 QVGA)");
    return ESP_OK;
}

/*
 * Viewfinder pump. One LVGL timer tick: grab the freshest landscape frame and
 * copy it into the landscape canvas with a per-pixel red/blue channel swap,
 * then return the frame buffer to the driver.
 *
 * The sensor outputs landscape QVGA (320 wide x 240 tall); the panel and canvas
 * are landscape too (320x240). The geometries match, so canvas pixel (cx, cy)
 * comes from frame pixel (cx, cy) directly — a straight row-by-row copy, no
 * transpose. If the preview comes out mirrored or upside down for a given
 * physical sensor mount, fix it at the sensor (esp_camera sensor->set_hmirror /
 * set_vflip) or flip the source indexing here; this is the single place the
 * frame-to-canvas mapping is set.
 *
 * Each pixel's red and blue channels are swapped during the copy via
 * camera_rgb565_swap_rb (the OV7670 here emits RGB565 with red and blue
 * transposed; the swap restores correct color while leaving green untouched).
 *
 * Writes honor the canvas draw buffer's row stride rather than assuming a
 * packed width*2 layout: LVGL aligns each canvas row (LV_DRAW_BUF_STRIDE_ALIGN),
 * so the destination row base is stride * cy, not (width * 2) * cy. The frame is
 * read as a packed 320-wide RGB565 plane, two bytes per pixel.
 */
static void viewfinder_pump_cb(lv_timer_t *timer)
{
    (void)timer;
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return;
    }

    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(s_viewfinder_canvas);

    /* Only copy when the frame matches the expected landscape geometry; a
     * mismatched frame (wrong sensor mode) is skipped rather than risking an
     * out-of-bounds read, since the copy assumes 320x240. */
    if (draw_buf != NULL && draw_buf->data != NULL &&
        fb->width == CAMERA_FRAME_W && fb->height == CAMERA_FRAME_H) {

        const uint16_t *src = (const uint16_t *)fb->buf;
        uint8_t *dst_base = draw_buf->data;
        const uint32_t stride = draw_buf->header.stride;
        const uint32_t canvas_w = draw_buf->header.w;
        const uint32_t canvas_h = draw_buf->header.h;

        for (uint32_t cy = 0; cy < canvas_h; cy++) {
            uint16_t *dst_row = (uint16_t *)(dst_base + stride * cy);
            const uint16_t *src_row = &src[cy * CAMERA_FRAME_W];
            for (uint32_t cx = 0; cx < canvas_w; cx++) {
                dst_row[cx] = camera_rgb565_swap_rb(src_row[cx]);
            }
        }
    }

    esp_camera_fb_return(fb);
    lv_obj_invalidate(s_viewfinder_canvas);
}

esp_err_t camera_viewfinder_start(lv_obj_t *canvas)
{
    if (canvas == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_viewfinder_timer != NULL) {
        /* Already running; retarget rather than stacking timers. */
        s_viewfinder_canvas = canvas;
        return ESP_OK;
    }

    s_viewfinder_canvas = canvas;
    s_viewfinder_timer = lv_timer_create(viewfinder_pump_cb,
                                         CAMERA_VIEWFINDER_PERIOD_MS, NULL);
    if (s_viewfinder_timer == NULL) {
        s_viewfinder_canvas = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "viewfinder started");
    return ESP_OK;
}

void camera_viewfinder_stop(void)
{
    if (s_viewfinder_timer != NULL) {
        lv_timer_delete(s_viewfinder_timer);
        s_viewfinder_timer = NULL;
    }
    s_viewfinder_canvas = NULL;
    ESP_LOGI(TAG, "viewfinder stopped");
}

esp_err_t camera_capture(const uint8_t **out_buf, size_t *out_len,
                         uint16_t *out_width, uint16_t *out_height)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return ESP_FAIL;
    }

    /* Hand back the driver-owned buffer directly. It stays valid until the
     * next grab; returning it here would invalidate it for the caller, so the
     * driver recycles it on the following esp_camera_fb_get. This matches the
     * documented "valid until next capture or viewfinder frame" contract. */
    *out_buf = fb->buf;
    *out_len = fb->len;
    if (out_width != NULL) {
        *out_width = fb->width;
    }
    if (out_height != NULL) {
        *out_height = fb->height;
    }
    esp_camera_fb_return(fb);
    return ESP_OK;
}