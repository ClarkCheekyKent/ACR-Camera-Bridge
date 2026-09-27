cmake_minimum_required(VERSION 3.20)
project(ACR_UEVR_CameraBridge LANGUAGES CXX)

# Point this at a UEVR source checkout, e.g.
# cmake -S . -B build -A x64 -DUEVR_ROOT=C:/src/UEVR
set(UEVR_ROOT "" CACHE PATH "Path to the UEVR source checkout")

if(NOT EXISTS "${UEVR_ROOT}/include/uevr/Plugin.hpp")
    message(FATAL_ERROR "Set UEVR_ROOT to a UEVR checkout containing include/uevr/Plugin.hpp")
endif()

add_library(ACR_CameraBridge SHARED
    src/ACRCameraBridge.cpp
)

target_include_directories(ACR_CameraBridge PRIVATE
    "${UEVR_ROOT}/include"
)

target_compile_features(ACR_CameraBridge PRIVATE cxx_std_20)
target_compile_definitions(ACR_CameraBridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(ACR_CameraBridge PRIVATE /EHsc /permissive-)
endif()

set_target_properties(ACR_CameraBridge PROPERTIES
    OUTPUT_NAME "ACR_CameraBridge"
    PREFIX ""
)

option(BRIDGE_BUILD_TESTS "Build isolated camera and Lua tests" OFF)
if(BRIDGE_BUILD_TESTS)
    enable_testing()
    add_executable(bridge_tests tests/bridge_tests.cpp)
    target_include_directories(bridge_tests PRIVATE "${UEVR_ROOT}/include")
    target_compile_features(bridge_tests PRIVATE cxx_std_20)
    target_compile_definitions(bridge_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME camera_bridge COMMAND bridge_tests)
    if(EXISTS "${UEVR_ROOT}/dependencies/lua/src/lua.c")
        enable_language(C)
        file(GLOB LUA_SOURCES "${UEVR_ROOT}/dependencies/lua/src/*.c")
        list(FILTER LUA_SOURCES EXCLUDE REGEX "/luac\\.c$")
        add_executable(bridge_lua ${LUA_SOURCES})
        add_test(NAME lua_panel COMMAND bridge_lua "${CMAKE_CURRENT_SOURCE_DIR}/tests/panel_test.lua" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/acr_camera_bridge.lua")
    endif()
endif()
