# Thermal Control Plane — administration and inspection CLI.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(tcp_cli tcp_cli.cpp)
target_link_libraries(tcp_cli PRIVATE ThermalControlPlane)
target_link_libraries(tcp_cli PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
tcplane_link_asan_runtime(tcp_cli)

install(TARGETS tcp_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
