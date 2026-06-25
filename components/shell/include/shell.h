/*
 * Shell — public interface.
 *
 * The shell is the device's system layer above the BSP. It brings up the
 * persistent services (storage and the registered capabilities), builds the
 * launcher, and hosts apps over it. It deliberately does NOT own the timer
 * loop: app_main pumps LVGL and calls into the shell only to start it.
 *
 * The one entry point is shell_start, called once from app_main after the BSP
 * is up. Everything else the shell does is driven by user interaction on the
 * LVGL task that app_main pumps.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the shell. Mounts storage, initializes capabilities, builds the
 * launcher, and confirms the running firmware image as good. Returns once the
 * launcher is active; it does not loop. The caller (app_main) continues to own
 * and pump the LVGL timer loop.
 */
void shell_start(void);

#ifdef __cplusplus
}
#endif