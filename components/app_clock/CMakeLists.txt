# Clock app component. Depends on the shell for the app descriptor and
# registration macro, and on LVGL for the label and timer. WHOLE_ARCHIVE
# retains the REGISTER_APP contribution. Removing this component removes the
# Clock tile with no shell change.
idf_component_register(
    SRCS "app_clock.c"
         "icon_clock.c"
    INCLUDE_DIRS ""
    REQUIRES shell lvgl
    WHOLE_ARCHIVE
)