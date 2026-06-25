/*
 * Board support package — implementation.
 *
 * Responsibilities, in order of bring-up:
 *   1. SPI bus + ILI9341 panel via esp_lcd over a 4-wire SPI interface.
 *   2. XPT2046 resistive touch on the same shared SPI bus, via esp_lcd_touch.
 *   3. LVGL: display, draw buffers in DMA-capable internal RAM, flush callback
 *      wired to the panel, an input device wired to the touch controller, and
 *      a 1 ms tick fed from an esp_timer.
 *   4. Backlight: BSP_PIN_LCD_BL driven by an LEDC PWM channel so brightness is
 *      continuously adjustable through bsp_backlight_set_percent.
 *
 * What this file deliberately does NOT do: it never creates a task that runs
 * lv_timer_handler. The timer loop belongs to app_main. esp_lvgl_port is not
 * used for the same reason. The only background timer here is the LVGL tick,
 * which merely advances LVGL's notion of elapsed time and touches no widgets.
 *
 * Panel interface: the target board (KMRTM32032-SPI) is an ILI9341 on a 4-wire
 * SPI bus, driven through the ESP32-S3 hardware SPI2 peripheral via esp_lcd.
 * The display and the XPT2046 touch controller share the SCK/MOSI/MISO lines
 * and are distinguished by separate chip selects. Only the panel-IO layer
 * differs from a parallel panel; everything above it (the flush callback, draw
 * buffers, LVGL display, tick, and touch input) is interface agnostic because
 * esp_lcd hides the bus behind one panel handle.
 *
 * Orientation: the UI runs landscape (320x240). The panel is natively 240x320
 * portrait; bsp_panel_init applies swap-xy and mirror flags from the board
 * config so the controller presents a landscape image, and the touch read
 * callback maps raw ADC coordinates into the same landscape frame.
 *
 * Color byte order: RGB565 pixels are byte-swapped in software in the flush
 * callback before being handed to the panel. On this esp_lcd/ILI9341 version
 * the panel driver's data_endian field has no observable effect, so the swap
 * that the SPI ILI9341 needs is performed by LVGL's lv_draw_sw_rgb565_swap on
 * the partial buffer. This is the documented LVGL 9 approach for SPI displays
 * whose bytes arrive in the wrong order, and it is valid here because the
 * display renders in PARTIAL mode (each flush buffer starts at index 0). See
 * lvgl_flush_cb.
 */
#include "bsp.h"
#include "bsp_config.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "bsp";

/*
 * RGB565 byte order transmitted to the panel, used below in the panel device
 * config's data_endian field. This is the SPI-side equivalent of the parallel
 * build's swap_color_bytes IO flag, which does not exist on the SPI panel-IO
 * config; the byte order is instead set on the panel driver.
 *
 * The value is normally provided by bsp_config.h as BSP_LCD_DATA_ENDIAN. The
 * guarded fallback below makes this translation unit self-sufficient: if the
 * board config does not define the macro, bsp.c supplies a default rather than
 * failing to compile. A board config that does define BSP_LCD_DATA_ENDIAN
 * overrides this default.
 *
 * On the ILI9341/esp_lcd version in use, this field has no effect on the byte
 * order reaching the panel — the necessary swap is performed in software in
 * lvgl_flush_cb via lv_draw_sw_rgb565_swap. The field is still set for
 * correctness and for stacks where it is honored.
 */
#ifndef BSP_LCD_DATA_ENDIAN
#define BSP_LCD_DATA_ENDIAN LCD_RGB_DATA_ENDIAN_BIG
#endif

/* Handles retained for the lifetime of the device. */
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;

/* The panel IO handle, retained from bsp_panel_init so bsp_lvgl_init can bind
 * the flush-ready callback context to the display once it is created. */
static esp_lcd_panel_io_handle_t s_panel_io = NULL;

/* Whether the backlight was bound to an LEDC PWM channel at init. False when
 * the board config sets BSP_PIN_LCD_BL to GPIO_NUM_NC (backlight hardwired
 * on), in which case the brightness API reports the service uncontrollable. */
static bool s_backlight_pwm = false;

/* Draw buffers in internal DMA-capable RAM. Two buffers enable double-buffered
 * flushing. Allocated at init from the internal-RAM budget. */
static lv_color_t *s_draw_buf_a = NULL;
static lv_color_t *s_draw_buf_b = NULL;

