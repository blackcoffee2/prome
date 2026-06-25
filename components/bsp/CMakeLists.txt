# BSP component: panel, touch, and the hand-written LVGL port glue.
#
# The touch driver component (esp_lcd_touch_ft5x06) is pulled in via this
# component's own idf_component.yml. esp_lvgl_port is intentionally absent:
# the timer loop is owned by app_main, not by an integration task.
idf_component_register(
    SRCS "bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_timer lvgl
)