# Brightness capability: a pluggable C component beside the shell. Depends on
# the shell for the capability descriptor/registration macro and on the BSP for
# the LEDC PWM backlight API. Removing this component from the build
# deregisters the capability with no shell change, and the app-API brightness
# verbs report unavailable. WHOLE_ARCHIVE keeps the REGISTER_CAPABILITY
# contribution from being dropped by the linker, since no strong symbol
# references this translation unit.
idf_component_register(
    SRCS "capability_brightness.c"
    INCLUDE_DIRS "include"
    REQUIRES shell bsp
    WHOLE_ARCHIVE
)