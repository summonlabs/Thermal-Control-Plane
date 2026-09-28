# Thermal Control Plane — runnable examples.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

set(TCPLANE_EXAMPLES
    assess_facility.cpp
    derating_and_recovery.cpp
    stale_evidence_refusal.cpp
    durable_session.cpp
)

foreach(example_source ${TCPLANE_EXAMPLES})
    get_filename_component(example_name "${example_source}" NAME_WE)
    add_executable(tcplane_example_${example_name} ${example_source})
    target_link_libraries(tcplane_example_${example_name} PRIVATE ThermalControlPlane)
    target_link_libraries(tcplane_example_${example_name} PRIVATE $<BUILD_INTERFACE:tcplane_warnings>)
    tcplane_link_asan_runtime(tcplane_example_${example_name})
endforeach()
