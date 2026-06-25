# Camera subsystem. Depends on the esp32-camera driver (pulled via this
# component's idf_component.yml), on LVGL for the canvas blit, and on the BSP
# config header for the DVP pin map.
idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl bsp
)