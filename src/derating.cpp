// Thermal Control Plane — derating selection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/derating.hpp"

namespace thermal_control_plane {
namespace {

BasisPoints clamp(BasisPoints value, BasisPoints ceiling) {
    return value <= ceiling ? value : ceiling;
}

}  // namespace

DerateSelection select_derating(const DeratingLadder& ladder,
                                BasisPoints max_derate,
                                BasisPoints unknown_evidence_derate,
                                const ScopeHeadroom& headroom,
                                const ThermalLimitSet* limits) {
    DerateSelection out;
    out.scope = headroom.scope;

    const BasisPoints scope_ceiling = limits == nullptr ? max_derate : clamp(max_derate, limits->max_derate);

    if (!headroom.has_hottest || !headroom.governing.known()) {
        out.from_unknown_evidence = true;
        out.reason = headroom.governing.reason;
        out.derate = clamp(clamp(unknown_evidence_derate, max_derate), scope_ceiling);
        out.limited_by_policy = out.derate != clamp(unknown_evidence_derate, max_derate) || max_derate != BasisPoints::full();
        out.basis = LimitKind::Advisory;
        out.basis_defined = false;
        return out;
    }

    // Walk the ladder from the most severe kind downwards. The first threshold
    // the observation has reached selects the derating.
    for (std::size_t i = kLimitKindCount; i-- > 0;) {
        const auto kind = static_cast<LimitKind>(i + 1);
        const Threshold& threshold = headroom.by_kind[i].limit;
        if (!threshold.defined) {
            continue;
        }
        if (headroom.hottest.milli_celsius() >= threshold.milli_celsius) {
            out.basis = kind;
            out.basis_defined = true;
            break;
        }
    }

    const BasisPoints selected = out.basis_defined ? ladder[out.basis] : BasisPoints::none();
    const BasisPoints policy_bounded = clamp(selected, max_derate);
    out.derate = clamp(policy_bounded, scope_ceiling);
    out.limited_by_policy = !(out.derate == selected);
    out.reason = HeadroomReason::None;
    return out;
}

}  // namespace thermal_control_plane
