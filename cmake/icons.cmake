# Build-time icon conversion helper.
#
# add_app_icon(<component_lib> <name> <png_path>) converts a source PNG into an
# LVGL C image descriptor and adds the generated source to the given component
# library. The conversion runs whenever the PNG is newer than the output, so
# edited art is picked up on the next build with no manual step.
#
# Requires LVGLImage.py on PATH (ships with the LVGL component under its
# scripts directory) and a Python interpreter. The generated file defines
# `const lv_image_dsc_t icon_<name>` which the app declares extern and points
# its descriptor at.

function(add_app_icon component_lib name png_path)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/icons")
    set(out_c "${out_dir}/icon_${name}.c")

    add_custom_command(
        OUTPUT "${out_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND python LVGLImage.py --ofmt C --cf RGB565A8
                --output "${out_dir}" "${png_path}"
        DEPENDS "${png_path}"
        COMMENT "Converting icon ${name} to LVGL C"
        VERBATIM
    )

    target_sources(${component_lib} PRIVATE "${out_c}")
endfunction()