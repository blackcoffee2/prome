# Camera app component. Depends on the shell for the app descriptor and
# registration macro, and on LVGL for the canvas type and image descriptor.
# icon_camera.c is the build-time-converted launcher icon. WHOLE_ARCHIVE keeps
# the REGISTER_APP contribution from being dropped by the linker, since no
# strong symbol references this translation unit. Removing this component from
# the build removes the Camera tile with no shell change.
idf_component_register(
    SRCS "app_camera.c"
         "icon_camera.c"
    INCLUDE_DIRS ""
    REQUIRES shell lvgl
    WHOLE_ARCHIVE
)