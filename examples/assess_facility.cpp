// Thermal Control Plane — assessing a synthetic facility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <memory>
#include <vector>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

using namespace thermal_control_plane;

namespace {

SyntheticFacility make_plant() {
    std::vector<SyntheticZoneSpec> zones;
    for (std::uint64_t index = 1; index <= 2; ++index) {
        SyntheticZoneSpec spec;
        spec.zone = ZoneRef{index};
        spec.domain = DomainRef{index};
        spec.sensor = SensorRef{index};
        spec.scope_generation = ScopeGeneration{1};
        spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
        spec.max_derate = BasisPoints::from_value_unchecked(4000);
        zones.push_back(spec);
    }
    return SyntheticFacility(FacilityId{1}, TopologyGeneration{1}, zones);
}

}  // namespace

int main() {
    // SYNTHETIC: a made-up two-zone facility. No hardware participates.
    SyntheticFacility facility = make_plant();
    auto clock = std::make_unique<ManualClock>(Tick{1000});
    ManualClock* ticks = clock.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(clock));
    if (!created.has_value()) {
        std::printf("runtime creation failed: %s\n", created.error().render().c_str());
        return 1;
    }
    std::unique_ptr<ThermalRuntime>& runtime = created.value();
    if (!runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                 PolicyGeneration{})
             .ok()) {
        return 1;
    }
    for (const ThermalLimitSet& limits : facility.all_limits(LimitGeneration{1})) {
        if (!runtime->publish_limits(limits).ok()) {
            return 1;
        }
    }
    // No authority is granted until the interlock owner states what is asserted.
    if (!runtime->declare_interlock_baseline(runtime->topology_generation(), {}).ok()) {
        return 1;
    }

    // Drive one zone above the derate threshold.
    facility.override_temperature(0, 91000);
    const auto report = runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(),
                                                        runtime->incarnation(),
                                                        runtime->evidence_generation()));
    if (!report.has_value()) {
        std::printf("ingest failed: %s\n", report.error().render().c_str());
        return 1;
    }

    const auto outcome = runtime->assess();
    if (!outcome.has_value()) {
        std::printf("assessment failed: %s\n", outcome.error().render().c_str());
        return 1;
    }
    std::printf("mode              %s\n", std::string(to_string(outcome.value().mode)).c_str());
    std::printf("required mode     %s\n", std::string(to_string(outcome.value().required_mode)).c_str());
    std::printf("primary cause     %s\n", std::string(to_string(outcome.value().reason.cause)).c_str());
    std::printf("headroom          %s %s\n",
                std::string(to_string(outcome.value().facility.state)).c_str(),
                std::string(to_string(outcome.value().facility.reason)).c_str());
    std::printf("required derate   %u bp\n", outcome.value().required_derate.value());
    std::printf("envelope ceiling  %d mC (defined=%d)\n", outcome.value().envelope.ceiling.milli_celsius,
                outcome.value().envelope.ceiling_defined ? 1 : 0);
    static_cast<void>(runtime->close());
    return 0;
}
