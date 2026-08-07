cmake_minimum_required(VERSION 3.31)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_VERSION_MAJOR 4)
set(PROJECT_VERSION_MINOR 10)
set(DOG_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.2")

execute_process(
    COMMAND echo `git describe --always --dirty --broken`_`git branch --show-current`
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_REV_DOG
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND git describe --always --dirty --broken
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/include/libchess/
    OUTPUT_VARIABLE GIT_REV_LC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND git describe --always --dirty --broken
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/fathom
    OUTPUT_VARIABLE GIT_REV_FATHOM
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# ESP32 with one of: WEMOS32, ESP32_S3_QTPY, ESP32_S3_XIAO, ESP32_S3_WAVESHARE
# NOTE: append only once. idf.py build re-runs cmake on every invocation, and an
# unconditional append would accumulate duplicate -D flags (e.g. two -DGIT_REV_LC)
# which then fail under -Werror. Guard so reconfigures don't duplicate.
if(NOT CMAKE_CXX_FLAGS MATCHES "-DGIT_REV_LC=")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDOG_VERSION=\"\\\"${DOG_VERSION}\\\"\" -DESP32 -DESP32_S3_XIAO -DGIT_REV_DOG=\"\\\"${GIT_REV_DOG}\\\"\" -DGIT_REV_LC=\"\\\"${GIT_REV_LC}\\\"\" -DGIT_REV_FATHOM=\"\\\"${GIT_REV_FATHOM}\\\"\" -DBUILD_TYPE=\"\\\"${CMAKE_BUILD_TYPE}\\\"\" -DBUILD_TARGET=\"\\\"regular\\\"\"")
endif()

project(esp32chesstest)

idf_build_set_property(MINIMAL_BUILD ON)
