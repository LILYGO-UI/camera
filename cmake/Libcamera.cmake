# Prefer development files supplied by the user or the target sysroot.
set(LIBCAMERA_DEV_ROOT "" CACHE PATH
    "Optional sysroot or extracted libcamera-dev package root")

# find_* trusts cached paths even when a temporary extraction has disappeared.
if(LIBCAMERA_DEV_ROOT AND NOT IS_DIRECTORY "${LIBCAMERA_DEV_ROOT}")
    message(STATUS "Discarding missing LIBCAMERA_DEV_ROOT: ${LIBCAMERA_DEV_ROOT}")
    set(LIBCAMERA_DEV_ROOT "" CACHE PATH
        "Optional sysroot or extracted libcamera-dev package root" FORCE)
endif()
if(LIBCAMERA_INCLUDE_DIR AND
   NOT EXISTS "${LIBCAMERA_INCLUDE_DIR}/libcamera/camera.h")
    unset(LIBCAMERA_INCLUDE_DIR CACHE)
    unset(LIBCAMERA_INCLUDE_DIR)
endif()
foreach(_library IN ITEMS LIBCAMERA_LIBRARY LIBCAMERA_BASE_LIBRARY)
    if(${_library} AND NOT EXISTS "${${_library}}")
        unset(${_library} CACHE)
        unset(${_library})
    endif()
endforeach()

if(LIBCAMERA_DEV_ROOT)
    # An explicit target prefix must not be rewritten under the BSP sysroot.
    find_path(LIBCAMERA_INCLUDE_DIR
        NAMES libcamera/camera.h
        PATHS "${LIBCAMERA_DEV_ROOT}/include" "${LIBCAMERA_DEV_ROOT}/usr/include"
        PATH_SUFFIXES libcamera
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LIBCAMERA_LIBRARY
        NAMES camera
        PATHS "${LIBCAMERA_DEV_ROOT}/lib" "${LIBCAMERA_DEV_ROOT}/usr/lib"
        PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LIBCAMERA_BASE_LIBRARY
        NAMES camera-base
        PATHS "${LIBCAMERA_DEV_ROOT}/lib" "${LIBCAMERA_DEV_ROOT}/usr/lib"
        PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
find_path(LIBCAMERA_INCLUDE_DIR
    NAMES libcamera/camera.h
    HINTS "${CM0_SDK_ROOT}/usr/include" "${CMAKE_SYSROOT}/usr/include"
    PATH_SUFFIXES libcamera)
find_library(LIBCAMERA_LIBRARY
    NAMES camera
    HINTS "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
    PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE})
find_library(LIBCAMERA_BASE_LIBRARY
    NAMES camera-base
    HINTS "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
    PATH_SUFFIXES ${CMAKE_LIBRARY_ARCHITECTURE})

if(NOT LIBCAMERA_INCLUDE_DIR OR NOT LIBCAMERA_LIBRARY OR NOT LIBCAMERA_BASE_LIBRARY)
    message(FATAL_ERROR
        "Device build requires libcamera headers, libcamera.so and libcamera-base.so "
        "in the target sysroot. Install matching libcamera-dev files or set "
        "LIBCAMERA_DEV_ROOT to an extracted package root.")
endif()
