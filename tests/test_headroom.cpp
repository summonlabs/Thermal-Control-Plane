// Thermal Control Plane — headroom arithmetic and unknown semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

const ScopeRef kScope = make_scope(1, 10, 100);
const SensorRef kSensor{1};
const FreshnessWindow kWindow{Duration::from_millis_unchecked(1000)};

ScopeHeadroom headroom_for(EvidenceStore& store, const ThermalLimitSet* limits, Tick at) {
    return evaluate_headroom(limits, store.view(kScope, at, kWindow));
}

}  // namespace

TCPLANE_CASE(headroom, exact_arithmetic) {
    TCPLANE_PHASE("headroom is limit minus observation, exactly");
    Threshold limit;
    limit.defined = true;
    limit.milli_celsius = 85000;
    const Headroom positive = compute_headroom(Temperature::from_milli_celsius_unchecked(60000), limit,
                                               LimitKind::Derate);
    TCPLANE_CHECK(positive.known());
    TCPLANE_CHECK_EQ(positive.delta.milli_celsius, std::int64_t{25000});
    TCPLANE_CHECK(!positive.over_limit);

    TCPLANE_PHASE("a temperature exactly at the limit has zero headroom and is over it");
    const Headroom at_limit = compute_headroom(Temperature::from_milli_celsius_unchecked(85000), limit,
                                               LimitKind::Derate);
    TCPLANE_CHECK(at_limit.known());
    TCPLANE_CHECK_EQ(at_limit.delta.milli_celsius, std::int64_t{0});
    TCPLANE_CHECK(at_limit.over_limit);

    TCPLANE_PHASE("an exceeded limit produces negative headroom, never a clamp");
    const Headroom negative = compute_headroom(Temperature::from_milli_celsius_unchecked(90000), limit,
                                               LimitKind::Derate);
    TCPLANE_CHECK_EQ(negative.delta.milli_celsius, std::int64_t{-5000});
    TCPLANE_CHECK(negative.over_limit);

    TCPLANE_PHASE("an undefined limit never yields a number");
    Threshold undefined;
    const Headroom none = compute_headroom(Temperature::from_milli_celsius_unchecked(10000), undefined,
                                           LimitKind::Derate);
    TCPLANE_CHECK(!none.known());
    TCPLANE_CHECK(none.reason == HeadroomReason::LimitUndefined);
    TCPLANE_CHECK_EQ(none.delta.milli_celsius, std::int64_t{0});
}

TCPLANE_CASE(headroom, missing_evidence_is_never_safe) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);

    TCPLANE_PHASE("a scope with no sensors has unknown headroom, not unlimited headroom");
    const ScopeHeadroom empty = headroom_for(store, &limits, Tick{10});
    TCPLANE_CHECK(!empty.governing.known());
    TCPLANE_CHECK(empty.governing.reason == HeadroomReason::NoEvidence);
    TCPLANE_CHECK_EQ(empty.governing.delta.milli_celsius, std::int64_t{0});

    TCPLANE_PHASE("a faulted sensor leaves headroom unknown rather than large");
    TemperatureObservation faulted = make_observation(kScope, kSensor, Tick{10}, 0, ObservationSequence{1});
    faulted.quality = ObservationQuality::Unavailable;
    faulted.reason = EvidenceReason::SensorFaulted;
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {faulted}), Tick{10}));
    const ScopeHeadroom broken = headroom_for(store, &limits, Tick{10});
    TCPLANE_CHECK(!broken.governing.known());
    TCPLANE_CHECK(broken.governing.reason == HeadroomReason::SensorFaulted);

    TCPLANE_PHASE("stale evidence leaves headroom unknown");
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{2}, TopologyGeneration{1}));
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{2}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensor, Tick{10}, 60000,
                                                                ObservationSequence{1}, EvidenceGeneration{2})}),
                                   Tick{10}));
    const ScopeHeadroom fresh = headroom_for(store, &limits, Tick{20});
    TCPLANE_CHECK(fresh.governing.known());
    const ScopeHeadroom stale = headroom_for(store, &limits, Tick{9000});
    TCPLANE_CHECK(!stale.governing.known());
    TCPLANE_CHECK(stale.governing.reason == HeadroomReason::EvidenceStale);

    TCPLANE_PHASE("a derived observation is never authority");
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{3}, TopologyGeneration{1}));
    TemperatureObservation derived = make_observation(kScope, kSensor, Tick{10}, 60000, ObservationSequence{1},
                                                      EvidenceGeneration{3});
    derived.quality = ObservationQuality::Derived;
    derived.reason = EvidenceReason::NotSupported;
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{3}, TopologyGeneration{1}, {derived}), Tick{10}));
    const ScopeHeadroom estimated = headroom_for(store, &limits, Tick{10});
    TCPLANE_CHECK(!estimated.governing.known());
}

