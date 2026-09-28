// Thermal Control Plane — bounded derating and gated recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <memory>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

using namespace thermal_control_plane;

int main() {
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{1};
    spec.domain = DomainRef{1};
    spec.sensor = SensorRef{1};
    spec.scope_generation = ScopeGeneration{1};
    spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
    spec.max_derate = BasisPoints::from_value_unchecked(4000);
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {spec});

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

    facility.override_temperature(0, 91000);
    if (!runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(), runtime->incarnation(),
                                          runtime->evidence_generation()))
             .has_value()) {
        return 1;
    }
    std::printf("after the excursion the mode is %s\n",
                std::string(to_string(runtime->mode())).c_str());

    AuthorityAttempt attempt;
    attempt.request = RequestId{1};
    attempt.attempt = AttemptId{1};
    attempt.key = IdempotencyKey::parse("example-derate-1").value();
    attempt.kind = AttemptKind::RequestDerate;
    attempt.authority = AuthorityClass::Operator;
    attempt.binding = runtime->binding().value();
    attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
    const auto granted = runtime->authorize(attempt);
    if (!granted.has_value()) {
        std::printf("attempt error: %s\n", granted.error().render().c_str());
        return 1;
    }
    std::printf("verdict %s granted %u bp directive %llu\n",
                std::string(to_string(granted.value().verdict)).c_str(),
                granted.value().granted_derate.value(),
                static_cast<unsigned long long>(granted.value().directive.value));

    // The plant responds to the derating; only fresh evidence can prove it.
    facility.override_temperature(0, std::nullopt);
    for (int step = 0; step < 10; ++step) {
        ticks->advance(10000);
        if (!runtime->ingest(facility.observe(ticks->peek(), BasisPoints::from_value_unchecked(3000),
                                              runtime->incarnation(),
                                              runtime->evidence_generation()))
                 .has_value()) {
            return 1;
        }
        const auto verification = runtime->verify_directive(granted.value().directive);
        if (verification.has_value()) {
            std::printf("step %d mode %s verification %s\n", step,
                        std::string(to_string(runtime->mode())).c_str(),
                        std::string(to_string(verification.value().state)).c_str());
        }
    }
    std::printf("gate satisfied %d samples %u\n",
                runtime->last_outcome().has_value() && runtime->last_outcome().value().gate.satisfied ? 1 : 0,
                runtime->last_outcome().has_value() ? runtime->last_outcome().value().gate.samples : 0U);
    static_cast<void>(runtime->close());
    return 0;
}
