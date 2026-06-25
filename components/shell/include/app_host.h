/*
 * App host — public interface.
 *
 * The host enumerates the app registry for the launcher and runs the app
 * lifecycle: launching a chosen app over the persistent launcher screen and
 * returning to the launcher on exit. It also implements the curated app API,
 * but that surface is private to the host; the launcher only needs registry
 * access and launch.
 */
#pragma once

#include <stddef.h>

#include "app_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Number of registered apps in the registry. The launcher walks 0..count-1 to
 * build one tile per app.
 */
size_t app_host_count(void);

/*
 * The descriptor at registry index i, or NULL if i is out of range. Registry
 * order is the order the linker placed the descriptors; the launcher does not
 * depend on any particular ordering.
 */
const app_descriptor_t *app_host_descriptor(size_t i);

/*
 * Launch an app. Creates a fresh screen and root over the persistent launcher,
 * binds the curated API, and calls the app's run(). Ignored if a descriptor is
 * NULL or an app is already running; the host hosts one app at a time.
 */
void app_host_launch(const app_descriptor_t *descriptor);

#ifdef __cplusplus
}
#endif