TCPLANE_CASE(headroom, governing_kind_and_generation_mismatch) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);

    TCPLANE_PHASE("the governing headroom is the tightest defined threshold");
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensor, Tick{10}, 84000,
                                                                ObservationSequence{1})}),
                                   Tick{10}));
    const ScopeHeadroom tight = headroom_for(store, &limits, Tick{10});
    TCPLANE_CHECK(tight.governing.known());
    // The tightest declared threshold governs, which here is the advisory band.
    TCPLANE_CHECK(tight.governing.basis == LimitKind::Advisory);
    TCPLANE_CHECK_EQ(tight.governing.delta.milli_celsius, std::int64_t{-44000});
    TCPLANE_CHECK(tight.governing.over_limit);
    TCPLANE_PHASE("with the advisory band absent the warning band governs");
    const ThermalLimitSet no_advisory = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);
    ThermalLimitSet sparse = no_advisory;
    sparse[LimitKind::Advisory].defined = false;
    sparse[LimitKind::Advisory].milli_celsius = 0;
    const ScopeHeadroom warning_governs = headroom_for(store, &sparse, Tick{10});
    TCPLANE_CHECK(warning_governs.governing.known());
    TCPLANE_CHECK(warning_governs.governing.basis == LimitKind::Warning);
    TCPLANE_CHECK_EQ(warning_governs.governing.delta.milli_celsius, std::int64_t{-9000});

    TCPLANE_PHASE("evidence taken against another scope generation cannot be compared with these limits");
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{2}, TopologyGeneration{1}));
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{2}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensor, Tick{10}, 50000,
                                                                ObservationSequence{1}, EvidenceGeneration{2},
                                                                ScopeGeneration{7})}),
                                   Tick{10}));
    const ScopeHeadroom mismatched = headroom_for(store, &limits, Tick{10});
    TCPLANE_CHECK(!mismatched.governing.known());
    TCPLANE_CHECK(mismatched.governing.reason == HeadroomReason::GenerationMismatch);
}

TCPLANE_CASE(headroom, facility_aggregation_never_reassures) {
    const ScopeRef first = make_scope(1, 10, 100);
    const ScopeRef second = make_scope(1, 11, 100);
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(first, kSensor, Tick{10}, 50000,
                                                                ObservationSequence{1}),
                                               make_observation(second, SensorRef{2}, Tick{10}, 60000,
                                                                ObservationSequence{1})}),
                                   Tick{10}));

    const ThermalLimitSet first_limits = make_limits(first, 70000, 75000, 85000, 95000, 105000);
    const ThermalLimitSet second_limits = make_limits(second, 70000, 75000, 85000, 95000, 105000);
    std::vector<ScopeHeadroom> scopes;
    scopes.push_back(evaluate_headroom(&first_limits, store.view(first, Tick{10}, kWindow)));
    scopes.push_back(evaluate_headroom(&second_limits, store.view(second, Tick{10}, kWindow)));

    TCPLANE_PHASE("with every scope known the facility reports the tightest delta");
    FacilityHeadroom known = aggregate_headroom(scopes);
    TCPLANE_CHECK(known.state == HeadroomState::Known);
    TCPLANE_CHECK_EQ(known.worst_delta.milli_celsius, std::int64_t{10000});
    TCPLANE_CHECK(known.worst_scope == second);

    TCPLANE_PHASE("one unknown scope makes the whole facility unknown");
    ScopeEvidenceView blind;
    blind.scope = second;
    scopes[1] = evaluate_headroom(&second_limits, blind);
    FacilityHeadroom unknown = aggregate_headroom(scopes);
    TCPLANE_CHECK(unknown.state != HeadroomState::Known);
    TCPLANE_CHECK(!unknown.has_worst);
    TCPLANE_CHECK_EQ(unknown.worst_delta.milli_celsius, std::int64_t{0});

    TCPLANE_PHASE("a facility with no published limits has no headroom at all");
    const std::vector<ScopeHeadroom> nothing;
    const FacilityHeadroom none = aggregate_headroom(nothing);
    TCPLANE_CHECK(none.state != HeadroomState::Known);
    TCPLANE_CHECK(none.reason == HeadroomReason::LimitUndefined);
}

TCPLANE_CASE(headroom, revalidation_marks_pre_recovery_evidence) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensor, Tick{10}, 60000,
                                                                ObservationSequence{1})}),
                                   Tick{10}));
    TCPLANE_CHECK(headroom_for(store, &limits, Tick{10}).governing.known());

    TCPLANE_PHASE("evidence stamped before a recovery point is not current");
    store.require_revalidation(Tick{20});
    const ScopeHeadroom after = headroom_for(store, &limits, Tick{20});
    TCPLANE_CHECK(!after.governing.known());
    TCPLANE_CHECK(after.governing.reason == HeadroomReason::AwaitingRevalidation);

    TCPLANE_PHASE("a post-recovery observation restores authority");
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensor, Tick{25}, 60000,
                                                                ObservationSequence{2})}),
                                   Tick{25}));
    TCPLANE_CHECK(headroom_for(store, &limits, Tick{25}).governing.known());
}