/*
 * LVGL flush callback. esp_lcd performs the actual SPI transfer; LVGL hands us
 * a rectangle of rendered pixels and we blit it. The bitmap draw is
 * asynchronous, so completion is reported back to LVGL from the panel IO
 * "color transfer done" callback below.
 *
 * Before blitting, the RGB565 pixels are byte-swapped in place with
 * lv_draw_sw_rgb565_swap. The SPI ILI9341 expects the two bytes of each 16-bit
 * pixel in the opposite order to how LVGL lays them out in the draw buffer; on
 * this esp_lcd/ILI9341 version the panel driver's data_endian field does not
 * apply that swap, so it is done here in software. Without the swap, text and
 * shapes render correctly but solid color fills break into vertical color
 * bands; the swap is independent of SPI clock rate.
 *
 * The pixel count passed to the swap is the area's width times its height. The
 * swap is safe in this configuration because the display renders in PARTIAL
 * mode, so each flush receives a self-contained buffer starting at index 0
 * (lv_draw_sw_rgb565_swap must not be used this way in DIRECT mode, where the
 * buffer is windowed into a full-screen frame).
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2 + 1;
    const int y2 = area->y2 + 1;

    const uint32_t px_count = (uint32_t)(x2 - x1) * (uint32_t)(y2 - y1);
    lv_draw_sw_rgb565_swap(px_map, px_count);

    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, px_map);
}

/*
 * Panel IO completion callback. Fires when a bitmap transfer finishes, letting
 * LVGL release the buffer and proceed. Runs in ISR context, so it only calls
 * the lightweight lv_display_flush_ready. This is the SPI panel-io event
 * signature (esp_lcd_panel_io_event_data_t), shared across esp_lcd IO
 * backends.
 */
static bool panel_io_color_done_cb(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx)
{
    lv_display_t *disp = user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

/*
 * Raw-ADC-to-pixel mapping for the XPT2046, invoked by the touch driver when
 * its built-in ADC-to-coordinate conversion is disabled (the
 * XPT2046_CONVERT_ADC_TO_COORDS Kconfig option is off). The driver fills the
 * x/y arrays with raw 12-bit ADC readings; this callback rescales them into
 * panel pixels using the calibration bounds from the board config, then clamps
 * so a reading slightly outside the calibrated range cannot produce an
 * off-panel coordinate.
 *
 * The signature matches esp_lcd_touch_config_t::process_coordinates exactly:
 * the touch handle is the first parameter, followed by the coordinate, the
 * strength/pressure, and point-count arrays. The handle is unused here because
 * the calibration bounds come from the board config rather than from per-handle
 * state, but the parameter must be present so the function pointer type agrees
 * with what the driver expects.
 *
 * Axes here are the controller's NATIVE (pre-swap) axes. The esp_lcd_touch
 * base calls this callback FIRST, on the raw readings, then applies swap_xy and
 * mirror_x/mirror_y AFTER it returns. So the native X axis spans the panel's
 * short dimension (BSP_LCD_V_RES) and the native Y axis spans the long
 * dimension (BSP_LCD_H_RES). Mapping against these fixed dimensions is what
 * lets the BSP_TOUCH_RAW_* bounds stay constant when the landscape swap is
 * toggled.
 *
 * This callback carries both the SCALE and the DIRECTION of each axis, because
 * direction cannot be corrected with the driver's mirror flags on this board:
 * the base applies mirror_x as (x_max - x) and mirror_y as (y_max - y) using
 * the POST-swap x_max/y_max (H_RES, V_RES), which do not match the PRE-swap
 * per-axis spans this callback maps against (V_RES for native X, H_RES for
 * native Y). A mirror flag would therefore subtract from the wrong-axis maximum
 * and offset the point rather than cleanly flip it. Direction is instead set by
 * the ordering of BSP_TOUCH_RAW_*_MIN vs _MAX: when MIN > MAX the span below
 * goes negative and the axis maps descending, inverting it against the correct
 * native span. The subtraction, the negative-span division, and the clamp below
 * all stay correct for a negative span.
 *
 * Z (pressure) is passed through untouched; the driver still uses it for the
 * press/release threshold.
 */
static void touch_process_coordinates(esp_lcd_touch_handle_t tp,
                                      uint16_t *x, uint16_t *y, uint16_t *z,
                                      uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)z;
    if (point_num == NULL) {
        return;
    }
    uint8_t count = *point_num;
    if (count > max_point_num) {
        count = max_point_num;
    }

    for (uint8_t i = 0; i < count; i++) {
        int32_t raw_x = x[i];
        int32_t raw_y = y[i];

        /* Map raw X across the native short axis (panel V_RES). */
        const int32_t span_x = BSP_TOUCH_RAW_X_MAX - BSP_TOUCH_RAW_X_MIN;
        int32_t px = (span_x != 0)
                         ? ((raw_x - BSP_TOUCH_RAW_X_MIN) * (BSP_LCD_V_RES - 1)) / span_x
                         : 0;

        /* Map raw Y across the native long axis (panel H_RES). */
        const int32_t span_y = BSP_TOUCH_RAW_Y_MAX - BSP_TOUCH_RAW_Y_MIN;
        int32_t py = (span_y != 0)
                         ? ((raw_y - BSP_TOUCH_RAW_Y_MIN) * (BSP_LCD_H_RES - 1)) / span_y
                         : 0;

        /* Clamp into the panel so out-of-range raw values stay on-screen. */
        if (px < 0) {
            px = 0;
        } else if (px > BSP_LCD_V_RES - 1) {
            px = BSP_LCD_V_RES - 1;
        }
        if (py < 0) {
            py = 0;
        } else if (py > BSP_LCD_H_RES - 1) {
            py = BSP_LCD_H_RES - 1;
        }

        x[i] = (uint16_t)px;
        y[i] = (uint16_t)py;
    }
}

