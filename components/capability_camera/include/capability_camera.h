/*
 * Camera capability — verbs exported to the app host.
 *
 * These are the functions the app host binds into the curated camera API. They
 * are declared in their own header so the app host can take them as weak
 * references: when the camera capability component is excluded from the build,
 * the references resolve to NULL and the API reports the camera unavailable,
 * with no hard link dependency on this component.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Name the capability registers under; the app API checks readiness with this
 * exact string. */
#define CAPABILITY_CAMERA_NAME "camera"

/*
 * Start the viewfinder onto the given canvas. Returns true on success, false
 * if the camera is unavailable or the start failed. Wraps the camera
 * subsystem's viewfinder start behind the boolean the curated API expects.
 */
bool capability_camera_viewfinder_start(lv_obj_t *canvas);

/*
 * Stop the viewfinder. Safe to call even if no viewfinder is running.
 */
void capability_camera_viewfinder_stop(void);

#ifdef __cplusplus
}
#endif