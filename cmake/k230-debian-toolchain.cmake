# Consume the selected source SDK through its public toolchain entry point.
# Keep an explicit -DLilyGoUI_DIR selection across subsequent lpm pack runs.
if(NOT LilyGoUI_DIR)
    if(NOT "$ENV{LILYGO_UI_APPKIT_DIR}" STREQUAL "")
        set(LilyGoUI_DIR "$ENV{LILYGO_UI_APPKIT_DIR}")
    else()
        set(LilyGoUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/cm0-appkit")
    endif()
endif()
get_filename_component(LilyGoUI_DIR "${LilyGoUI_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(LilyGoUI_DIR "${LilyGoUI_DIR}" CACHE PATH "LILYGO UI source SDK" FORCE)
if(NOT EXISTS "${LilyGoUI_DIR}/cmake/k230-debian-toolchain.cmake")
    message(FATAL_ERROR
        "The selected AppKit has no Debian K230 toolchain:\n"
        "  ${LilyGoUI_DIR}\n"
        "Update the pinned AppKit, or select a compatible source SDK once with:\n"
        "  cmake --preset k230-debian -DLilyGoUI_DIR=/path/to/appkit\n"
        "Then run lpm pack --arch riscv64 again. See README.md for the compiler "
        "and Debian sysroot requirements.")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LilyGoUI_DIR)
include("${LilyGoUI_DIR}/cmake/k230-debian-toolchain.cmake")