/*
 * LVGL input read callback. Polls the XPT2046 for the latest point. The touch
 * driver is read here (not in an ISR) so the read stays on the LVGL task,
 * preserving the single-task discipline. The driver applies the raw-to-pixel
 * mapping (touch_process_coordinates) and the swap/mirror flags configured at
 * init, so the coordinates arrive already in the landscape pixel frame.
 *
 * esp_lcd_touch_get_coordinates carries a deprecation notice in the touch base
 * component: it is slated for removal in esp_lcd_touch 2.0.0 in favor of
 * esp_lcd_touch_get_data. It remains the documented, functional read path in
 * the currently installed component version and in the XPT2046 driver's own
 * LVGL example, so it is kept here and the deprecation warning is suppressed
 * locally. Revisit when upgrading the touch base component to 2.0.0.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = lv_indev_get_user_data(indev);
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t count = 0;

    esp_lcd_touch_read_data(touch);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    bool pressed = esp_lcd_touch_get_coordinates(touch, &x, &y, NULL, &count, 1);
#pragma GCC diagnostic pop

    if (pressed && count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*
 * LVGL tick source. esp_timer fires every millisecond and advances LVGL's
 * clock. This is the only LVGL-related background activity; it performs no
 * rendering and does not run the timer handler.
 */
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1);
}

/*
 * Bring up the backlight as an LEDC PWM output.
 *
 * The backlight pin is driven by a hardware LEDC channel rather than a plain
 * on/off GPIO so brightness is continuously adjustable. A timer is configured
 * at the backlight frequency and duty resolution from the board config, and a
 * channel is bound to BSP_PIN_LCD_BL on that timer. The channel starts at full
 * duty so the panel comes up at full brightness; a higher layer (the brightness
 * capability, restoring a persisted preference) may dim it afterward.
 *
 * When the board's LED line is hardwired on, BSP_PIN_LCD_BL is NC and there is
 * nothing to drive: the function leaves s_backlight_pwm false and returns OK so
 * the rest of bring-up proceeds. The brightness capability then reports the
 * service unavailable, since there is no PWM channel to adjust.
 */
static esp_err_t bsp_backlight_init(void)
{
    if (BSP_PIN_LCD_BL == GPIO_NUM_NC) {
        ESP_LOGI(TAG, "backlight hardwired (no PWM control)");
        s_backlight_pwm = false;
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BSP_BL_LEDC_MODE,
        .timer_num = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES_BITS,
        .freq_hz = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight timer config failed (%s); backlight uncontrolled",
                 esp_err_to_name(err));
        s_backlight_pwm = false;
        return ESP_OK;
    }

    /* Start at full duty so the panel comes up at full brightness. On an
     * active-high backlight that is the maximum duty; an active-low panel would
     * invert this, matching the inversion in bsp_backlight_set_percent. */
    const uint32_t start_duty = (BSP_LCD_BL_ON_LEVEL != 0)
                                    ? BSP_BL_LEDC_DUTY_MAX : 0;
    const ledc_channel_config_t channel_cfg = {
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel = BSP_BL_LEDC_CHANNEL,
        .timer_sel = BSP_BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BSP_PIN_LCD_BL,
        .duty = start_duty,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight channel config failed (%s); backlight uncontrolled",
                 esp_err_to_name(err));
        s_backlight_pwm = false;
        return ESP_OK;
    }

    s_backlight_pwm = true;
    ESP_LOGI(TAG, "backlight on LEDC PWM (channel %d, %d Hz)",
             (int)BSP_BL_LEDC_CHANNEL, (int)BSP_BL_LEDC_FREQ_HZ);
    return ESP_OK;
}

