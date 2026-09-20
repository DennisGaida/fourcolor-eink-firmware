# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# No hand-maintained version constant here: it's derived from
# `git describe --tags --match "v[0-9]*"` instead, restricted to firmware
# release tags (vX.Y.Z) so it can't accidentally resolve to a bridge-vX.Y.Z
# tag (see .github/workflows/build-bridge.yml - the server/ image has its own
# independent version namespace). CI (.github/workflows/build-firmware.yml)
# computes and pushes the next vX.Y.Z tag from Conventional Commits before
# building, so release builds embed a clean "X.Y.Z"; other builds embed a
# descriptive "X.Y.Z-N-gHASH[-dirty]" string instead.
execute_process(
    COMMAND git describe --tags --always --dirty --match "v[0-9]*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_DESCRIBE_RESULT
)
if(GIT_DESCRIBE_RESULT EQUAL 0 AND GIT_PROJECT_VER)
    string(REGEX REPLACE "^v" "" GIT_PROJECT_VER "${GIT_PROJECT_VER}")
    set(PROJECT_VER "${GIT_PROJECT_VER}")
else()
    set(PROJECT_VER "0.0.0-unknown")
endif()

# Local project components under main/components
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/components")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/compat/cxx_math_compat.h>")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)
