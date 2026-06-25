/*
 * OTA — public interface.
 *
 * Over-the-air firmware update over the dual-slot partition layout, with the
 * image transport fully decoupled behind a read callback. The caller supplies
 * the bytes (from HTTP, BLE, serial, or anywhere); this module writes them into
 * the inactive slot, validates the image, and switches the boot pointer. A
 * freshly booted image is confirmed only after the device proves it can run, so
 * a bad update rolls back on reset rather than bricking the device.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Image-byte transport callback. Called repeatedly to pull the next chunk of
 * the firmware image into buf (up to len bytes). Returns the number of bytes
 * read, 0 at end of image, or negative on a transport error. Decoupling the
 * transport this way keeps the OTA module independent of how the image
 * arrives.
 */
typedef int (*ota_read_fn)(void *user, void *buf, size_t len);

/*
 * Confirm the running image as good, making a pending OTA update permanent and
 * cancelling the automatic rollback. Called once the device has proven it can
 * boot and reach a known-good state (the shell calls this after the launcher
 * builds). A no-op when no update is pending.
 */
void ota_confirm_running_image(void);

/*
 * Apply an update. Streams the image from read_fn into the inactive slot,
 * validates it, and sets it as the boot partition. The new image takes effect
 * on the next reboot and must be confirmed (see ota_confirm_running_image) or
 * it rolls back. Returns ESP_OK when the update is staged, or an error if the
 * transport, write, or validation failed (in which case the current image is
 * untouched).
 */
esp_err_t ota_apply(ota_read_fn read_fn, void *user);

#ifdef __cplusplus
}
#endif