// Thermal Control Plane — independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

// An out-of-tree consumer of the installed Thermal Control Plane package.
//
// It performs a real library lifecycle through the exported target only: it
// configures a facility, feeds evidence, evaluates the policy, obtains a
// bounded derating directive and checks that a directive is not proof of a
// physical change.

#include <cstdio>
#include <memory>

#include <thermal_control_plane/runtime.hpp>
#include <thermal_control_plane/synthetic.hpp>
#include <thermal_control_plane/version.hpp>

using namespace thermal_control_plane;

int main() {
    std::printf("Thermal Control Plane consumer, library version %s\n", version_string());

    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{1};
    spec.domain = DomainRef{1};
    spec.sensor = SensorRef{1};
    spec.scope_generation = ScopeGeneration{1};
    spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
    spec.max_derate = BasisPoints::from_value_unchecked(4000);
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {spec});

    auto clock = std::make_unique<ManualClock>(Tick{500});
    ManualClock* ticks = clock.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(clock));
    if (!created.has_value()) {
        std::printf("runtime creation failed: %s\n", created.error().render().c_str());
        return 1;
    }
    std::unique_ptr<ThermalRuntime>& runtime = created.value();

    const Status policy = runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                                  PolicyGeneration{});
    if (!policy.ok()) {
        std::printf("policy install failed: %s\n", policy.error().render().c_str());
        return 1;
    }
    const Status limits = runtime->publish_limits(facility.limits_for(0, LimitGeneration{1}));
    if (!limits.ok()) {
        std::printf("limit publish failed: %s\n", limits.error().render().c_str());
        return 1;
    }
    const Status baseline = runtime->declare_interlock_baseline(runtime->topology_generation(), {});
    if (!baseline.ok()) {
        std::printf("interlock baseline failed: %s\n", baseline.error().render().c_str());
        return 1;
    }

    facility.override_temperature(0, 91000);
    const auto report = runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(),
                                                        runtime->incarnation(),
                                                        runtime->evidence_generation()));
    if (!report.has_value()) {
        std::printf("ingest failed: %s\n", report.error().render().c_str());
        return 1;
    }

    const auto assessment = runtime->assess();
    if (!assessment.has_value()) {
        std::printf("assessment failed: %s\n", assessment.error().render().c_str());
        return 1;
    }
    std::printf("mode %s required %s cause %s required derate %u bp\n",
                std::string(to_string(assessment.value().mode)).c_str(),
                std::string(to_string(assessment.value().required_mode)).c_str(),
                std::string(to_string(assessment.value().reason.cause)).c_str(),
                assessment.value().required_derate.value());

    AuthorityAttempt attempt;
    attempt.request = RequestId{1};
    attempt.attempt = AttemptId{1};
    attempt.key = IdempotencyKey::parse("consumer-1").value();
    attempt.kind = AttemptKind::RequestDerate;
    attempt.binding = runtime->binding().value();
    attempt.requested_derate = assessment.value().required_derate;
    const auto outcome = runtime->authorize(attempt);
    if (!outcome.has_value()) {
        std::printf("authorize failed: %s\n", outcome.error().render().c_str());
        return 1;
    }
    std::printf("verdict %s directive %llu granted %u bp\n",
                std::string(to_string(outcome.value().verdict)).c_str(),
                static_cast<unsigned long long>(outcome.value().directive.value),
                outcome.value().granted_derate.value());
    if (outcome.value().verdict != AuthorityVerdict::Granted) {
        std::printf("expected a granted directive, got %s\n",
                    std::string(to_string(outcome.value().code)).c_str());
        return 1;
    }

    const auto verification = runtime->verify_directive(outcome.value().directive);
    if (!verification.has_value()) {
        std::printf("verification failed: %s\n", verification.error().render().c_str());
        return 1;
    }
    if (verification.value().state == VerificationState::Proven) {
        std::printf("a directive was treated as proof of a physical change\n");
        return 1;
    }
    std::printf("verification %s (a directive is not proof of a physical change)\n",
                std::string(to_string(verification.value().state)).c_str());

    const Status closed = runtime->close();
    if (!closed.ok()) {
        std::printf("close failed: %s\n", closed.error().render().c_str());
        return 1;
    }
    std::printf("consumer completed successfully\n");
    return 0;
}
