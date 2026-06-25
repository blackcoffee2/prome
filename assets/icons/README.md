# Launcher Icons — build-time conversion

Every app carries a launcher icon compiled into the firmware as an LVGL binary
image descriptor (`const lv_image_dsc_t`), referenced by pointer from the app
descriptor. There is no runtime image decoder and no filesystem dependency:
icons ship inside the firmware image with their apps, which matches the
whole-image OTA delivery model. An app whose descriptor has a NULL `.icon` is
not an error — the launcher draws the app's initial on the tile face instead.

## Source art

- Put source PNGs at `assets/icons/<name>.png`.
- Square, with transparency (RGBA). The tile is `THEME_TILE_SIZE_PX` (92 px)
  square; 100x100 source art scales down cleanly and stays crisp. A 100x100
  RGB565A8 icon is ~30 KB in flash — negligible against the 3 MB app slot.

## Adding an icon

This is the manual convert-and-commit flow. It is more reliable than the CMake
auto-conversion helper, because the converter script is not on PATH and lives
inside the downloaded LVGL managed component. Using the Camera app as the
example.

1. Find the converter (first build downloads it under managed_components):

   ```
   dir /s /b managed_components\lvgl__lvgl\*LVGLImage.py
   ```

2. Convert the PNG to a C source. RGB565A8 = 16-bit color + 8-bit alpha, so
   icons have clean edges over the dark gradient without a full 32-bit cost:

   ```
   python managed_components\lvgl__lvgl\scripts\LVGLImage.py --ofmt C --cf RGB565A8 -o components\app_camera assets\icons\camera.png
   ```

   This writes `components\app_camera\camera.c` defining the variable
   `const lv_image_dsc_t camera` (named after the PNG).

3. Rename the generated file to `icon_<name>.c` (e.g. `icon_camera.c`) so it is
   obviously an asset and never collides with an app source file.

4. Rename the descriptor variable inside that file so it cannot clash with
   component/header names, AND confirm it is not `static` (it needs external
   linkage so the app can reference it — a `static` descriptor causes an
   "undefined reference" link error). Change:

   ```c
   const lv_image_dsc_t camera = {
   ```

   to:

   ```c
   const lv_image_dsc_t icon_camera_dsc = {
   ```

   The new name must match the extern in step 6 byte-for-byte. (The
   `<name>_map[]` byte-array name can stay; only the `lv_image_dsc_t` variable
   matters.)

5. Fix the LVGL include at the top of the generated file. The converter emits a
   conditional block that falls through to `#include "lvgl/lvgl.h"`, a path that
   does not exist in ESP-IDF and causes a fatal "No such file or directory"
   error. Replace the entire `#if defined(LV_LVGL_H_INCLUDE_SIMPLE) ... #endif`
   block with a single line:

   ```c
   #include "lvgl.h"
   ```

   This matches how every other source in the project includes LVGL.

6. Add the generated file to the app component's `CMakeLists.txt` SRCS:

   ```cmake
   SRCS "app_camera.c"
        "icon_camera.c"
   ```

7. In the app's `.c`, declare the extern after the includes and point the
   descriptor at it:

   ```c
   extern const lv_image_dsc_t icon_camera_dsc;
   ...
   .icon = &icon_camera_dsc,
   ```

8. `idf.py build`, then flash. The tile shows the art instead of the initial.
   (No `del sdkconfig` needed — these are source/CMake changes, not config.)

## Per-app names used in this project

- Camera → PNG `camera.png` → file `icon_camera.c` → var `icon_camera_dsc` → in `app_camera`

## Troubleshooting

- `fatal error: lvgl/lvgl.h: No such file or directory`
  → Step 5 not applied. Fix the include block in the generated file.

- `undefined reference to 'icon_<name>_dsc'`
  → Step 4 name mismatch, or the descriptor is still `static`. Make the
  generated variable name match the extern exactly, with no `static`.

- Tile still shows the letter initial instead of art
  → `.icon` in the app's descriptor is still `NULL` (step 7 not applied), or the
  generated file was not added to SRCS (step 6).

## Note on the CMake helper

`cmake/icons.cmake` defines an `add_app_icon` function intended to auto-convert
art at build time. It assumes `LVGLImage.py` is on PATH, which it is not by
default, so it is currently unused. The manual flow above is the supported
path. If you wire up the helper later, steps 4 and 5 still apply.
