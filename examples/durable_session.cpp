// Thermal Control Plane — durable authority state across a restart.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <memory>
#include <string>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

using namespace thermal_control_plane;

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : std::string();
    if (path.empty()) {
        std::printf("usage: tcplane_example_durable_session <store-path>\n");
        return 64;
    }

    for (int session = 0; session < 2; ++session) {
        RuntimeOptions options;
        options.store_path = path;
        auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{100}));
        if (!created.has_value()) {
            std::printf("open failed: %s\n", created.error().render().c_str());
            return 1;
        }
        std::unique_ptr<ThermalRuntime>& runtime = created.value();
        std::printf("session %d epoch %llu revision %llu commit %llu mode %s policy %llu\n", session,
                    static_cast<unsigned long long>(runtime->epoch().value),
                    static_cast<unsigned long long>(runtime->revision().value),
                    static_cast<unsigned long long>(runtime->commit_sequence().value),
                    std::string(to_string(runtime->mode())).c_str(),
                    static_cast<unsigned long long>(runtime->policy_generation().value));
        if (session == 0) {
            if (!runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                         runtime->policy_generation())
                     .ok()) {
                return 1;
            }
            const auto binding = runtime->binding();
            if (!binding.has_value()) {
                return 1;
            }
            if (!runtime->set_administrative_mode(ThermalMode::Maintenance, true, binding.value()).ok()) {
                return 1;
            }
            // Limits are deliberately not durable: their owner republishes them.
            SyntheticZoneSpec spec;
            spec.zone = ZoneRef{1};
            spec.domain = DomainRef{1};
            spec.sensor = SensorRef{1};
            spec.scope_generation = ScopeGeneration{1};
            SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {spec});
            if (!runtime->publish_limits(facility.limits_for(0, LimitGeneration{1})).ok()) {
                return 1;
            }
        } else {
            const auto headroom = runtime->headroom();
            if (headroom.has_value()) {
                std::printf("session %d headroom %s %s\n", session,
                            std::string(to_string(headroom.value().state)).c_str(),
                            std::string(to_string(headroom.value().reason)).c_str());
            }
        }
        static_cast<void>(runtime->close());
    }
    return 0;
}
