# Thermal Control Plane — independent downstream consumer.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately NOT part of the Thermal Control Plane build. It
# exists to prove that an unrelated downstream project can consume only the
# installed artifacts:
#
#   cmake -S consumer -B <build> -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <build>
#   <build>/tcplane_consumer
#
# It resolves ThermalControlPlane through find_package(CONFIG) alone, with no
# reference to the source tree, the build tree or any private header.

cmake_minimum_required(VERSION 3.21)
project(ThermalControlPlaneConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ThermalControlPlane CONFIG REQUIRED)

add_executable(tcplane_consumer consumer.cpp)

if(MSVC)
    target_compile_options(tcplane_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()

target_link_libraries(tcplane_consumer PRIVATE ThermalControlPlane::ThermalControlPlane)

# Fail loudly if the public include path ever leaks the source tree.
get_target_property(_tcplane_includes ThermalControlPlane::ThermalControlPlane INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _tcplane_includes)
    message(FATAL_ERROR "ThermalControlPlane::ThermalControlPlane exposes no include directories")
endif()
message(STATUS "ThermalControlPlane consumer include directories: ${_tcplane_includes}")
message(STATUS "ThermalControlPlane consumer version: ${ThermalControlPlane_VERSION}")
