cmake_minimum_required(VERSION 3.20)
project(barista_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(BARISTA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(X11 REQUIRED)
find_library(XTST_LIBRARY Xtst REQUIRED)
find_library(XDAMAGE_LIBRARY Xdamage REQUIRED)
find_package(Threads REQUIRED)

add_executable(barista-dashboard-bridge
    src/main.cpp
    ${BARISTA_SOURCE_DIR}/core/api/app_hook.cpp)
target_include_directories(barista-dashboard-bridge PRIVATE
    ${BARISTA_SOURCE_DIR}/core ${X11_INCLUDE_DIR})
target_link_libraries(barista-dashboard-bridge PRIVATE
    ${X11_LIBRARIES} ${XTST_LIBRARY} ${XDAMAGE_LIBRARY} Threads::Threads)
target_compile_options(barista-dashboard-bridge PRIVATE -Wall -Wextra -Wpedantic)

option(GAMEPAD_BUILD_PROBE "Build the cross-container AppHook validation tool" OFF)
if(GAMEPAD_BUILD_PROBE)
    add_executable(barista-media-probe tests/media_probe.cpp
        ${BARISTA_SOURCE_DIR}/core/api/app_hook.cpp)
    target_include_directories(barista-media-probe PRIVATE ${BARISTA_SOURCE_DIR}/core)
    target_link_libraries(barista-media-probe PRIVATE Threads::Threads)
endif()
