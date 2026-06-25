# Camera capability: a pluggable C component beside the shell. Depends on the
# shell for the capability descriptor/registration macro, on the camera
# subsystem for the heavy pipeline, and on LVGL for the canvas type. Removing
# this component from the build deregisters the capability with no shell change.
idf_component_register(
    SRCS "capability_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES shell camera lvgl
    WHOLE_ARCHIVE
)