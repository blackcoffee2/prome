/*
 * Brightness capability.
 *
 * Bridges the curated brightness API verbs to the BSP backlight control. Like
 * the camera capability, it is a thin adapter: it registers a capability
 * descriptor so the shell checks backlight controllability at startup, and
 * exposes the set/get verbs the app host binds into the curated API.
 *
 * Controllability depends on the board. When the backlight is hardwired on
 * (BSP_PIN_LCD_BL is NC), the BSP reports it uncontrollable; this capability's
 * init then leaves it not-ready, and the curated brightness verbs report
 * unavailable. Settings still cycles and persists the preference in that case,
 * it just cannot dim the panel.
 *
 * The last applied percentage is cached here so brightness_get can answer
 * without a hardware read-back, which the LEDC backlight path does not provide.
 * It starts at full brightness to match the BSP bringing the panel up at full
 * duty.
 */
#include "capability_brightness.h"
#include "capability.h"
#include "bsp.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "cap_brightness";

/* Last applied brightness percentage, cached for brightness_get. Initialized
 * to full to match the panel coming up at full brightness in the BSP. */
static uint8_t s_brightness_percent = 100;

/*
 * Capability init: succeed only if the board's backlight is under PWM control.
 * On a board with a hardwired backlight this returns an error, leaving the
 * capability not-ready so the curated verbs report unavailable.
 */
static esp_err_t brightness_capability_init(void)
{
    if (!bsp_backlight_is_controllable()) {
        ESP_LOGW(TAG, "backlight not under PWM control; brightness unavailable");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "brightness ready");
    return ESP_OK;
}

/* The registered descriptor. Mutable so the host can set `ready`. */
static capability_descriptor_t s_brightness_capability = {
    .name = CAPABILITY_BRIGHTNESS_NAME,
    .init = brightness_capability_init,
    .ready = false,
};

REGISTER_CAPABILITY(s_brightness_capability);

bool capability_brightness_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    esp_err_t err = bsp_backlight_set_percent(percent);
    if (err != ESP_OK) {
        return false;
    }
    s_brightness_percent = percent;
    return true;
}

uint8_t capability_brightness_get(void)
{
    return s_brightness_percent;
}