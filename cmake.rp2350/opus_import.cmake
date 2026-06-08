# Locate the Opus codec source tree.
#
# Resolution order (mirrors pico_sdk_import.cmake):
#   1. OPUS_PATH cache variable (already set by a previous configure)
#   2. OPUS_PATH environment variable
#   3. <ito-denwa>/vendor/opus if already populated (manual checkout)
#   4. FetchContent: git clone https://github.com/xiph/opus.git into
#      <ito-denwa>/vendor/opus at configure time
#
# The URL here is kept in sync with the entry in <ito-denwa>/.gitmodules.
# Note that the .gitmodules file is informational only — ito-denwa is not
# its own git root (libpeer is), so `git submodule update` does not see it;
# this helper is what actually populates vendor/opus.

if (DEFINED ENV{OPUS_PATH} AND (NOT OPUS_PATH))
    set(OPUS_PATH $ENV{OPUS_PATH})
    message("Using OPUS_PATH from environment ('${OPUS_PATH}')")
endif ()

set(OPUS_PATH "${OPUS_PATH}" CACHE PATH "Path to the Opus codec source tree")

if (NOT OPUS_PATH)
    set(_OPUS_LOCAL "${CMAKE_CURRENT_LIST_DIR}/../vendor/opus")
    if (EXISTS "${_OPUS_LOCAL}/CMakeLists.txt")
        set(OPUS_PATH "${_OPUS_LOCAL}")
        message("Using OPUS_PATH from ito-denwa/vendor/opus ('${OPUS_PATH}')")
    endif ()
endif ()

if (NOT OPUS_PATH)
    message("Opus not found locally — fetching from git")
    include(FetchContent)
    FetchContent_Declare(
        opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
        SOURCE_DIR     "${CMAKE_CURRENT_LIST_DIR}/../vendor/opus"
    )
    FetchContent_GetProperties(opus)
    if (NOT opus_POPULATED)
        FetchContent_Populate(opus)
    endif ()
    set(OPUS_PATH "${opus_SOURCE_DIR}")
endif ()

get_filename_component(OPUS_PATH "${OPUS_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if (NOT EXISTS "${OPUS_PATH}")
    message(FATAL_ERROR "Opus directory '${OPUS_PATH}' not found")
endif ()

set(OPUS_PATH "${OPUS_PATH}" CACHE PATH "Path to the Opus codec source tree" FORCE)
message(STATUS "Opus source: ${OPUS_PATH}")
