// Thermal Control Plane — recovery gate implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/recovery.hpp"

namespace thermal_control_plane {
namespace {

struct CeilingKinds {
    LimitKind primary = LimitKind::Warning;
    LimitKind fallback = LimitKind::Warning;
    bool has_fallback = false;
};

CeilingKinds ceiling_for(ThermalMode target) {
    switch (target) {
        case ThermalMode::Constrained: return CeilingKinds{LimitKind::Derate, LimitKind::Warning, true};
        case ThermalMode::Recovery:
        case ThermalMode::Normal: return CeilingKinds{LimitKind::Warning, LimitKind::Warning, false};
        case ThermalMode::Degraded:
        case ThermalMode::Emergency: return CeilingKinds{LimitKind::Critical, LimitKind::Shutdown, true};
        case ThermalMode::Maintenance: return CeilingKinds{LimitKind::Warning, LimitKind::Warning, false};
        case ThermalMode::Isolated: return CeilingKinds{LimitKind::Critical, LimitKind::Shutdown, true};
    }
    return CeilingKinds{LimitKind::Warning, LimitKind::Warning, false};
}

HeadroomReason worse(HeadroomReason a, HeadroomReason b) {
    return reason_precedence(a) <= reason_precedence(b) ? a : b;
}

}  // namespace

RecoveryGateState advance_recovery_gate(const RecoveryGateState& previous,
                                        const RecoveryGateInput& input,
                                        const std::vector<ScopeHeadroom>& scopes) {
    RecoveryGateState out;
    out.target = step_down(input.current_mode);
    if (input.current_mode == ThermalMode::Normal) {
        out.armed = false;
        out.armed_for = ThermalMode::Normal;
        return out;
    }

    const CeilingKinds ceiling = ceiling_for(out.target);
    bool favorable = true;
    HeadroomReason blocked = HeadroomReason::None;
    std::size_t limited_scopes = 0;
    ObservationSequence highest_sample{};

    for (const ScopeHeadroom& scope : scopes) {
        if (!scope.has_limits) {
            continue;
        }
        ++limited_scopes;
        if (!scope.governing.known()) {
            favorable = false;
            blocked = worse(blocked, scope.governing.reason);
            continue;
        }
        const Headroom& primary = scope.by_kind[limit_index(ceiling.primary)];
        const Headroom& fallback = scope.by_kind[limit_index(ceiling.fallback)];
        const Headroom* chosen = nullptr;
        if (primary.limit.defined && primary.known()) {
            chosen = &primary;
        } else if (ceiling.has_fallback && fallback.limit.defined && fallback.known()) {
            chosen = &fallback;
        }
        if (chosen == nullptr) {
            favorable = false;
            blocked = worse(blocked, HeadroomReason::LimitUndefined);
            continue;
        }
        if (chosen->delta.milli_celsius < input.hysteresis.milli_celsius) {
            favorable = false;
        }
        if (scope.hottest_sequence > highest_sample) {
            highest_sample = scope.hottest_sequence;
        }
    }

    if (limited_scopes == 0) {
        favorable = false;
        blocked = worse(blocked, HeadroomReason::LimitUndefined);
    }

    if (!favorable) {
        out.armed = true;
        out.armed_for = input.current_mode;
        out.favorable_since = Tick{};
        out.samples = 0;
        out.last_counted = ObservationSequence{};
        out.satisfied = false;
        out.blocked_reason = blocked;
        return out;
    }

    if (!previous.armed || previous.armed_for != input.current_mode || previous.favorable_since.is_zero()) {
        out.armed = true;
        out.armed_for = input.current_mode;
        out.favorable_since = input.now;
        out.samples = 0;
        out.last_counted = ObservationSequence{};
    } else {
        out = previous;
        out.target = step_down(input.current_mode);
    }

    if (highest_sample > out.last_counted) {
        ++out.samples;
        out.last_counted = highest_sample;
    }

    std::int64_t dwell_millis = 0;
    if (out.favorable_since <= input.now) {
        dwell_millis = static_cast<std::int64_t>(input.now.value - out.favorable_since.value);
    }
    const bool dwell_met = dwell_millis >= input.dwell.millis;
    const bool samples_met = out.samples >= input.min_observations;
    out.satisfied = dwell_met && samples_met;
    out.blocked_reason = out.satisfied ? HeadroomReason::None : HeadroomReason::None;
    return out;
}

}  // namespace thermal_control_plane
