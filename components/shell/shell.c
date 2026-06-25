/*
 * Shell — implementation.
 *
 * Startup brings up services in order: mount persistent storage, initialize
 * registered capabilities (camera and any others compiled in), then delegate
 * to the launcher, which owns the persistent three-band home screen. Both
 * services come up before any app can run so the curated API is fully backed
 * on first launch; a failed storage mount or an absent capability degrades to
 * an unavailable service rather than failing startup.
 *
 * Reaching a built launcher is the self-check that gates OTA confirmation: if
 * the firmware boots this far and renders the home screen, the running image is
 * good, so a freshly OTA-updated image is marked permanent here. An image that
 * crashes before this point is rolled back to the previous slot on reset, so a
 * bad update cannot brick the device.
 *
 * The shell never owns the timer loop. app_main pumps LVGL; the shell only
 * brings up services and builds the launcher, then hosts apps over it.
 */
#include "shell.h"
#include "launcher.h"
#include "storage.h"
#include "capability_host.h"
#include "ota.h"

#include "esp_log.h"

static const char *TAG = "shell";

void shell_start(void)
{
    if (storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "storage unavailable; continuing without it");
    }
    capability_host_init();
    launcher_build();

    /* The launcher built successfully: confirm the running image as good so a
     * pending OTA update is made permanent. No-op when no update is pending. */
    ota_confirm_running_image();

    ESP_LOGI(TAG, "shell started; launcher active");
}