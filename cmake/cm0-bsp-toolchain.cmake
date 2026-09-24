set(CM0_BSP_VERSION "0.1.0")
set(CM0_BSP_URL
    "https://github.com/LILYGO-UI/CM0BspBuilder/releases/download/0.1.0/cm0_sdk.tar.gz"
    CACHE STRING "URL of the CM0 BSP SDK archive")
set(CM0_BSP_SHA256
    "095b1e18e338ad297717f6454675081deff17997704c7460cd0fe8bdcef2a5ee"
    CACHE STRING "SHA-256 checksum of the CM0 BSP SDK archive")

get_filename_component(_CM0_BSP_PROJECT_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CM0_BSP_CACHE_DIR "${_CM0_BSP_PROJECT_ROOT}/.cache/cm0-bsp"
    CACHE PATH "Directory used to cache downloaded CM0 BSP releases")

set(_CM0_BSP_RELEASE_DIR "${CM0_BSP_CACHE_DIR}/${CM0_BSP_VERSION}")
set(_CM0_BSP_ARCHIVE "${_CM0_BSP_RELEASE_DIR}/cm0_sdk.tar.gz")
set(_CM0_BSP_SYSROOT "${_CM0_BSP_RELEASE_DIR}/sysroot")
set(_CM0_BSP_TOOLCHAIN
    "${_CM0_BSP_SYSROOT}/usr/share/cm0-bsp/toolchain.cmake")

function(_cm0_bsp_sysroot_is_ready output)
    if(EXISTS "${_CM0_BSP_TOOLCHAIN}" AND
       EXISTS "${_CM0_BSP_SYSROOT}/usr/include" AND
       EXISTS "${_CM0_BSP_SYSROOT}/usr/lib")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

_cm0_bsp_sysroot_is_ready(_CM0_BSP_READY)
if(NOT _CM0_BSP_READY)
    file(MAKE_DIRECTORY "${_CM0_BSP_RELEASE_DIR}")
    file(LOCK "${_CM0_BSP_RELEASE_DIR}" DIRECTORY GUARD FILE TIMEOUT 900
        RESULT_VARIABLE _CM0_BSP_LOCK_RESULT)
    if(NOT _CM0_BSP_LOCK_RESULT STREQUAL "0")
        message(FATAL_ERROR
            "Could not lock the CM0 BSP cache: ${_CM0_BSP_LOCK_RESULT}")
    endif()

    _cm0_bsp_sysroot_is_ready(_CM0_BSP_READY)
    if(NOT _CM0_BSP_READY)
        message(STATUS "Downloading CM0 BSP ${CM0_BSP_VERSION}")
        file(DOWNLOAD "${CM0_BSP_URL}" "${_CM0_BSP_ARCHIVE}"
            EXPECTED_HASH "SHA256=${CM0_BSP_SHA256}"
            INACTIVITY_TIMEOUT 60
            SHOW_PROGRESS
            STATUS _CM0_BSP_DOWNLOAD_STATUS)
        list(GET _CM0_BSP_DOWNLOAD_STATUS 0 _CM0_BSP_DOWNLOAD_CODE)
        list(GET _CM0_BSP_DOWNLOAD_STATUS 1 _CM0_BSP_DOWNLOAD_MESSAGE)
        if(NOT _CM0_BSP_DOWNLOAD_CODE EQUAL 0)
            file(REMOVE "${_CM0_BSP_ARCHIVE}")
            message(FATAL_ERROR
                "Failed to download CM0 BSP ${CM0_BSP_VERSION} from\n"
                "  ${CM0_BSP_URL}\n"
                "${_CM0_BSP_DOWNLOAD_MESSAGE}")
        endif()

        set(_CM0_BSP_STAGING "${_CM0_BSP_RELEASE_DIR}/sysroot.extracting")
        file(REMOVE_RECURSE "${_CM0_BSP_STAGING}")
        file(MAKE_DIRECTORY "${_CM0_BSP_STAGING}")
        message(STATUS "Extracting CM0 BSP ${CM0_BSP_VERSION}")
        file(ARCHIVE_EXTRACT
            INPUT "${_CM0_BSP_ARCHIVE}"
            DESTINATION "${_CM0_BSP_STAGING}")

        if(NOT EXISTS "${_CM0_BSP_STAGING}/usr/share/cm0-bsp/toolchain.cmake" OR
           NOT EXISTS "${_CM0_BSP_STAGING}/usr/include" OR
           NOT EXISTS "${_CM0_BSP_STAGING}/usr/lib")
            file(REMOVE_RECURSE "${_CM0_BSP_STAGING}")
            message(FATAL_ERROR
                "The downloaded CM0 BSP archive does not contain a complete sysroot")
        endif()

        file(REMOVE_RECURSE "${_CM0_BSP_SYSROOT}")
        file(RENAME "${_CM0_BSP_STAGING}" "${_CM0_BSP_SYSROOT}")
    endif()
endif()

set(CM0_SDK_ROOT "${_CM0_BSP_SYSROOT}" CACHE PATH
    "Path to the CM0 BSP sysroot" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CM0_BSP_CACHE_DIR
    CM0_BSP_SHA256
    CM0_BSP_URL
    CM0_SDK_ROOT)

include("${_CM0_BSP_TOOLCHAIN}")