bool bsp_backlight_is_controllable(void)
{
    return s_backlight_pwm;
}

esp_err_t bsp_backlight_set_percent(uint8_t percent)
{
    if (!s_backlight_pwm) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }

    /* Scale the percentage to the configured duty resolution. The rounding
     * (+50 before dividing by 100) keeps the endpoints exact: 0% maps to duty
     * 0 and 100% maps to the full-scale duty. */
    uint32_t duty = ((uint32_t)percent * BSP_BL_LEDC_DUTY_MAX + 50) / 100;

    /* Invert for an active-low backlight so a higher percent is always
     * brighter regardless of panel polarity. On this board the LED line is
     * active-high (BSP_LCD_BL_ON_LEVEL == 1), so no inversion is applied. */
    if (BSP_LCD_BL_ON_LEVEL == 0) {
        duty = BSP_BL_LEDC_DUTY_MAX - duty;
    }

    esp_err_t err = ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

/*
 * Initialize the shared SPI bus. Both the display and the touch controller
 * attach to this one bus; each adds its own device with its own chip select.
 * The bus is sized for the largest display transfer (one full draw buffer);
 * the touch controller's transfers are tiny by comparison. MISO is included
 * because the touch controller returns its readings on it (the display is
 * write-only, but the bus must carry MISO for touch).
 */
static esp_err_t bsp_spi_bus_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = BSP_PIN_SPI_SCK,
        .mosi_io_num = BSP_PIN_SPI_MOSI,
        .miso_io_num = BSP_PIN_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_MAX_TRANSFER_BYTES,
    };
    return spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
}

static esp_err_t bsp_panel_init(void)
{
    /* Panel IO device on the shared SPI bus. dc_gpio_num is the data/command
     * select; the panel clock is the SPI clock from the board config. The SPI
     * panel-IO flags struct has no color byte-swap field; RGB565 byte order for
     * SPI panels is set on the panel driver config via its data_endian field
     * (see panel_cfg below). On this driver version that field is not honored,
     * so the swap is done in software in lvgl_flush_cb. */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_PIN_LCD_CS,
        .dc_gpio_num = BSP_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = BSP_LCD_CMD_BITS,
        .lcd_param_bits = BSP_LCD_PARAM_BITS,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &io));

    /* ILI9341 controller driver on top of the IO layer. data_endian sets the
     * RGB565 byte order transmitted to the panel. The value comes from
     * BSP_LCD_DATA_ENDIAN (provided by bsp_config.h, with a fallback defined at
     * the top of this file). On this driver version it has no observable
     * effect, so the actual byte swap is performed in software in
     * lvgl_flush_cb; this field is left set for stacks where it is honored. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = BSP_LCD_DATA_ENDIAN,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, BSP_LCD_INVERT_COLOR));
    /* swap_xy is what rotates the native portrait panel into landscape; the
     * mirror flags then orient the landscape image for the board mounting. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, BSP_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BSP_LCD_MIRROR_X, BSP_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Retain the IO handle so the LVGL init step can register the display as
     * the transfer-complete user context. */
    s_panel_io = io;
    return ESP_OK;
}

