/*
 * Board support package — public interface.
 *
 * The BSP brings up the display panel, the touch controller, and the LVGL
 * port glue, then hands back control. Crucially, it does NOT start a separate
 * LVGL task: the caller (app_main) owns the timer loop and drives LVGL by
 * calling bsp_lvgl_timer_handler() in its own loop. This is what lets the
 * shell preempt a running app.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the display, touch, and LVGL.
 *
 * On success LVGL is fully initialized with a registered display and input
 * device, a 1 ms tick source is running, and the panel backlight is on. The
 * function does not block and does not spawn a loop task; the caller must
 * pump LVGL via bsp_lvgl_timer_handler().
 */
esp_err_t bsp_init(void);

/*
 * Run one pass of LVGL's timer handler and return the number of milliseconds
 * the caller should wait before the next call. app_main calls this in a tight
 * loop, sleeping for the returned delay between passes. Because the caller
 * owns this loop, nothing else can hold it.
 */
uint32_t bsp_lvgl_timer_handler(void);

/*
 * Whether the backlight is under PWM control on this board. True once
 * bsp_init has bound BSP_PIN_LCD_BL to an LEDC channel; false when the board
 * config sets BSP_PIN_LCD_BL to GPIO_NUM_NC (the LED line is hardwired on and
 * not software-dimmable). The brightness capability gates itself on this so a
 * board with a fixed backlight reports the service unavailable rather than
 * pretending to dim.
 */
bool bsp_backlight_is_controllable(void);

/*
 * Set the backlight brightness as a percentage in the range 0..100, where 0 is
 * off (or the dimmest the panel allows) and 100 is full brightness. Values
 * outside the range are clamped. The percentage is converted to an LEDC duty
 * cycle against the configured duty resolution and applies immediately.
 *
 * Returns ESP_OK on success, or ESP_ERR_INVALID_STATE if the backlight is not
 * under PWM control on this board (see bsp_backlight_is_controllable). A board
 * with a hardwired backlight ignores the request and reports the error so the
 * caller can degrade gracefully.
 */
esp_err_t bsp_backlight_set_percent(uint8_t percent);

#ifdef __cplusplus
}
#endif