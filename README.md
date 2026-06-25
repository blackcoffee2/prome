# PROME

PROME ("Productivity and Media console") is open-source firmware for a small
ESP32-S3 handheld console, built on [ESP-IDF](https://github.com/espressif/esp-idf)
and [LVGL](https://lvgl.io/). It boots to a tile-grid home screen and
runs a small set of apps — a camera viewfinder, a focus timer, a settings
screen, and a touch-calibration tool — each built against a deliberately narrow,
sandboxed API.

The codebase is organized around two load-bearing ideas, and the inline comments
document the hardware bring-up lessons (display timing, color byte order, camera
clocking, touch calibration) in detail. If you are porting PROME to different
hardware, those comments are the bring-up guide.

## Demo

## Hardware

PROME targets readily available, breadboard-friendly modules:

- **MCU board:** ESP32-S3-DevKitC-1, N16R8 variant (16 MB flash, 8 MB octal
  PSRAM).
- **Display:** KMRTM32032-SPI — a 3.2" 240×320 ILI9341 panel on a 4-wire SPI
  interface. The UI runs landscape (320×240). Note this is the **SPI** variant
  of the common red TFT board, not the 8-bit parallel one.
- **Touch:** XPT2046 (HR2046) resistive controller, sharing the display's SPI
  bus with its own chip select.
- **Camera (optional):** OV7670 (no FIFO) on the parallel DVP interface.

All board specifics — every GPIO assignment, bus parameter, and panel
orientation flag — live in a single file, `components/bsp/include/bsp_config.h`.
Porting to a different board is a one-file change there. Nothing above the board
support layer references a raw pin.

For the exact pin-by-pin connections, see [`WIRING.md`](WIRING.md), which lists
the ESP32-S3-to-LCD and ESP32-S3-to-camera mappings as tables. **Keep the camera
jumper cables under 10 cm** — the no-FIFO OV7670 has no frame buffer to absorb
DVP timing slop, so longer leads corrupt the camera feed.

Wire the board's VCC to 3V3 (not 5V) so every line is ESP32-S3 safe, and drive
the display's LED (backlight) line — tie it to 3V3 for an always-on backlight,
or to the configured backlight GPIO for software brightness control. The camera
is optional: when its pins are left unconnected the firmware detects this and
brings the device up without it.

## Building and flashing

PROME builds with a standard ESP-IDF toolchain. With ESP-IDF installed and its
environment sourced:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your serial device (for example `/dev/ttyACM0` on Linux,
or a `COM` port on Windows). The DevKitC-1's native USB also serves as the
serial/JTAG interface, so flashing and logging work over the single USB
connection.

The default build configuration in `sdkconfig.defaults` enables octal PSRAM, the
camera and touch drivers, and the fonts the UI uses. Local `sdkconfig` changes
are not tracked; edit `sdkconfig.defaults` to change a default for everyone.

## Architecture

**The timer loop is owned at the top.** `app_main` brings up the board support
layer and the shell, then runs LVGL's timer handler forever in its own loop. It
never hands that loop to a separate task — not to `esp_lvgl_port`, not to the
shell, not to any app. Everything (the shell, every app, every capability) runs
cooperatively on that single LVGL task via registered callbacks and timers.
Because the highest-level function holds the loop, nothing beneath it can capture
control: an app builds its UI and returns. A direct consequence is that no LVGL
locking is needed anywhere in the codebase, since there is exactly one LVGL task.

**The app API is a sandbox.** Apps never touch LVGL, the filesystem, or hardware
directly. The shell hands each app a struct of function pointers (`app_api_t`)
plus an opaque per-app context. That curated surface _is_ the sandbox: an app can
reach exactly what the API exposes and nothing more. Storage is automatically
scoped to a per-app folder, and hardware services (the camera viewfinder, the
backlight brightness) are reached through coarse verbs backed by "capabilities."

**Apps and capabilities self-register.** Each app and each capability places a
descriptor into a dedicated linker section via the `REGISTER_APP` /
`REGISTER_CAPABILITY` macros. The launcher enumerates the app section at startup
to build one tile per app; the shell enumerates the capability section to
initialize hardware services. Adding or removing a feature is a build-list change
with no edit to the shell or any central table. A capability whose hardware is
absent (no camera sensor, a hardwired backlight) simply reports itself
unavailable, and the device comes up without it.

### Layout

`main/` holds `app_main`, which brings up the board and shell and owns the loop.
Everything else lives under `components/`, organized by a naming convention:
`bsp` and `camera` own the hardware; `shell` owns the system layer above it;
each user-facing app is an `app_*` component; and a hardware service is a
`capability_*` component that binds an app verb to the hardware (so `app_camera`
pairs with `capability_camera`). Adding a feature means adding another component
of the matching kind — the directory listing on the repo is the authoritative,
always-current map.

The shell layer (`components/shell`) holds the system pieces above the board:
the persistent launcher home screen, the app host that runs the app lifecycle
and implements the curated API, per-app-scoped storage on a LittleFS partition,
a transport-agnostic OTA updater with automatic rollback, and the theme — a
single header where every color, font, and geometry value is defined by role, so
the whole device look changes from one place.

## License

PROME is licensed under the Apache License, Version 2.0. See the
[`LICENSE`](LICENSE) file for the full text.

## Credits

The launcher app icons are from [3dicons](https://3dicons.co).