static esp_err_t bsp_touch_init(void)
{
    /* The XPT2046 attaches to the shared SPI bus as its own device, selected
     * by its own chip select. The touch panel IO carries the controller's low
     * clock (a few MHz), independent of the panel's much faster clock on the
     * same bus.
     *
     * Touch failures are non-fatal: each step returns its error rather than
     * aborting, so a board with no touch controller (or a non-XPT2046 one)
     * leaves s_touch NULL and the device still comes up. The display does not
     * depend on touch. */
    const esp_lcd_panel_io_spi_config_t touch_io_cfg =
        ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(BSP_PIN_TOUCH_CS);
    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &touch_io_cfg, &touch_io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch IO init failed (%s); continuing without touch",
                 esp_err_to_name(err));
        return err;
    }

    /* The touch resolution matches the panel geometry so coordinates map onto
     * the full screen. The swap/mirror flags come from the BSP_TOUCH_* macros,
     * which on this board alias the display's BSP_LCD_* flags one-to-one: the
     * touch glass is the same physical surface as the panel, so the orientation
     * transform that lands the image right-side up also lands a touch under the
     * finger. The XPT2046 returns raw 12-bit ADC values; with the driver's
     * built-in conversion disabled (XPT2046_CONVERT_ADC_TO_COORDS off),
     * touch_process_coordinates performs the raw-to-pixel mapping using the
     * calibration bounds from the board config, and the driver then applies
     * these swap/mirror flags.
     *
     * Axis INVERSION is not corrected by these flags but by the ordering of the
     * BSP_TOUCH_RAW_*_MIN/_MAX bounds: the base component applies the mirror
     * flags against the post-swap x_max/y_max, which do not match the pre-swap
     * per-axis spans the mapping uses, so a mirror flag would offset rather than
     * flip. See the bounds and touch_process_coordinates for the full
     * reasoning.
     *
     * The INT (pen IRQ) line is wired so the driver can use it when available;
     * the read path polls regardless. A probe failure (no controller on the
     * bus) is reported and swallowed so the display still comes up. */
    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BSP_PIN_TOUCH_IRQ,
        .flags = {
            .swap_xy = BSP_TOUCH_SWAP_XY,
            .mirror_x = BSP_TOUCH_MIRROR_X,
            .mirror_y = BSP_TOUCH_MIRROR_Y,
        },
        .process_coordinates = touch_process_coordinates,
    };
    err = esp_lcd_touch_new_spi_xpt2046(touch_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not found (%s); continuing without touch",
                 esp_err_to_name(err));
        s_touch = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t bsp_lvgl_init(void)
{
    lv_init();

    /* Draw buffers in internal DMA-capable RAM, drawn from the internal-RAM
     * budget. PSRAM is intentionally not used for these because the SPI DMA
     * path performs best from internal memory. */
    const size_t buf_bytes = BSP_LCD_DRAW_BUF_PIXELS * sizeof(lv_color_t);
    s_draw_buf_a = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_draw_buf_b = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_draw_buf_a == NULL || s_draw_buf_b == NULL) {
        ESP_LOGE(TAG, "draw buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    /* The display is created at the landscape resolution from the board
     * config (320x240); the panel controller already presents a landscape
     * image via swap-xy, so LVGL renders straight into landscape. */
    s_display = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_display_set_user_data(s_display, s_panel);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    lv_display_set_buffers(s_display, s_draw_buf_a, s_draw_buf_b, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* The SPI transfer-complete callback was registered in the IO config; bind
     * its user context to the display now that the display exists, so
     * lv_display_flush_ready receives the right handle. */
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = panel_io_color_done_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(s_panel_io, &io_cbs, s_display);

    /* Touch input device on the same task as rendering. Registered only when
     * a touch controller was actually found; without it the display still
     * runs, just with no pointer input. */
    if (s_touch != NULL) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(s_indev, s_touch);
        lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
    }

    /* 1 ms tick. Only advances LVGL's clock; runs no widget logic. */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000));

    return ESP_OK;
}

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "bringing up display, touch, and LVGL");

    /* The shared SPI bus must exist before either device attaches to it. */
    ESP_ERROR_CHECK(bsp_spi_bus_init());

    ESP_ERROR_CHECK(bsp_panel_init());

    /* Touch is optional: a board with no touch controller (or a non-XPT2046
     * one) logs a warning and the device continues with display only. The
     * display path does not depend on touch, so its failure is not fatal. */
    if (bsp_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without touch input");
    }

    ESP_ERROR_CHECK(bsp_lvgl_init());
    ESP_ERROR_CHECK(bsp_backlight_init());

    ESP_LOGI(TAG, "bsp ready");
    return ESP_OK;
}

uint32_t bsp_lvgl_timer_handler(void)
{
    /* One LVGL pass. The return value is how long the caller may idle before
     * the next pass; LVGL computes it from its pending timers and animations. */
    return lv_timer_handler();
}