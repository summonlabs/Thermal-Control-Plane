# Thermal Control Plane — test suites.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# No test timeout is configured anywhere: a hanging case is a defect to
# diagnose and repair, never something to terminate.

add_library(tcplane_test_support STATIC framework.cpp process_util.cpp main.cpp)
target_include_directories(tcplane_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tcplane_test_support PUBLIC ThermalControlPlane)
target_link_libraries(tcplane_test_support PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
tcplane_link_asan_runtime(tcplane_test_support)

# Non-interactive crash/restart helper used by the multiprocess suite. It is a
# test fixture, not part of the installed product.
add_executable(tcplane_crash_child crash_child.cpp)
target_link_libraries(tcplane_crash_child PRIVATE ThermalControlPlane)
target_link_libraries(tcplane_crash_child PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
tcplane_link_asan_runtime(tcplane_crash_child)

set(TCPLANE_TEST_SOURCES
    test_quantity.cpp
    test_evidence.cpp
    test_headroom.cpp
    test_derating.cpp
    test_policy.cpp
    test_recovery.cpp
    test_authority.cpp
    test_persistence.cpp
    test_property.cpp
    test_adversarial.cpp
    test_concurrency.cpp
    test_multiprocess.cpp
    test_end_to_end.cpp
)

foreach(test_source ${TCPLANE_TEST_SOURCES})
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE tcplane_test_support)
    target_link_libraries(${test_name} PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
    target_compile_definitions(${test_name} PRIVATE
        TCPLANE_CRASH_CHILD="$<TARGET_FILE:tcplane_crash_child>")
    tcplane_link_asan_runtime(${test_name})
    add_dependencies(${test_name} tcplane_crash_child)
    add_test(NAME ${test_name} COMMAND ${test_name})
    # TIMEOUT 0 is CTest's documented "no timeout": a hanging case is a defect
    # to diagnose, never something to terminate.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 0)
endforeach()
