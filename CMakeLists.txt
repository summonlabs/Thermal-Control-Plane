cmake_minimum_required(VERSION 3.21)

# Thermal Control Plane — facility-wide thermal operating authority runtime.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

project(ThermalControlPlane
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for facility-wide thermal operating authority: canonical thermal modes, generation-bound limits, headroom assessment, bounded derating, escalation precedence, recovery hysteresis and durable stale-authority fencing"
    HOMEPAGE_URL "https://github.com/summonlabs/Thermal-Control-Plane"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TCPLANE_IS_TOP_LEVEL ON)
else()
    set(TCPLANE_IS_TOP_LEVEL OFF)
endif()

option(TCPLANE_BUILD_TESTS "Build the Thermal Control Plane test suites" ${TCPLANE_IS_TOP_LEVEL})
option(TCPLANE_BUILD_EXAMPLES "Build the Thermal Control Plane examples" ${TCPLANE_IS_TOP_LEVEL})
option(TCPLANE_BUILD_BENCHMARKS "Build the Thermal Control Plane benchmarks" ${TCPLANE_IS_TOP_LEVEL})
option(TCPLANE_BUILD_TOOLS "Build the Thermal Control Plane CLI and helpers" ${TCPLANE_IS_TOP_LEVEL})
option(TCPLANE_ENABLE_ASAN "Build with MSVC AddressSanitizer" OFF)
option(TCPLANE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(TCPLANE_ENABLE_ASAN AND NOT MSVC)
    message(FATAL_ERROR "TCPLANE_ENABLE_ASAN is implemented for MSVC only")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Warning configuration
# ---------------------------------------------------------------------------
add_library(tcplane_warnings INTERFACE)
if(MSVC)
    target_compile_options(tcplane_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
    if(TCPLANE_WARNINGS_AS_ERRORS)
        target_compile_options(tcplane_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(tcplane_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(TCPLANE_WARNINGS_AS_ERRORS)
        target_compile_options(tcplane_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library. Never depends on a facility BMS, DCIM, PLC or vendor SDK.
# ---------------------------------------------------------------------------
set(TCPLANE_SOURCES
    src/version.cpp
    src/error.cpp
    src/identity.cpp
    src/quantity.cpp
    src/time.cpp
    src/evidence.cpp
    src/limits.cpp
    src/mode.cpp
    src/interlock.cpp
    src/headroom.cpp
    src/derating.cpp
    src/recovery.cpp
    src/policy.cpp
    src/wire.cpp
    src/persistence.cpp
    src/runtime.cpp
    src/runtime_decisions.cpp
    src/synthetic.cpp
)

add_library(ThermalControlPlane ${TCPLANE_SOURCES})
add_library(ThermalControlPlane::ThermalControlPlane ALIAS ThermalControlPlane)

target_include_directories(ThermalControlPlane
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# The warning interface is a build-time-only dependency: it is deliberately
# excluded from the exported target so consumers never inherit /W4 or /WX.
target_link_libraries(ThermalControlPlane PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)

target_compile_definitions(ThermalControlPlane
    PUBLIC
        TCPLANE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        TCPLANE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        TCPLANE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(ThermalControlPlane PRIVATE /bigobj)
endif()

if(TCPLANE_ENABLE_ASAN)
    message(STATUS "Thermal Control Plane: AddressSanitizer enabled")
    target_compile_options(ThermalControlPlane PUBLIC /fsanitize=address /Zi)
    target_link_options(ThermalControlPlane PUBLIC /INCREMENTAL:NO)
    get_filename_component(_tcplane_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_tcplane_cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
        set(TCPLANE_ASAN_RUNTIME_DIR "${_tcplane_cl_dir}" CACHE INTERNAL "")
        message(STATUS "Thermal Control Plane: ASan runtime found at ${_tcplane_cl_dir}")
    else()
        message(WARNING "Thermal Control Plane: clang_rt.asan_dynamic-x86_64.dll not found next to the compiler; ASan binaries may not run")
    endif()
endif()

# Copy the AddressSanitizer runtime next to every first-party executable so
# sanitizer builds run without depending on a machine-wide PATH entry.
function(tcplane_link_asan_runtime target)
    if(TCPLANE_ENABLE_ASAN AND DEFINED TCPLANE_ASAN_RUNTIME_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TCPLANE_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(TCPLANE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(TCPLANE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(TCPLANE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(TCPLANE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------
install(TARGETS ThermalControlPlane
    EXPORT ThermalControlPlaneTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY include/thermal_control_plane
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT ThermalControlPlaneTargets
    FILE ThermalControlPlaneTargets.cmake
    NAMESPACE ThermalControlPlane::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalControlPlane")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThermalControlPlaneConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalControlPlaneConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalControlPlane")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalControlPlaneConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalControlPlaneConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalControlPlaneConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalControlPlane")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ThermalControlPlane")

message(STATUS "Thermal Control Plane ${PROJECT_VERSION}: tests=${TCPLANE_BUILD_TESTS} examples=${TCPLANE_BUILD_EXAMPLES} benchmarks=${TCPLANE_BUILD_BENCHMARKS} tools=${TCPLANE_BUILD_TOOLS} asan=${TCPLANE_ENABLE_ASAN}")
