cmake_minimum_required(VERSION 3.5)
project(DesktopFix)

# Build the INIT as a flat code resource
add_executable(DesktopFix
    desktopfix.c
    ShowInitIcon.c
    desktopfix.r
    ShowInitIcon.h)

set_target_properties(DesktopFix PROPERTIES
    OUTPUT_NAME DesktopFix.flt
    COMPILE_OPTIONS -ffunction-sections
    LINK_FLAGS "-Wl,--mac-flat -Wl,-gc-sections")

if (REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Use Rez to assemble the final INIT file
add_custom_command(
    OUTPUT DesktopFix.bin DesktopFix.dsk
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                    ${CMAKE_CURRENT_SOURCE_DIR}/desktopfix.r
                    -o DesktopFix.bin
                    --cc DesktopFix.dsk
                    --cc DesktopFix
                    -t INIT
                    -c "DsFx"
    DEPENDS DesktopFix desktopfix.r)

add_custom_target(DesktopFix_INIT ALL DEPENDS DesktopFix.dsk)
