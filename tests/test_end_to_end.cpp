// Thermal Control Plane — synthetic facility end-to-end proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include <string>
#include <vector>

namespace {

using namespace thermal_control_plane;

SyntheticZoneSpec make_zone(std::uint64_t zone, std::uint64_t domain, std::uint64_t sensor) {
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{zone};
    spec.domain = DomainRef{domain};
    spec.sensor = SensorRef{sensor};
    spec.scope_generation = ScopeGeneration{1};
    spec.baseline_milli_c = 30000;
    spec.load_rise_milli_c = 45000;
    spec.derate_relief_milli_c = 60000;
    spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
    spec.max_derate = BasisPoints::from_value_unchecked(4000);
    return spec;
}

struct Facility {
    SyntheticFacility facility;
    std::unique_ptr<ThermalRuntime> runtime;
    ManualClock* clock = nullptr;

    Facility()
        : facility(FacilityId{1}, TopologyGeneration{1}, {make_zone(10, 100, 1), make_zone(11, 101, 2)}) {
        auto manual = std::make_unique<ManualClock>(Tick{1000});
        clock = manual.get();
        auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
        if (created.has_value()) {
            runtime = std::move(created.value());
        }
    }

    bool commission() {
        if (runtime == nullptr) {
            return false;
        }
        if (!runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                     PolicyGeneration{})
                 .ok()) {
            return false;
        }
        for (const ThermalLimitSet& limits : facility.all_limits(LimitGeneration{1})) {
            if (!runtime->publish_limits(limits).ok()) {
                return false;
            }
        }
        return runtime->declare_interlock_baseline(runtime->topology_generation(), {}).ok();
    }

    bool feed(std::uint32_t derate_bp) {
        return runtime
            ->ingest(facility.observe(clock->peek(), BasisPoints::from_value_unchecked(derate_bp),
                                      runtime->incarnation(), runtime->evidence_generation()))
            .has_value();
    }

    AuthorityAttempt attempt(RequestId request, AttemptId id, const std::string& key, AttemptKind kind) {
        return make_attempt(*runtime, request, id, key, kind);
    }
};

}  // namespace

TCPLANE_CASE(end_to_end, a_healthy_synthetic_facility_is_normal_and_known) {
    Facility plant;
    TCPLANE_CHECK(plant.commission());
    TCPLANE_PHASE("with full cooling demand the synthetic zones exceed the derate threshold");
    TCPLANE_CHECK(plant.feed(0));
    TCPLANE_CHECK(plant.runtime->mode() == ThermalMode::Degraded);

    TCPLANE_PHASE("the facility headroom is exact and known");
    const auto headroom = plant.runtime->headroom();
    TCPLANE_OK(headroom);
    TCPLANE_CHECK(headroom.value().state == HeadroomState::Known);
    TCPLANE_CHECK(headroom.value().has_worst);
    const auto assessment = plant.runtime->assess();
    TCPLANE_OK(assessment);
    TCPLANE_CHECK_EQ(
        assessment.value().scopes[0].by_kind[limit_index(LimitKind::Derate)].delta.milli_celsius,
        std::int64_t{10000});
    TCPLANE_CHECK_EQ(
        assessment.value().scopes[0].by_kind[limit_index(LimitKind::Warning)].delta.milli_celsius,
        std::int64_t{0});

    TCPLANE_PHASE("the synthetic temperature model is exact integer arithmetic");
    const std::int32_t derated = plant.facility.temperature(0, BasisPoints::from_value_unchecked(3000));
    TCPLANE_CHECK_EQ(derated, 57000);
    TCPLANE_PHASE("a full derating is 20000mC of relief at this model's relief rate");
    const std::int32_t full = plant.facility.temperature(0, BasisPoints::full());
    TCPLANE_CHECK_EQ(full, 15000);
}

