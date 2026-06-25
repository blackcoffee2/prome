/*
 * Brightness capability — verbs exported to the app host.
 *
 * The functions the app host binds into the curated brightness API. Declared
 * in their own header so the app host can take them as weak references: when
 * this capability component is excluded from the build, the references resolve
 * to NULL and the API reports brightness unavailable, with no hard link
 * dependency on this component.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Name the capability registers under; the app API checks readiness with this
 * exact string. */
#define CAPABILITY_BRIGHTNESS_NAME "brightness"

/*
 * Set the backlight brightness as a percentage (0..100), clamped. Returns true
 * if the change took effect, false if the backlight is not controllable on
 * this board or the set failed. Caches the applied value for
 * capability_brightness_get.
 */
bool capability_brightness_set(uint8_t percent);

/*
 * The last applied brightness percentage (0..100). Answered from a cached
 * value, since the LEDC backlight path offers no hardware read-back. Returns
 * full brightness before any set has been applied.
 */
uint8_t capability_brightness_get(void);

#ifdef __cplusplus
}
#endif