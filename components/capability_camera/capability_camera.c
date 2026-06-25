/*
 * Camera capability.
 *
 * Bridges the curated camera API verbs to the camera subsystem. It is a thin
 * adapter: it registers a capability descriptor so the shell initializes the
 * sensor at startup, and exposes the viewfinder start/stop verbs the app host
 * binds into the curated API.
 *
 * The split is deliberate. The camera component (components/camera) owns the
 * hardware pipeline — sensor DMA, frame buffers, the blit. This capability is
 * the registration-and-binding seam between that C subsystem and the sandboxed
 * app API, so the shell does not take a direct dependency on the camera
 * component and an app reaches the camera only through the curated verbs.
 *
 * If the sensor is absent, camera_init returns an error, the capability is
 * marked not-ready by the capability host, and the curated camera verbs report
 * unavailable. The app then shows its "camera unavailable" state.
 */
#include "capability_camera.h"
#include "capability.h"
#include "camera.h"

#include "esp_log.h"

static const char *TAG = "cap_camera";

/*
 * Capability init: bring up the sensor. Returns whatever camera_init returns,
 * so a board with no camera leaves this capability not-ready rather than
 * failing startup.
 */
static esp_err_t camera_capability_init(void)
{
    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera sensor not available");
    }
    return err;
}

/* The registered descriptor. Mutable so the host can set `ready`. */
static capability_descriptor_t s_camera_capability = {
    .name = CAPABILITY_CAMERA_NAME,
    .init = camera_capability_init,
    .ready = false,
};

REGISTER_CAPABILITY(s_camera_capability);

bool capability_camera_viewfinder_start(lv_obj_t *canvas)
{
    return camera_viewfinder_start(canvas) == ESP_OK;
}

void capability_camera_viewfinder_stop(void)
{
    camera_viewfinder_stop();
}