TCPLANE_CASE(end_to_end, no_directive_is_emitted_without_sufficient_evidence) {
    Facility plant;
    TCPLANE_CHECK(plant.commission());
    TCPLANE_CHECK(plant.feed(0));

    TCPLANE_PHASE("a stale-evidence attempt is refused and emits nothing");
    plant.clock->advance(60000);
    AuthorityAttempt stale = plant.attempt(RequestId{1}, AttemptId{1}, "stale-1", AttemptKind::RequestDerate);
    stale.requested_derate = BasisPoints::from_value_unchecked(3000);
    AuthorityOutcome outcome = plant.runtime->authorize(stale).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Refused);
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::EVIDENCE_STALE));
    TCPLANE_CHECK_EQ(plant.runtime->directive_tail(64).size(), std::size_t{0});
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{0});

    TCPLANE_PHASE("a coordination request on stale evidence emits nothing either");
    AuthorityAttempt coordinate =
        plant.attempt(RequestId{2}, AttemptId{2}, "stale-2", AttemptKind::RequestCoordination);
    coordinate.coordination_kind = CoordinationKind::PowerDerateNotice;
    outcome = plant.runtime->authorize(coordinate).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Refused);
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{0});

    TCPLANE_PHASE("an interlock blocks the same attempt even with fresh evidence");
    TCPLANE_CHECK(plant.feed(0));
    Interlock interlock;
    interlock.id = InterlockId{5};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = true;
    interlock.topology_generation = plant.runtime->topology_generation();
    interlock.observed_at = plant.clock->peek();
    interlock.source = "synthetic-plant";
    TCPLANE_STATUS_OK(plant.runtime->apply_interlock(interlock));
    AuthorityAttempt blocked = plant.attempt(RequestId{3}, AttemptId{3}, "blocked", AttemptKind::RequestDerate);
    blocked.requested_derate = BasisPoints::from_value_unchecked(3000);
    outcome = plant.runtime->authorize(blocked).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::INTERLOCK_ASSERTED));
    TCPLANE_CHECK_EQ(plant.runtime->directive_tail(64).size(), std::size_t{0});
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{0});

    TCPLANE_PHASE("a generation-mismatched attempt emits nothing");
    interlock.state = InterlockState::Cleared;
    interlock.observed_at = plant.clock->peek();
    TCPLANE_STATUS_OK(plant.runtime->apply_interlock(interlock));
    AuthorityAttempt mismatched =
        plant.attempt(RequestId{4}, AttemptId{4}, "mismatch", AttemptKind::RequestDerate);
    mismatched.binding.evidence = EvidenceGeneration{mismatched.binding.evidence.value + 3};
    mismatched.requested_derate = BasisPoints::from_value_unchecked(3000);
    outcome = plant.runtime->authorize(mismatched).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::FUTURE_EVIDENCE_GENERATION));
    TCPLANE_CHECK_EQ(plant.runtime->directive_tail(64).size(), std::size_t{0});
}

