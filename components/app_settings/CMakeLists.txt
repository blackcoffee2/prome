# Settings app component. Depends on the shell for the app descriptor and
# registration macro, on LVGL for widget types used in the list, and on
# esp_timer and esp_system for the About panel's live runtime stats
# (esp_timer_get_time for uptime, esp_get_free_heap_size for free memory).
# WHOLE_ARCHIVE retains the REGISTER_APP contribution. Removing this component
# removes the Settings tile with no shell change.
idf_component_register(
    SRCS "app_settings.c"
         "icon_settings.c"
    INCLUDE_DIRS ""
    REQUIRES shell lvgl esp_timer esp_system
    WHOLE_ARCHIVE
)