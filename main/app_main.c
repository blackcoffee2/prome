/*
 * Entry point.
 *
 * app_main owns the one thing it must never delegate: the LVGL timer loop.
 * After bringing up the BSP (display, touch, LVGL) and starting the shell
 * (services, launcher), it runs lv_timer_handler forever in its own loop,
 * sleeping for the interval LVGL asks for between passes.
 *
 * This loop ownership is the load-bearing runtime decision of the whole
 * firmware. Because the single highest-level function holds the loop — not the
 * BSP, not the shell, not an app — nothing below it can capture control: an app
 * builds its UI and returns, the shell hosts apps without running them, and the
 * BSP exposes a one-pass handler rather than spinning its own task. Everything
 * cooperative about the system follows from the loop living here.
 *
 * There is exactly one LVGL task (this one). All rendering, input, timers, and
 * callbacks run on it, so no LVGL locking is needed anywhere in the codebase.
 */
#include "bsp.h"
#include "shell.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

/* Upper bound on idle time between LVGL passes. lv_timer_handler returns how
 * long it is safe to wait, but capping it keeps input latency low even when
 * LVGL reports no imminent work. */
#define LVGL_MAX_IDLE_MS 16

void app_main(void)
{
    ESP_LOGI(TAG, "PROME starting");

    /* Display, touch, and LVGL. After this returns LVGL is initialized but not
     * yet being pumped; nothing renders until the loop below runs. */
    ESP_ERROR_CHECK(bsp_init());

    /* Services and the launcher. The shell builds UI on the LVGL objects the
     * BSP created and returns; it does not loop. */
    shell_start();

    /* The timer loop. Owned here and nowhere else. Each pass runs one round of
     * LVGL work and returns the milliseconds until the next pass is due; we
     * sleep that long so the loop yields the CPU between passes rather than
     * busy-spinning. The delay is clamped to at least one tick so the loop
     * always yields. */
    for (;;) {
        uint32_t delay_ms = bsp_lvgl_timer_handler();
        if (delay_ms > LVGL_MAX_IDLE_MS) {
            delay_ms = LVGL_MAX_IDLE_MS;
        }
        if (delay_ms < 1) {
            delay_ms = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}