# Buddy app component. Depends on the shell for the app descriptor and
# registration macro, and on LVGL for the canvas, event, and timer types. The
# pixel-art panda and its compositor live in panda_sprite.c, split from the app
# logic so the art can be edited in isolation. There is no icon source: the
# descriptor sets .icon = NULL, so the launcher renders the initial-on-tile
# placeholder ("B") rather than a compiled-in image. To add a real icon later,
# place a PNG, convert it with add_app_icon (see cmake/icons.cmake), and add the
# generated icon_buddy.c here alongside a matching extern descriptor in
# app_buddy.c. WHOLE_ARCHIVE keeps the REGISTER_APP contribution from being
# dropped by the linker, since no strong symbol references this translation
# unit. Removing this component from the build removes the Buddy tile with no
# shell change.
idf_component_register(
    SRCS "app_buddy.c"
         "panda_sprite.c"
    INCLUDE_DIRS "."
    REQUIRES shell lvgl
    WHOLE_ARCHIVE
)