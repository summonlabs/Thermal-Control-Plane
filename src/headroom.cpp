// Thermal Control Plane — headroom arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/headroom.hpp"

namespace thermal_control_plane {

Headroom compute_headroom(Temperature observed, const Threshold& limit, LimitKind basis) {
    Headroom out;
    out.basis = basis;
    out.limit = limit;
    out.observed = observed;
    if (!limit.defined) {
        out.state = HeadroomState::Unknown;
        out.reason = HeadroomReason::LimitUndefined;
        return out;
    }
    const auto delta = difference(Temperature::from_milli_celsius_unchecked(limit.milli_celsius), observed);
    if (!delta.has_value()) {
        out.state = HeadroomState::Unknown;
        out.reason = HeadroomReason::LimitUndefined;
        return out;
    }
    out.state = HeadroomState::Known;
    out.reason = HeadroomReason::None;
    out.delta = delta.value();
    // "Over the limit" means at or beyond it: a reading exactly at a declared
    // limit is not headroom, it is the boundary of the envelope.
    out.over_limit = out.delta.milli_celsius <= 0;
    return out;
}

Headroom unknown_headroom(HeadroomReason reason, LimitKind basis) {
    Headroom out;
    out.basis = basis;
    out.reason = reason;
    out.state = reason == HeadroomReason::EvidenceIndeterminate ? HeadroomState::Indeterminate
                                                                : HeadroomState::Unknown;
    return out;
}

ScopeHeadroom evaluate_headroom(const ThermalLimitSet* limits, const ScopeEvidenceView& view) {
    ScopeHeadroom out;
    out.scope = view.scope;
    out.sensors = view.sensors;
    out.fresh_measured = view.fresh_measured;
    out.has_hottest = view.has_hottest;
    out.hottest = view.hottest;
    out.hottest_sensor = view.hottest_sensor;
    out.hottest_sequence = view.hottest_sequence;
    out.hottest_at = view.hottest_at;

    if (limits == nullptr) {
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            out.by_kind[i] = unknown_headroom(HeadroomReason::LimitUndefined, static_cast<LimitKind>(i + 1));
        }
        out.governing = unknown_headroom(HeadroomReason::LimitUndefined, LimitKind::Advisory);
        return out;
    }

    out.has_limits = true;
    out.limits_generation = limits->generation;

    if (view.scope_generation_mixed ||
        (view.has_scope_generation && view.scope_generation != limits->scope_generation)) {
        const HeadroomReason reason = HeadroomReason::GenerationMismatch;
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            out.by_kind[i] = unknown_headroom(reason, static_cast<LimitKind>(i + 1));
        }
        out.governing = unknown_headroom(reason, LimitKind::Advisory);
        return out;
    }

    if (!view.has_hottest) {
        const HeadroomReason reason = view.empty ? HeadroomReason::NoEvidence
                                                 : to_headroom_reason(view.primary_reason);
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            out.by_kind[i] = unknown_headroom(reason, static_cast<LimitKind>(i + 1));
        }
        out.governing = unknown_headroom(reason, LimitKind::Advisory);
        return out;
    }

    bool have_governing = false;
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        const auto kind = static_cast<LimitKind>(i + 1);
        const Threshold& threshold = (*limits)[kind];
        Headroom headroom = compute_headroom(view.hottest, threshold, kind);
        out.by_kind[i] = headroom;
        if (!headroom.known()) {
            continue;
        }
        if (!have_governing || headroom.delta.milli_celsius < out.governing.delta.milli_celsius ||
            (headroom.delta.milli_celsius == out.governing.delta.milli_celsius &&
             limit_severity(kind) > limit_severity(out.governing.basis))) {
            out.governing = headroom;
            have_governing = true;
        }
    }
    if (!have_governing) {
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            out.by_kind[i] = unknown_headroom(HeadroomReason::LimitUndefined, static_cast<LimitKind>(i + 1));
        }
        out.governing = unknown_headroom(HeadroomReason::LimitUndefined, LimitKind::Advisory);
    }
    return out;
}

FacilityHeadroom aggregate_headroom(const std::vector<ScopeHeadroom>& scopes) {
    FacilityHeadroom out;
    out.scopes_total = scopes.size();
    bool first_unknown = true;
    for (const ScopeHeadroom& scope : scopes) {
        if (!scope.has_limits) {
            continue;
        }
        ++out.scopes_with_limits;
        if (scope.governing.known()) {
            ++out.scopes_known;
            if (!out.has_worst || scope.governing.delta.milli_celsius < out.worst_delta.milli_celsius ||
                (scope.governing.delta.milli_celsius == out.worst_delta.milli_celsius &&
                 scope.scope < out.worst_scope)) {
                out.has_worst = true;
                out.worst_delta = scope.governing.delta;
                out.worst_scope = scope.scope;
                out.worst_basis = scope.governing.basis;
                out.worst_observed = scope.governing.observed;
                out.worst_sensor = scope.hottest_sensor;
            }
            continue;
        }
        if (scope.governing.state == HeadroomState::Indeterminate) {
            ++out.scopes_indeterminate;
        } else {
            ++out.scopes_unknown;
        }
        if (first_unknown || reason_precedence(scope.governing.reason) < reason_precedence(out.reason) ||
            (reason_precedence(scope.governing.reason) == reason_precedence(out.reason) &&
             scope.scope < out.worst_scope)) {
            out.reason = scope.governing.reason;
            out.worst_scope = scope.scope;
            first_unknown = false;
        }
    }

    if (out.scopes_with_limits == 0) {
        out.state = HeadroomState::Unknown;
        out.reason = HeadroomReason::LimitUndefined;
        out.has_worst = false;
        return out;
    }
    if (out.scopes_unknown != 0 || out.scopes_indeterminate != 0) {
        out.state = out.scopes_indeterminate != 0 ? HeadroomState::Indeterminate : HeadroomState::Unknown;
        // The worst known delta is deliberately not reported: a facility whose
        // headroom is unknown has no reportable number.
        out.has_worst = false;
        out.worst_delta = TemperatureDelta{};
        return out;
    }
    out.state = HeadroomState::Known;
    out.reason = HeadroomReason::None;
    return out;
}

}  // namespace thermal_control_plane
