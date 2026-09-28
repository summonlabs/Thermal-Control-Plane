// Thermal Control Plane — bounded derating authority.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_DERATING_HPP
#define THERMAL_CONTROL_PLANE_DERATING_HPP

#include <array>
#include <cstdint>
#include <string_view>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/headroom.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/limits.hpp"
#include "thermal_control_plane/mode.hpp"
#include "thermal_control_plane/quantity.hpp"

namespace thermal_control_plane {

/// The derating ladder: how much capability is withdrawn at each limit kind.
struct DeratingLadder {
    std::array<BasisPoints, kLimitKindCount> at{};

    [[nodiscard]] BasisPoints operator[](LimitKind kind) const noexcept { return at[limit_index(kind)]; }
    [[nodiscard]] BasisPoints& operator[](LimitKind kind) noexcept { return at[limit_index(kind)]; }
};

/// The result of selecting a derating for one scope.
struct DerateSelection {
    BasisPoints derate{};
    LimitKind basis = LimitKind::Advisory;
    bool basis_defined = false;
    bool from_unknown_evidence = false;
    bool limited_by_policy = false;
    ScopeRef scope{};
    HeadroomReason reason = HeadroomReason::None;
};

/// Select the derating required by the current evidence for one scope.
///
/// The selection is monotone with respect to worsening evidence: a higher
/// temperature, or evidence that has become unusable, never selects a smaller
/// derating. Policy validation enforces the precondition that makes this true,
/// namely that unusable evidence derates at least as hard as any defined
/// threshold can.
[[nodiscard]] DerateSelection select_derating(const DeratingLadder& ladder,
                                              BasisPoints max_derate,
                                              BasisPoints unknown_evidence_derate,
                                              const ScopeHeadroom& headroom,
                                              const ThermalLimitSet* limits);

/// A bounded derating directive issued by this runtime.
///
/// A directive is a recorded authority decision, not proof that anything
/// physical changed. Its effect is only ever established by fresh evidence
/// observed after it was issued.
struct DeratingDirective {
    DirectiveId id{};
    RequestId request{};
    AttemptId attempt{};
    ScopeRef scope{};
    bool facility_wide = false;
    BasisPoints requested{};
    BasisPoints granted{};
    LimitKind basis = LimitKind::Advisory;
    bool basis_defined = false;
    bool from_unknown_evidence = false;
    bool limited_by_policy = false;
    Temperature observed{};
    Threshold limit{};
    PolicyGeneration policy_generation{};
    TopologyGeneration topology_generation{};
    EvidenceGeneration evidence_generation{};
    StateRevision revision{};
    Tick issued_at{};
    AuthorityClass authority = AuthorityClass::Automatic;
    /// Highest evidence sequence observed in this scope when the directive was
    /// issued. Verification requires a strictly newer observation.
    ObservationSequence observed_sequence{};
    /// Set when a downstream owner acknowledges the directive. An
    /// acknowledgement is not evidence that anything physical changed.
    bool acknowledged = false;
    Tick acknowledged_at{};

    [[nodiscard]] bool bounded(BasisPoints ceiling) const noexcept { return granted <= ceiling; }
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_DERATING_HPP
