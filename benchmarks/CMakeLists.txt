# Thermal Control Plane — completed-operation benchmark.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(tcplane_bench thermal_control_plane_bench.cpp)
target_link_libraries(tcplane_bench PRIVATE ThermalControlPlane)
target_link_libraries(tcplane_bench PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
tcplane_link_asan_runtime(tcplane_bench)
