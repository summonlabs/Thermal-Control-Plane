// Thermal Control Plane — derating selection proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

const ScopeRef kScope = make_scope(1, 10, 100);
const SensorRef kSensor{1};

DeratingLadder default_ladder() {
    DeratingLadder ladder;
    ladder[LimitKind::Advisory] = BasisPoints::from_value_unchecked(0);
    ladder[LimitKind::Warning] = BasisPoints::from_value_unchecked(500);
    ladder[LimitKind::Derate] = BasisPoints::from_value_unchecked(1500);
    ladder[LimitKind::Critical] = BasisPoints::from_value_unchecked(2500);
    ladder[LimitKind::Shutdown] = BasisPoints::from_value_unchecked(3000);
    return ladder;
}

ScopeHeadroom scope_at(const ThermalLimitSet& limits, std::int32_t milli_celsius) {
    ScopeHeadroom scope;
    scope.scope = kScope;
    scope.has_limits = true;
    scope.has_hottest = true;
    scope.hottest = Temperature::from_milli_celsius_unchecked(milli_celsius);
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        scope.by_kind[i] =
            compute_headroom(scope.hottest, limits.thresholds[i], static_cast<LimitKind>(i + 1));
    }
    scope.governing = scope.by_kind[limit_index(LimitKind::Advisory)];
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        if (scope.by_kind[i].known() &&
            (scope.governing.basis == LimitKind::Advisory ||
             scope.by_kind[i].delta.milli_celsius < scope.governing.delta.milli_celsius)) {
            scope.governing = scope.by_kind[i];
        }
    }
    return scope;
}

}  // namespace

TCPLANE_CASE(derating, ladder_selection_follows_the_highest_violated_kind) {
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000, 4000);
    const DeratingLadder ladder = default_ladder();
    const BasisPoints ceiling = BasisPoints::from_value_unchecked(3000);

    TCPLANE_PHASE("below every threshold nothing is derated");
    DerateSelection selection = select_derating(ladder, ceiling, ceiling, scope_at(limits, 30000), &limits);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{0});
    TCPLANE_CHECK(!selection.basis_defined);

    TCPLANE_PHASE("at the warning threshold the warning rung is selected");
    selection = select_derating(ladder, ceiling, ceiling, scope_at(limits, 75000), &limits);
    TCPLANE_CHECK(selection.basis == LimitKind::Warning);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{500});

    TCPLANE_PHASE("at the derate threshold the derate rung is selected");
    selection = select_derating(ladder, ceiling, ceiling, scope_at(limits, 85000), &limits);
    TCPLANE_CHECK(selection.basis == LimitKind::Derate);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{1500});

    TCPLANE_PHASE("at the shutdown threshold the most severe rung is selected");
    selection = select_derating(ladder, ceiling, ceiling, scope_at(limits, 105000), &limits);
    TCPLANE_CHECK(selection.basis == LimitKind::Shutdown);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{3000});

    TCPLANE_PHASE("the derating is clamped by the scope ceiling");
    const ThermalLimitSet narrow = make_limits(kScope, 40000, 75000, 85000, 95000, 105000, 1000);
    selection = select_derating(ladder, ceiling, ceiling, scope_at(narrow, 85000), &narrow);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{1000});
    TCPLANE_CHECK(selection.limited_by_policy);

    TCPLANE_PHASE("a policy ceiling below the scope ceiling still bounds the derating");
    const BasisPoints tight = BasisPoints::from_value_unchecked(200);
    selection = select_derating(ladder, tight, tight, scope_at(limits, 85000), &limits);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{200});
}

TCPLANE_CASE(derating, unusable_evidence_derates_at_least_as_hard) {
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000, 4000);
    const DeratingLadder ladder = default_ladder();
    const BasisPoints ceiling = BasisPoints::from_value_unchecked(3000);

    ScopeHeadroom unknown;
    unknown.scope = kScope;
    unknown.has_limits = true;
    unknown.governing = unknown_headroom(HeadroomReason::EvidenceStale, LimitKind::Derate);

    TCPLANE_PHASE("a stale scope derates at the unknown-evidence floor");
    const DerateSelection selection = select_derating(ladder, ceiling, ceiling, unknown, &limits);
    TCPLANE_CHECK(selection.from_unknown_evidence);
    TCPLANE_CHECK_EQ(selection.derate.value(), std::uint32_t{3000});

    TCPLANE_PHASE("a stricter policy floor still bounds the derating");
    const BasisPoints reduced = BasisPoints::from_value_unchecked(2000);
    const DerateSelection bounded = select_derating(ladder, reduced, reduced, unknown, &limits);
    TCPLANE_CHECK_EQ(bounded.derate.value(), std::uint32_t{2000});
}

