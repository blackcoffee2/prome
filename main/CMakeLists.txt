# The main component is the firmware entry point. It owns the loop and wires
# together the BSP (hardware + LVGL port), the shell (UI/registry/lifecycle),
# the camera subsystem, and the capabilities. Listing the capability components
# here ensures they link into the image; each registers itself via the linker
# section, so nothing references it by a strong symbol. Removing a capability
# from REQUIRES is how a board without the matching hardware drops it: the
# camera capability for a board with no sensor, the brightness capability for a
# board whose backlight is hardwired on.
#
# The same linker-section registration applies to the apps: each app component
# (app_camera, app_settings, app_clock, app_touch) self-registers via
# REGISTER_APP and is referenced by no strong symbol, so it must appear in
# REQUIRES to be pulled into the image. Removing an app from this list is how
# its tile is dropped from the launcher with no shell change.
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp shell camera capability_camera capability_brightness
             app_camera app_settings app_clock app_touch lvgl
)