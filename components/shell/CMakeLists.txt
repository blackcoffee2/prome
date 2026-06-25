# Shell component: launcher, app registry, curated app API, app lifecycle, and
# per-app storage. Depends on LVGL for widgets and on the LittleFS component
# (pulled via this component's idf_component.yml) for the storage backing.
#
# wallpaper.c carries the compiled-in 320x240 launcher background as a const
# lv_image_dsc_t (wallpaper_dsc), which theme.h points THEME_WALLPAPER_SRC at;
# it links into flash and costs no RAM. The descriptor was generated offline
# from the source art, so no build-time image conversion step is needed here.
#
# The linker.lf fragment places and KEEPs the .app_registry and .cap_registry
# sections and emits their boundary symbols, so REGISTER_APP/REGISTER_CAPABILITY
# entries survive link-time GC and the hosts can walk them. LDFRAGMENTS is the
# ESP-IDF mechanism for this and hooks into IDF's managed sections layout,
# unlike a raw INSERT-based linker script.
idf_component_register(
    SRCS "shell.c"
         "launcher.c"
         "app_host.c"
         "storage.c"
         "capability_host.c"
         "ota.c"
         "wallpaper.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl littlefs app_update
    LDFRAGMENTS "linker.lf"
)