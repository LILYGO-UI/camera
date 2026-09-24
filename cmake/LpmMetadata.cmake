set(_LILYGO_UI_PROJECT_CONFIG
    "${CMAKE_CURRENT_LIST_DIR}/../lpm.toml")
if(NOT EXISTS "${_LILYGO_UI_PROJECT_CONFIG}")
    message(FATAL_ERROR
        "Missing LPM project configuration: ${_LILYGO_UI_PROJECT_CONFIG}")
endif()

if(LPM_EXECUTABLE AND NOT EXISTS "${LPM_EXECUTABLE}")
    unset(LPM_EXECUTABLE CACHE)
    unset(LPM_EXECUTABLE)
endif()
find_program(LPM_EXECUTABLE NAMES lpm
    HINTS "${CMAKE_CURRENT_LIST_DIR}/../.venv/bin"
    DOC "LILYGO package manager executable")
if(NOT LPM_EXECUTABLE)
    message(FATAL_ERROR
        "lpm is required to read project metadata; install lilygo-lpm or set LPM_EXECUTABLE")
endif()

execute_process(
    COMMAND "${LPM_EXECUTABLE}" metadata --format cmake
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    RESULT_VARIABLE _LILYGO_UI_METADATA_RESULT
    OUTPUT_VARIABLE _LILYGO_UI_METADATA_CONTENT
    ERROR_VARIABLE _LILYGO_UI_METADATA_ERROR)
if(NOT _LILYGO_UI_METADATA_RESULT EQUAL 0)
    string(STRIP "${_LILYGO_UI_METADATA_ERROR}" _LILYGO_UI_METADATA_ERROR)
    message(FATAL_ERROR
        "Could not read lpm.toml with lpm "
        "(${_LILYGO_UI_METADATA_RESULT}): ${_LILYGO_UI_METADATA_ERROR}")
endif()

set(_LILYGO_UI_METADATA_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_LILYGO_UI_METADATA_FILE
    "${_LILYGO_UI_METADATA_DIR}/lpm_metadata.cmake")
file(MAKE_DIRECTORY "${_LILYGO_UI_METADATA_DIR}")
file(WRITE "${_LILYGO_UI_METADATA_FILE}"
    "${_LILYGO_UI_METADATA_CONTENT}")
include("${_LILYGO_UI_METADATA_FILE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_LILYGO_UI_PROJECT_CONFIG}")

function(_lilygo_ui_escape_json output input)
    set(_value "${input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(_lilygo_ui_escape_xml output input)
    set(_value "${input}")
    string(REPLACE "&" "&amp;" _value "${_value}")
    string(REPLACE "<" "&lt;" _value "${_value}")
    string(REPLACE ">" "&gt;" _value "${_value}")
    string(REPLACE "\"" "&quot;" _value "${_value}")
    string(REPLACE "'" "&apos;" _value "${_value}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(_lilygo_ui_escape_desktop output input)
    set(_value "${input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(_lilygo_ui_escape_c output input)
    set(_value "${input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

set(_desktop_categories)
foreach(_category IN LISTS LILYGO_UI_CATEGORIES)
    if(_category STREQUAL "Utilities")
        set(_category "Utility")
    elseif(_category STREQUAL "Games")
        set(_category "Game")
    elseif(_category STREQUAL "Multimedia")
        set(_category "AudioVideo")
    elseif(_category STREQUAL "Lifestyle")
        set(_category "Utility")
    endif()
    list(APPEND _desktop_categories "${_category}")
endforeach()
list(JOIN _desktop_categories ";" LILYGO_UI_DESKTOP_CATEGORIES)
string(APPEND LILYGO_UI_DESKTOP_CATEGORIES ";")

_lilygo_ui_escape_json(LILYGO_UI_APP_ID_JSON "${LILYGO_UI_APP_ID}")
_lilygo_ui_escape_json(LILYGO_UI_APP_NAME_JSON "${LILYGO_UI_APP_NAME}")
_lilygo_ui_escape_json(LILYGO_UI_APP_SUMMARY_JSON "${LILYGO_UI_APP_SUMMARY}")
_lilygo_ui_escape_json(LILYGO_UI_EXECUTABLE_NAME_JSON "${LILYGO_UI_EXECUTABLE_NAME}")

_lilygo_ui_escape_xml(LILYGO_UI_APP_ID_XML "${LILYGO_UI_APP_ID}")
_lilygo_ui_escape_xml(LILYGO_UI_APP_NAME_XML "${LILYGO_UI_APP_NAME}")
_lilygo_ui_escape_xml(LILYGO_UI_APP_SUMMARY_XML "${LILYGO_UI_APP_SUMMARY}")
_lilygo_ui_escape_xml(LILYGO_UI_APP_DESCRIPTION_XML "${LILYGO_UI_APP_DESCRIPTION}")
_lilygo_ui_escape_xml(LILYGO_UI_APP_LICENSE_XML "${LILYGO_UI_APP_LICENSE}")

_lilygo_ui_escape_desktop(LILYGO_UI_APP_NAME_DESKTOP "${LILYGO_UI_APP_NAME}")
_lilygo_ui_escape_desktop(LILYGO_UI_APP_SUMMARY_DESKTOP "${LILYGO_UI_APP_SUMMARY}")
_lilygo_ui_escape_c(LILYGO_UI_APP_ID_C "${LILYGO_UI_APP_ID}")
_lilygo_ui_escape_c(LILYGO_UI_APP_NAME_C "${LILYGO_UI_APP_NAME}")