TCPLANE_CASE(end_to_end, derating_and_recovery_walk_the_whole_mode_ladder) {
    Facility plant;
    TCPLANE_CHECK(plant.commission());
    TCPLANE_CHECK(plant.feed(0));
    TCPLANE_CHECK(plant.runtime->mode() == ThermalMode::Degraded);

    TCPLANE_PHASE("a bounded derating directive is granted and recorded");
    AuthorityAttempt request = plant.attempt(RequestId{1}, AttemptId{1}, "derate-1", AttemptKind::RequestDerate);
    request.authority = AuthorityClass::Operator;
    request.requested_derate = BasisPoints::from_value_unchecked(3000);
    const AuthorityOutcome granted = plant.runtime->authorize(request).value();
    TCPLANE_CHECK(granted.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(granted.granted_derate <= BasisPoints::from_value_unchecked(3000));
    TCPLANE_CHECK_EQ(plant.runtime->directive_tail(64).size(), std::size_t{1});

    TCPLANE_PHASE("the synthetic plant responds to the derating and the directive is proven");
    TCPLANE_CHECK(plant.feed(3000));
    plant.clock->advance(10);
    const auto proven = plant.runtime->verify_directive(granted.directive);
    TCPLANE_OK(proven);
    TCPLANE_CHECK(proven.value().state == VerificationState::Proven);

    TCPLANE_PHASE("acknowledging the directive is recorded and idempotent");
    AuthorityAttempt acknowledgement =
        plant.attempt(RequestId{2}, AttemptId{2}, "ack-1", AttemptKind::AcknowledgeDirective);
    acknowledgement.directive = granted.directive;
    TCPLANE_CHECK(plant.runtime->authorize(acknowledgement).value().verdict == AuthorityVerdict::Granted);
    AuthorityAttempt repeated =
        plant.attempt(RequestId{3}, AttemptId{3}, "ack-2", AttemptKind::AcknowledgeDirective);
    repeated.directive = granted.directive;
    TCPLANE_CHECK(plant.runtime->authorize(repeated).value().verdict == AuthorityVerdict::Granted);

    TCPLANE_PHASE("the facility steps down one rung at a time as evidence stays favourable");
    const std::vector<ThermalMode> expected{ThermalMode::Constrained, ThermalMode::Recovery,
                                            ThermalMode::Normal};
    for (const ThermalMode target : expected) {
        ThermalMode previous = plant.runtime->mode();
        for (int observation = 0; observation < 40 && plant.runtime->mode() != target; ++observation) {
            plant.clock->advance(20000);
            TCPLANE_CHECK(plant.feed(3000));
            const ThermalMode now = plant.runtime->mode();
            if (now != previous) {
                // De-escalation moves exactly one rung and never skips.
                TCPLANE_CHECK(now == step_down(previous));
                previous = now;
            }
        }
        TCPLANE_CHECK(plant.runtime->mode() == target);
    }
    TCPLANE_CHECK(plant.runtime->mode() == ThermalMode::Normal);

    TCPLANE_PHASE("a coordination notice is only emitted on an explicit granted request");
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{0});
    AuthorityAttempt coordinate =
        plant.attempt(RequestId{10}, AttemptId{10}, "coord-1", AttemptKind::RequestCoordination);
    coordinate.coordination_kind = CoordinationKind::PlacementConstraint;
    const AuthorityOutcome emitted = plant.runtime->authorize(coordinate).value();
    TCPLANE_CHECK(emitted.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(emitted.coordination.has_value());
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{1});
    const std::vector<CoordinationRequest> notices = plant.runtime->drain_outbound();
    TCPLANE_CHECK_EQ(notices.size(), std::size_t{1});
    TCPLANE_CHECK(notices[0].kind == CoordinationKind::PlacementConstraint);
    TCPLANE_CHECK(notices[0].mode == ThermalMode::Normal);
    TCPLANE_CHECK_EQ(plant.runtime->outbound_size(), std::size_t{0});

    TCPLANE_PHASE("a coordination request is refused when the outbound queue is full");
    for (int index = 0; index < 300; ++index) {
        AuthorityAttempt burst = plant.attempt(RequestId{static_cast<std::uint64_t>(100 + index)},
                                              AttemptId{static_cast<std::uint64_t>(100 + index)},
                                              "burst-" + std::to_string(index),
                                              AttemptKind::RequestCoordination);
        burst.coordination_kind = CoordinationKind::ThermalStateNotice;
        const AuthorityOutcome burst_outcome = plant.runtime->authorize(burst).value();
        if (burst_outcome.code == ErrorCode::RESOURCE_EXHAUSTED) {
            TCPLANE_CHECK(plant.runtime->outbound_size() <= 256);
            return;
        }
    }
    TCPLANE_FAIL("the outbound coordination bound was never enforced");
}

TCPLANE_CASE(end_to_end, a_single_favourable_sampling_never_de_escalates) {
    Facility plant;
    TCPLANE_CHECK(plant.commission());
    TCPLANE_PHASE("a critical excursion escalates to emergency");
    for (std::size_t index = 0; index < plant.facility.zone_count(); ++index) {
        plant.facility.override_temperature(index, 97000);
    }
    TCPLANE_CHECK(plant.feed(0));
    TCPLANE_CHECK(plant.runtime->mode() == ThermalMode::Emergency);

    TCPLANE_PHASE("one favourable observation leaves the facility in emergency");
    for (std::size_t index = 0; index < plant.facility.zone_count(); ++index) {
        plant.facility.override_temperature(index, 50000);
    }
    TCPLANE_CHECK(plant.feed(0));
    TCPLANE_CHECK(plant.runtime->mode() == ThermalMode::Emergency);

    TCPLANE_PHASE("repeated favourable observations eventually step down exactly one rung");
    bool stepped = false;
    for (int attempt = 0; attempt < 40 && !stepped; ++attempt) {
        plant.clock->advance(20000);
        TCPLANE_CHECK(plant.feed(0));
        stepped = plant.runtime->mode() == ThermalMode::Degraded;
    }
    TCPLANE_CHECK(stepped);
    TCPLANE_CHECK(plant.runtime->mode() != ThermalMode::Normal);
}
