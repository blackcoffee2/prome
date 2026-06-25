/*
 * OTA — implementation.
 *
 * Uses esp_ota_* over the dual-slot partition layout. apply streams an image
 * from the transport callback into the inactive slot via an OTA write handle;
 * esp_ota_end validates the image (and, with signed-app verification enabled in
 * the build, checks the signature) before esp_ota_set_boot_partition switches
 * the active pointer. confirm marks a freshly booted image permanent; if the
 * device resets before confirming, the bootloader rolls back to the prior
 * slot, so a bad image cannot brick the device.
 *
 * The transport is fully decoupled: this module never knows whether the image
 * arrived over HTTP, BLE, or serial — only the read callback.
 */
#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

/* Streaming buffer size for moving image bytes from transport into flash. A
 * few KB balances throughput against RAM use during an update. */
#define OTA_CHUNK_BYTES 4096

void ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        /* The self-check that gates this call has passed by the time we are
         * here, so mark the image valid and cancel the pending rollback. */
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running image confirmed");
    }
}

esp_err_t ota_apply(ota_read_fn read_fn, void *user)
{
    if (read_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no inactive slot to write");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "writing update to slot '%s'", target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    static uint8_t chunk[OTA_CHUNK_BYTES];
    for (;;) {
        int got = read_fn(user, chunk, sizeof(chunk));
        if (got < 0) {
            ESP_LOGE(TAG, "transport error");
            esp_ota_abort(handle);
            return ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        err = esp_ota_write(handle, chunk, (size_t)got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return err;
        }
    }

    /* esp_ota_end validates the written image (and verifies its signature when
     * signed-app verification is configured). A failure here leaves the boot
     * pointer untouched, so the device stays on its current image. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image verification failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "update staged; reboot to apply (pending confirmation)");
    return ESP_OK;
}