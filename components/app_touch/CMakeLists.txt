# components/app_touch/CMakeLists.txt (Version 1)
# Touch calibration app component. Depends on the shell for the app descriptor
# and registration macro, and on LVGL for the canvas, event, and input-device
# types. There is no icon source: the descriptor sets .icon = NULL, so the
# launcher renders the initial-on-tile placeholder ("T") rather than a
# compiled-in image. To add a real icon later, place a PNG, convert it with
# add_app_icon (see cmake/icons.cmake), and add the generated icon_touch.c here
# alongside a matching extern descriptor in app_touch.c. WHOLE_ARCHIVE keeps
# the REGISTER_APP contribution from being dropped by the linker, since no
# strong symbol references this translation unit. Removing this component from
# the build removes the Touch tile with no shell change.
idf_component_register(
    SRCS "app_touch.c"
         "icon_touch.c"
    INCLUDE_DIRS ""
    REQUIRES shell lvgl
    WHOLE_ARCHIVE
)