TCPLANE_CASE(derating, monotone_in_worsening_evidence) {
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000, 4000);
    const DeratingLadder ladder = default_ladder();
    const BasisPoints ceiling = BasisPoints::from_value_unchecked(3000);

    TCPLANE_PHASE("sweeping the whole temperature range, derating never decreases");
    std::uint32_t previous = 0;
    for (std::int32_t temperature = 20000; temperature <= 110000; temperature += 250) {
        const DerateSelection selection =
            select_derating(ladder, ceiling, ceiling, scope_at(limits, temperature), &limits);
        TCPLANE_CHECK(selection.derate.value() >= previous);
        previous = selection.derate.value();
    }

    TCPLANE_PHASE("becoming unknown never derates less than the worst measured case");
    const std::uint32_t worst_measured = previous;
    ScopeHeadroom unknown;
    unknown.scope = kScope;
    unknown.has_limits = true;
    unknown.governing = unknown_headroom(HeadroomReason::SensorFaulted, LimitKind::Derate);
    const DerateSelection fallback = select_derating(ladder, ceiling, ceiling, unknown, &limits);
    TCPLANE_CHECK(fallback.derate.value() >= worst_measured);
}

TCPLANE_CASE(derating, policy_validation_protects_monotonicity) {
    TCPLANE_PHASE("a decreasing ladder is refused");
    ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    policy.ladder[LimitKind::Derate] = BasisPoints::from_value_unchecked(100);
    policy.ladder[LimitKind::Critical] = BasisPoints::from_value_unchecked(2000);
    TCPLANE_STATUS_ERROR_CODE(validate_policy(policy), ErrorCode::POLICY_NON_MONOTONE_DERATING);

    TCPLANE_PHASE("a ladder above the unknown-evidence floor is refused");
    ThermalPolicy raised = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    raised.unknown_evidence_derate = BasisPoints::from_value_unchecked(1000);
    TCPLANE_STATUS_ERROR_CODE(validate_policy(raised), ErrorCode::POLICY_NON_MONOTONE_DERATING);

    TCPLANE_PHASE("an explicit opt-out is honoured");
    raised.allow_non_monotone_derating = true;
    TCPLANE_STATUS_OK(validate_policy(raised));

    TCPLANE_PHASE("the baseline policy validates");
    const ThermalPolicy baseline = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    TCPLANE_STATUS_OK(validate_policy(baseline));
    TCPLANE_CHECK_EQ(baseline.recovery_min_observations, std::uint32_t{3});
}

TCPLANE_CASE(derating, policy_field_bounds) {
    TCPLANE_PHASE("the unknown-evidence floor cannot exceed the ceiling");
    ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    policy.unknown_evidence_derate = BasisPoints::from_value_unchecked(3001);
    TCPLANE_STATUS_ERROR_CODE(validate_policy(policy), ErrorCode::POLICY_INVALID);

    TCPLANE_PHASE("a policy with no recovery observation requirement is refused");
    ThermalPolicy lax = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    lax.recovery_min_observations = 0;
    TCPLANE_STATUS_ERROR_CODE(validate_policy(lax), ErrorCode::POLICY_INVALID);

    TCPLANE_PHASE("negative hysteresis and margin are refused");
    ThermalPolicy negative = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    negative.hysteresis = TemperatureDelta{-1};
    TCPLANE_STATUS_ERROR_CODE(validate_policy(negative), ErrorCode::POLICY_INVALID);
    ThermalPolicy negative_margin = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    negative_margin.constrained_margin = TemperatureDelta{-1};
    TCPLANE_STATUS_ERROR_CODE(validate_policy(negative_margin), ErrorCode::POLICY_INVALID);

    TCPLANE_PHASE("a policy without identity or generation is refused");
    const ThermalPolicy anonymous = ThermalPolicy::baseline(PolicyId{0}, PolicyGeneration{1});
    TCPLANE_STATUS_ERROR_CODE(validate_policy(anonymous), ErrorCode::INVALID_ARGUMENT);
    const ThermalPolicy generationless = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{0});
    TCPLANE_STATUS_ERROR_CODE(validate_policy(generationless), ErrorCode::INVALID_ARGUMENT);
}
