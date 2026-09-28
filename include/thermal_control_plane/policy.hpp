// Thermal Control Plane — deterministic thermal policy evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_POLICY_HPP
#define THERMAL_CONTROL_PLANE_POLICY_HPP

#include <cstdint>
#include <vector>

#include "thermal_control_plane/derating.hpp"
#include "thermal_control_plane/evidence.hpp"
#include "thermal_control_plane/headroom.hpp"
#include "thermal_control_plane/interlock.hpp"
#include "thermal_control_plane/limits.hpp"
#include "thermal_control_plane/mode.hpp"
#include "thermal_control_plane/recovery.hpp"
#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// The thermal policy: every parameter that decides escalation, derating and
/// recovery.
///
/// The policy is a plain value. Installing a policy is a generation change,
/// and every authority decision is bound to the generation it was evaluated
/// under.
struct ThermalPolicy {
    PolicyId id{};
    PolicyGeneration generation{};

    /// How old an observation may be and still count as current evidence.
    FreshnessWindow freshness{};
    /// Continuous time below the hysteresis threshold before a step-down.
    Duration recovery_dwell = Duration::from_millis_unchecked(30000);
    /// Distinct evidence advances required before a step-down.
    std::uint32_t recovery_min_observations = 3;
    /// How far below an escalation threshold the temperature must fall before
    /// the facility may step down.
    TemperatureDelta hysteresis = TemperatureDelta{5000};
    /// How far below the warning threshold the advisory band begins.
    TemperatureDelta constrained_margin = TemperatureDelta{2000};
    /// The largest derating this policy will ever authorise.
    BasisPoints max_derate = BasisPoints::from_value_unchecked(3000);
    /// The derating applied when a scope with published limits has no usable
    /// evidence. Validated to be at least every ladder step, so unusable
    /// evidence can never derate less than a measured violation would.
    BasisPoints unknown_evidence_derate = BasisPoints::from_value_unchecked(3000);
    DeratingLadder ladder{};
    /// Escape hatch for a policy that deliberately breaks derating
    /// monotonicity. Off by default, and required to be on for validation to
    /// accept a non-monotone ladder.
    bool allow_non_monotone_derating = false;
    /// Whether the runtime may leave Isolated mode without an operator action.
    bool allow_automatic_isolation_exit = false;

    /// A validated baseline policy with conservative defaults.
    [[nodiscard]] static ThermalPolicy baseline(PolicyId id, PolicyGeneration generation);
};

/// Structural and semantic validation of a policy.
[[nodiscard]] Status validate_policy(const ThermalPolicy& policy);

/// The externally owned inputs to a policy evaluation.
struct ThermalEnvironment {
    const EvidenceStore* evidence = nullptr;
    const LimitRegistry* limits = nullptr;
    const InterlockRegistry* interlocks = nullptr;
    TopologyGeneration topology_generation{};
};

/// The situation a policy evaluation is performed in.
struct PolicyInput {
    Tick now{};
    /// The mode that is currently authoritative.
    ThermalMode current_mode = ThermalMode::Normal;
    /// The mode the operator has asked for, or Normal for none.
    ThermalMode administrative_mode = ThermalMode::Normal;
    /// Operator holds the current administrative mode until explicitly cleared.
    bool administrative_hold = false;
    EvidenceGeneration evidence_generation{};
    TopologyGeneration topology_generation{};
    StateRevision revision{};
    RecoveryGateState gate{};
};

/// The safe operating envelope this runtime is willing to authorise.
struct SafeOperatingEnvelope {
    ScopeRef scope{};
    bool facility_wide = false;
    ThermalMode mode = ThermalMode::Normal;
    bool ceiling_defined = false;
    LimitKind ceiling_basis = LimitKind::Warning;
    Threshold ceiling{};
    BasisPoints max_derate{};
    PolicyGeneration policy_generation{};
    TopologyGeneration topology_generation{};
    EvidenceGeneration evidence_generation{};
    StateRevision revision{};
    Tick issued_at{};
};

/// The result of a policy evaluation.
struct PolicyOutcome {
    ThermalMode mode = ThermalMode::Normal;
    ThermalMode previous_mode = ThermalMode::Normal;
    ThermalMode required_mode = ThermalMode::Normal;
    bool escalated = false;
    bool de_escalated = false;
    bool transition_deferred = false;
    ModeReason reason{};

    FacilityHeadroom facility{};
    std::vector<ScopeHeadroom> scopes{};

    BasisPoints required_derate{};
    ScopeRef derate_scope{};
    bool derate_from_unknown_evidence = false;
    bool derate_limited_by_policy = false;

    RecoveryGateState gate{};
    bool recovery_eligible = false;

    SafeOperatingEnvelope envelope{};

    PolicyGeneration policy_generation{};
    EvidenceGeneration evidence_generation{};
    TopologyGeneration topology_generation{};
    StateRevision revision{};
    Tick evaluated_at{};
};

/// Evaluate the thermal policy.
///
/// Pure: no clock, no locks, no I/O. The clock tick, the evidence, the limits
/// and the interlocks all arrive as inputs, so the same inputs always produce
/// the same outcome.
[[nodiscard]] PolicyOutcome evaluate_policy(const ThermalPolicy& policy,
                                            const ThermalEnvironment& environment,
                                            const PolicyInput& input);

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_POLICY_HPP
