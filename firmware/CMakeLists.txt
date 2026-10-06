# pocket-tank firmware — ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.8)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# the build id: the commit's short hash (+ "-dirty"), never a tag - the
# release number lives in common/version.h and is shown beside it (ESP-IDF's
# own default is `git describe --tags`, which on the tagged release commit
# would read "v0.2.0" and say the number twice; 2026-09-29)
execute_process(COMMAND git describe --always --dirty --exclude=*
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE PT_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(PT_BUILD_ID)
    set(PROJECT_VER ${PT_BUILD_ID})
endif()
project(pocket_tank)
