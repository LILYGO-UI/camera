cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)
set(LIBCAMERA_DEBIAN_PACKAGE "libcamera0.7" CACHE STRING
    "Debian package matching the libcamera ABI linked by the CM0 BSP")

# The host simulator keeps its synthetic backend and does not need libcamera.
# The CM0 build links the same libcamera and libcamera-base ABI used by the
# Raspberry Pi pipeline.  The BSP toolchain exposes CM0_SDK_ROOT; CMAKE_SYSROOT
# is used as a fallback for other cross toolchains.
if(NOT CM0_SIMULATOR)
    set(LIBCAMERA_DEV_ROOT "" CACHE PATH
        "Optional sysroot or extracted libcamera-dev package root")
    if(LIBCAMERA_DEV_ROOT)
        # The BSP toolchain restricts find_* to its sysroot. Search this explicit
        # target-only development prefix without rewriting it under that root.
        find_path(LIBCAMERA_INCLUDE_DIR
            NAMES libcamera/camera.h
            PATHS "${LIBCAMERA_DEV_ROOT}/include" "${LIBCAMERA_DEV_ROOT}/usr/include"
            PATH_SUFFIXES libcamera
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        find_library(LIBCAMERA_LIBRARY
            NAMES camera
            PATHS "${LIBCAMERA_DEV_ROOT}/lib" "${LIBCAMERA_DEV_ROOT}/usr/lib"
            PATH_SUFFIXES aarch64-linux-gnu
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        find_library(LIBCAMERA_BASE_LIBRARY
            NAMES camera-base
            PATHS "${LIBCAMERA_DEV_ROOT}/lib" "${LIBCAMERA_DEV_ROOT}/usr/lib"
            PATH_SUFFIXES aarch64-linux-gnu
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    endif()
    find_path(LIBCAMERA_INCLUDE_DIR
        NAMES libcamera/camera.h
        HINTS "${LIBCAMERA_DEV_ROOT}/include"
              "${LIBCAMERA_DEV_ROOT}/usr/include"
              "${CM0_SDK_ROOT}/usr/include" "${CMAKE_SYSROOT}/usr/include"
        PATH_SUFFIXES libcamera)
    find_library(LIBCAMERA_LIBRARY
        NAMES camera
        HINTS "${LIBCAMERA_DEV_ROOT}/lib"
              "${LIBCAMERA_DEV_ROOT}/usr/lib"
              "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
        PATH_SUFFIXES aarch64-linux-gnu)
    find_library(LIBCAMERA_BASE_LIBRARY
        NAMES camera-base
        HINTS "${LIBCAMERA_DEV_ROOT}/lib"
              "${LIBCAMERA_DEV_ROOT}/usr/lib"
              "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
        PATH_SUFFIXES aarch64-linux-gnu)
    if(NOT LIBCAMERA_INCLUDE_DIR OR NOT LIBCAMERA_LIBRARY OR NOT LIBCAMERA_BASE_LIBRARY)
        message(FATAL_ERROR
            "CM0 build requires libcamera headers, libcamera.so and libcamera-base.so "
            "in the BSP sysroot. Install matching libcamera-dev files or set "
            "LIBCAMERA_DEV_ROOT to an extracted package root.")
    endif()
endif()

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)

add_library(lilygo_ui_camera_core STATIC
    src/pages/camera/camera_model.cpp
    src/pages/camera/camera_process.cpp
    src/pages/camera/libcamera_backend.cpp)
target_include_directories(lilygo_ui_camera_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_camera_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_camera_core PUBLIC cxx_std_17)
target_link_libraries(lilygo_ui_camera_core PUBLIC Threads::Threads)
if(NOT CM0_SIMULATOR)
    target_include_directories(lilygo_ui_camera_core PRIVATE
        "${LIBCAMERA_INCLUDE_DIR}")
    target_link_libraries(lilygo_ui_camera_core PUBLIC
        "${LIBCAMERA_LIBRARY}"
        "${LIBCAMERA_BASE_LIBRARY}")
    target_compile_definitions(lilygo_ui_camera_core PRIVATE CAMERA_HAS_LIBCAMERA=1)
else()
    target_compile_definitions(lilygo_ui_camera_core PRIVATE CAMERA_HAS_LIBCAMERA=0)
endif()
if(CM0_SIMULATOR)
    target_compile_definitions(lilygo_ui_camera_core PRIVATE CAMERA_DEFAULT_SIMULATED=1)
else()
    target_compile_definitions(lilygo_ui_camera_core PRIVATE CAMERA_DEFAULT_SIMULATED=0)
endif()

add_library(lilygo_ui_camera_ui STATIC
    src/app.cpp
    src/pages/camera/camera_view.cpp
    src/pages/camera/camera_view_model.cpp)
target_include_directories(lilygo_ui_camera_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_camera_ui PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_camera_ui PUBLIC
    lilygo_ui_camera_core
    LilyGoUI::Runtime)

add_executable(lilygo_ui_camera src/main.cpp)
target_compile_options(lilygo_ui_camera PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_camera PRIVATE lilygo_ui_camera_ui)
set_target_properties(lilygo_ui_camera PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_camera
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps
    COMPONENT app)
lilygo_ui_install_launcher_icon(
    "${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}"
    app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(camera-core-tests tests/test_core.cpp)
    target_compile_options(camera-core-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(camera-core-tests PRIVATE
        lilygo_ui_camera_core)
    add_test(NAME camera-core COMMAND camera-core-tests)

    add_executable(camera-view-model-tests tests/test_view_model.cpp)
    target_compile_options(camera-view-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(camera-view-model-tests PRIVATE
        lilygo_ui_camera_ui)
    add_test(NAME camera-view-model COMMAND camera-view-model-tests)

    add_executable(camera-render-ui tests/render_ui.cpp)
    target_compile_options(camera-render-ui PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(camera-render-ui PRIVATE
        lilygo_ui_camera_ui)
    foreach(viewport IN ITEMS portrait landscape)
        if(viewport STREQUAL "portrait")
            set(_width 568)
            set(_height 1232)
        else()
            set(_width 1232)
            set(_height 568)
        endif()
        add_test(NAME camera-render-${viewport} COMMAND camera-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/camera-${viewport}.ppm)
    endforeach()

    # AppKit reserves 96 px for status and 38 px for the home gesture area.
    add_test(NAME camera-render-content-portrait COMMAND camera-render-ui
        568 1098 ${CMAKE_CURRENT_BINARY_DIR}/camera-content-portrait.ppm)
    add_test(NAME camera-render-content-landscape COMMAND camera-render-ui
        1232 434 ${CMAKE_CURRENT_BINARY_DIR}/camera-content-landscape.ppm)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_camera_integration.cpp")
        add_executable(camera-integration-tests tests/test_camera_integration.cpp)
        target_compile_options(camera-integration-tests PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(camera-integration-tests PRIVATE lilygo_ui_camera_core)
        add_test(NAME camera-integration COMMAND camera-integration-tests)
        set_tests_properties(camera-integration PROPERTIES
            TIMEOUT 90
            ENVIRONMENT "CAMERA_SIMULATE=1")
    endif()
endif()

# New applications have no legacy CM0 package to replace. Add Debian migration
# fields only when this project supersedes an actually published package.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, ${LIBCAMERA_DEBIAN_PACKAGE}, libcamera-ipa, ffmpeg, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
