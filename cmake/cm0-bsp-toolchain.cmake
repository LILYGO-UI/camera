# CM0 camera cross build. Build a complete, pinned target sysroot on macOS or
# Linux; the older BSP release lacks libcamera-dev and Debian symbol metadata.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CM0_MULTIARCH "aarch64-linux-gnu" CACHE STRING "Target Debian multiarch tuple")
get_filename_component(_camera_project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CM0_SYSROOT_CACHE_DIR "${_camera_project_root}/.cache/cm0-sysroot" CACHE PATH
    "Cache for the camera's locked ARM64 development packages")
set(CM0_SYSROOT "" CACHE PATH "Optional complete ARM64 sysroot with Debian package metadata")

if(CM0_SYSROOT)
    get_filename_component(_camera_sysroot "${CM0_SYSROOT}" ABSOLUTE)
else()
    set(_camera_lock "${CMAKE_CURRENT_LIST_DIR}/cm0-sysroot-packages.json")
    file(SHA256 "${_camera_lock}" _camera_lock_hash)
    string(SUBSTRING "${_camera_lock_hash}" 0 16 _camera_lock_id)
    set(_camera_sysroot "${CM0_SYSROOT_CACHE_DIR}/${_camera_lock_id}/sysroot")
    find_program(_camera_python NAMES python3 REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    execute_process(
        COMMAND "${_camera_python}" "${_camera_project_root}/tools/prepare-cm0-sysroot.py"
            --lock "${_camera_lock}" --output "${_camera_sysroot}"
        RESULT_VARIABLE _camera_prepare_result
        OUTPUT_VARIABLE _camera_prepare_output
        ERROR_VARIABLE _camera_prepare_error)
    if(NOT _camera_prepare_result EQUAL 0)
        message(FATAL_ERROR "${_camera_prepare_output}${_camera_prepare_error}")
    endif()
    string(STRIP "${_camera_prepare_output}" _camera_prepare_output)
    message(STATUS "${_camera_prepare_output}")
    file(READ "${_camera_lock}" _camera_lock_content)
    string(JSON _camera_package GET "${_camera_lock_content}" libcamera_package)
    string(JSON _camera_version GET "${_camera_lock_content}" libcamera_version)
    set(LIBCAMERA_DEBIAN_PACKAGE "${_camera_package}" CACHE STRING
        "Debian package matching the managed target sysroot" FORCE)
    set(LIBCAMERA_PINNED_VERSION "${_camera_version}" CACHE STRING
        "libcamera version matching the managed target sysroot" FORCE)
endif()

# Migrate existing cm0-cross builds too: find_* otherwise trusts cached paths
# into the old BSP, even after CMAKE_SYSROOT has changed.
if(CMAKE_SYSROOT AND (NOT CMAKE_SYSROOT STREQUAL _camera_sysroot OR
                     NOT CAMERA_CONFIGURED_SYSROOT STREQUAL _camera_sysroot))
    set(_camera_old_sysroot "${CMAKE_SYSROOT}")
    get_cmake_property(_camera_cached_variables CACHE_VARIABLES)
    foreach(_camera_variable IN LISTS _camera_cached_variables)
        string(FIND "${${_camera_variable}}" "${_camera_old_sysroot}" _camera_old_path)
        if(NOT _camera_old_path EQUAL -1 OR _camera_variable MATCHES "^__pkg_config_")
            unset(${_camera_variable} CACHE)
        endif()
    endforeach()
endif()
set(CAMERA_CONFIGURED_SYSROOT "${_camera_sysroot}" CACHE INTERNAL "Last configured camera sysroot" FORCE)
set(CM0_SDK_ROOT "${_camera_sysroot}" CACHE PATH "Path to the CM0 target sysroot" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CM0_SYSROOT CM0_SYSROOT_CACHE_DIR)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CM0_SDK_ROOT
    CM0_MULTIARCH)

if(NOT EXISTS "${CM0_SDK_ROOT}/usr/include" OR
   NOT EXISTS "${CM0_SDK_ROOT}/usr/lib")
    message(FATAL_ERROR
        "CM0 target sysroot is missing or incomplete: ${CM0_SDK_ROOT}\n"
        "Set CM0_SYSROOT to a complete ARM64 development sysroot.")
endif()

set(CMAKE_SYSROOT "${CM0_SDK_ROOT}" CACHE PATH
    "Sysroot used for CM0 cross builds" FORCE)
set(CMAKE_SYSROOT_COMPILE "${CMAKE_SYSROOT}" CACHE PATH
    "Compile sysroot used for CM0 cross builds" FORCE)
set(CMAKE_SYSROOT_LINK "${CMAKE_SYSROOT}" CACHE PATH
    "Link sysroot used for CM0 cross builds" FORCE)

# Homebrew's compiler does not add Debian's multiarch include automatically.
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
    "${CMAKE_SYSROOT}/usr/include/${CM0_MULTIARCH}" CACHE STRING
    "Target multiarch C system include directory" FORCE)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
    "${CMAKE_SYSROOT}/usr/include/${CM0_MULTIARCH}" CACHE STRING
    "Target multiarch C++ system include directory" FORCE)

find_program(_CM0_C_COMPILER NAMES aarch64-linux-gnu-gcc REQUIRED)
set(CMAKE_C_COMPILER "${_CM0_C_COMPILER}" CACHE FILEPATH
    "AArch64 Linux C compiler" FORCE)

find_program(_CM0_CXX_COMPILER NAMES aarch64-linux-gnu-g++)
if(_CM0_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${_CM0_CXX_COMPILER}" CACHE FILEPATH
        "AArch64 Linux C++ compiler" FORCE)
endif()

set(CMAKE_LIBRARY_ARCHITECTURE "${CM0_MULTIARCH}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" CACHE STRING
    "Root paths for cross find_* calls" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

list(PREPEND CMAKE_PREFIX_PATH
    "${CMAKE_SYSROOT}/usr"
    "${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}/cmake"
    "${CMAKE_SYSROOT}/usr/local")

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/local/lib/${CM0_MULTIARCH}/pkgconfig:${CMAKE_SYSROOT}/usr/local/lib/pkgconfig:${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/lib/${CM0_MULTIARCH}/pkgconfig")

file(GLOB _CM0_GCC_RUNTIME_DIRS LIST_DIRECTORIES true
    "${CMAKE_SYSROOT}/usr/lib/gcc/${CM0_MULTIARCH}/*")
set(_CM0_MULTIARCH_LIB_DIR "${CMAKE_SYSROOT}/usr/lib/${CM0_MULTIARCH}")
if(_CM0_GCC_RUNTIME_DIRS)
    list(SORT _CM0_GCC_RUNTIME_DIRS COMPARE NATURAL ORDER DESCENDING)
    list(GET _CM0_GCC_RUNTIME_DIRS 0 CM0_GCC_RUNTIME_DIR)
    set(CM0_GCC_RUNTIME_DIR "${CM0_GCC_RUNTIME_DIR}" CACHE PATH
        "GCC runtime directory inside the CM0 sysroot")
    set(_CM0_SYSROOT_LINK_FLAGS
        "-B${_CM0_MULTIARCH_LIB_DIR}/ -B${CM0_GCC_RUNTIME_DIR}/ -Wl,-rpath-link,${_CM0_MULTIARCH_LIB_DIR} -L${_CM0_MULTIARCH_LIB_DIR}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${_CM0_SYSROOT_LINK_FLAGS}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${_CM0_SYSROOT_LINK_FLAGS}")
endif()
