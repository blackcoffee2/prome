/*
 * Launcher — public interface.
 *
 * The launcher builds the persistent home screen (the three-band layout) once
 * at startup and never destroys it. App screens are layered over
 * it and torn down on exit, returning the user here. The shell builds the
 * launcher during startup; the app host reloads it when an app exits.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build the launcher home screen and load it. Constructs the status pane, the
 * softkey pane, and the scrollable icon grid (one tile per registered app),
 * starts the status-clock timer, and makes the screen active. Returns the
 * launcher screen object. Called once at startup.
 */
lv_obj_t *launcher_build(void);

/*
 * The persistent launcher screen object, for the app host to reload when an
 * app exits. Returns NULL if called before launcher_build.
 */
lv_obj_t *launcher_screen(void);