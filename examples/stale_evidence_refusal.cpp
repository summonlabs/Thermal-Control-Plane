// Thermal Control Plane — refusing stale and insufficient authority.
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
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{1};
    spec.domain = DomainRef{1};
    spec.sensor = SensorRef{1};
    spec.scope_generation = ScopeGeneration{1};
    return SyntheticFacility(FacilityId{1}, TopologyGeneration{1}, {spec});
}

}  // namespace

int main() {
    SyntheticFacility facility = make_plant();
    auto clock = std::make_unique<ManualClock>(Tick{1000});
    ManualClock* ticks = clock.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(clock));
    if (!created.has_value()) {
        return 1;
    }
    std::unique_ptr<ThermalRuntime>& runtime = created.value();
    if (!runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                 PolicyGeneration{})
             .ok()) {
        return 1;
    }
    if (!runtime->publish_limits(facility.limits_for(0, LimitGeneration{1})).ok()) {
        return 1;
    }
    if (!runtime->declare_interlock_baseline(runtime->topology_generation(), {}).ok()) {
        return 1;
    }
    if (!runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(),
                                          runtime->incarnation(), runtime->evidence_generation()))
             .has_value()) {
        return 1;
    }

    // Age the evidence well past the policy freshness window.
    ticks->advance(60000);

    AuthorityAttempt attempt;
    attempt.request = RequestId{1};
    attempt.attempt = AttemptId{1};
    attempt.key = IdempotencyKey::parse("example-stale-1").value();
    attempt.kind = AttemptKind::RequestDerate;
    attempt.binding = runtime->binding().value();
    attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
    const auto outcome = runtime->authorize(attempt);
    if (!outcome.has_value()) {
        std::printf("attempt error: %s\n", outcome.error().render().c_str());
        return 1;
    }
    std::printf("verdict %s code %s detail %s\n",
                std::string(to_string(outcome.value().verdict)).c_str(),
                std::string(to_string(outcome.value().code)).c_str(),
                outcome.value().detail.c_str());
    std::printf("directives recorded %zu outbound notices %zu\n", runtime->directive_tail(16).size(),
                runtime->outbound_size());
    static_cast<void>(runtime->close());
    return 0;
}
