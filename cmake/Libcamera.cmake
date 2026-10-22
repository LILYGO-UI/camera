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
    HINTS "${CM0_SDK_ROOT}/usr/include" "${CMAKE_SYSROOT}/usr/include"
    PATH_SUFFIXES libcamera)
find_library(LIBCAMERA_LIBRARY
    NAMES camera
    HINTS "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
    PATH_SUFFIXES aarch64-linux-gnu)
find_library(LIBCAMERA_BASE_LIBRARY
    NAMES camera-base
    HINTS "${CM0_SDK_ROOT}/usr/lib" "${CMAKE_SYSROOT}/usr/lib"
    PATH_SUFFIXES aarch64-linux-gnu)

function(_camera_fetch_libcamera_headers output)
    # This package matches the runtime shipped in CM0 BSP 0.1.0 exactly.
    set(_version "0.7.2+rpt20260817-1")
    set(_sha256 "b799340d3f753563f583f705af26411b45d8426b6f13d0b734b4da0872bb3b62")
    set(_package "libcamera-dev_${_version}_arm64.deb")
    set(_cache "${CM0_BSP_CACHE_DIR}/libcamera/${_version}")
    set(_root "${_cache}/arm64")
    set(_archive "${_cache}/${_package}")
    file(MAKE_DIRECTORY "${_cache}")
    file(LOCK "${_cache}" DIRECTORY GUARD FUNCTION TIMEOUT 180
        RESULT_VARIABLE _lock_result)
    if(NOT _lock_result STREQUAL "0")
        message(FATAL_ERROR "Could not lock the libcamera cache: ${_lock_result}")
    endif()

    if(NOT EXISTS "${_root}/usr/include/libcamera/libcamera/camera.h")
        set(_archive_hash "")
        if(EXISTS "${_archive}")
            file(SHA256 "${_archive}" _archive_hash)
        endif()
        if(NOT _archive_hash STREQUAL _sha256)
            message(STATUS "Downloading libcamera-dev ${_version} (arm64)")
            file(DOWNLOAD
                "https://archive.raspberrypi.com/debian/pool/main/libc/libcamera/${_package}"
                "${_archive}"
                TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30
                STATUS _download_status)
            list(GET _download_status 0 _download_code)
            list(GET _download_status 1 _download_message)
            if(NOT _download_code EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR
                    "Failed to download ${_package}: ${_download_message}\n"
                    "For offline builds, provide matching development files with "
                    "-DLIBCAMERA_DEV_ROOT=/path/to/libcamera-dev.")
            endif()
            file(SHA256 "${_archive}" _archive_hash)
            if(NOT _archive_hash STREQUAL _sha256)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "SHA-256 mismatch for ${_package}")
            endif()
        endif()

        # CMake handles both the outer Debian ar archive and its data tarball;
        # no host dpkg installation is needed (including on macOS).
        set(_staging "${_cache}/extracting")
        file(REMOVE_RECURSE "${_staging}")
        file(MAKE_DIRECTORY "${_staging}/package" "${_staging}/root")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_staging}/package")
        file(GLOB _data_archives "${_staging}/package/data.tar.*")
        list(LENGTH _data_archives _data_count)
        if(NOT _data_count EQUAL 1)
            message(FATAL_ERROR "Missing data archive in ${_package}")
        endif()
        list(GET _data_archives 0 _data_archive)
        file(ARCHIVE_EXTRACT INPUT "${_data_archive}" DESTINATION "${_staging}/root")
        if(NOT EXISTS "${_staging}/root/usr/include/libcamera/libcamera/camera.h")
            message(FATAL_ERROR "Missing libcamera headers in ${_package}")
        endif()
        file(REMOVE_RECURSE "${_root}")
        file(RENAME "${_staging}/root" "${_root}")
        file(REMOVE_RECURSE "${_staging}")
    endif()
    set(${output} "${_root}/usr/include/libcamera" PARENT_SCOPE)
endfunction()

# The pinned BSP contains runtime libraries but omits libcamera-dev. Only
# supplement this known version; other SDKs must provide matching development
# files themselves. Link the versioned BSP libraries without modifying sysroot.
set(_libcamera_bsp_libdir "${CM0_SDK_ROOT}/usr/lib/aarch64-linux-gnu")
if(CM0_BSP_VERSION STREQUAL "0.1.0" AND
   CM0_BSP_CACHE_DIR AND NOT LIBCAMERA_DEV_ROOT AND
   LIBCAMERA_DEBIAN_PACKAGE STREQUAL "libcamera0.7" AND
   EXISTS "${_libcamera_bsp_libdir}/libcamera.so.0.7.2" AND
   EXISTS "${_libcamera_bsp_libdir}/libcamera-base.so.0.7.2")
    if(NOT LIBCAMERA_INCLUDE_DIR)
        _camera_fetch_libcamera_headers(_libcamera_headers)
        set(LIBCAMERA_INCLUDE_DIR "${_libcamera_headers}" CACHE PATH
            "libcamera include directory" FORCE)
    endif()
    if(NOT LIBCAMERA_LIBRARY)
        set(LIBCAMERA_LIBRARY "${_libcamera_bsp_libdir}/libcamera.so.0.7.2" CACHE FILEPATH
            "libcamera library" FORCE)
    endif()
    if(NOT LIBCAMERA_BASE_LIBRARY)
        set(LIBCAMERA_BASE_LIBRARY "${_libcamera_bsp_libdir}/libcamera-base.so.0.7.2" CACHE FILEPATH
            "libcamera base library" FORCE)
    endif()
endif()

if(NOT LIBCAMERA_INCLUDE_DIR OR NOT LIBCAMERA_LIBRARY OR NOT LIBCAMERA_BASE_LIBRARY)
    message(FATAL_ERROR
        "CM0 build requires libcamera headers, libcamera.so and libcamera-base.so "
        "in the BSP sysroot. Install matching libcamera-dev files or set "
        "LIBCAMERA_DEV_ROOT to an extracted package root.")
endif()
