// Thermal Control Plane — canonical thermal operating modes and escalation precedence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_MODE_HPP
#define THERMAL_CONTROL_PLANE_MODE_HPP

#include <cstdint>
#include <string_view>

#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/limits.hpp"

namespace thermal_control_plane {

/// The canonical facility thermal operating modes.
///
/// These are the only operating states this runtime will speak about. They are
/// ordered by severity, and that order is used for exactly two things:
/// deciding which of several simultaneously applicable modes wins, and
/// reporting severity. It is never used to compute a transition: leaving a
/// mode is governed by the explicit transition relation below.
enum class ThermalMode : std::uint8_t {
    Normal = 1,
    Maintenance = 2,
    Recovery = 3,
    Constrained = 4,
    Degraded = 5,
    Emergency = 6,
    Isolated = 7,
};

[[nodiscard]] constexpr std::string_view to_string(ThermalMode mode) noexcept {
    switch (mode) {
        case ThermalMode::Normal: return "NORMAL";
        case ThermalMode::Maintenance: return "MAINTENANCE";
        case ThermalMode::Recovery: return "RECOVERY";
        case ThermalMode::Constrained: return "CONSTRAINED";
        case ThermalMode::Degraded: return "DEGRADED";
        case ThermalMode::Emergency: return "EMERGENCY";
        case ThermalMode::Isolated: return "ISOLATED";
    }
    return "UNRECOGNISED_THERMAL_MODE";
}

[[nodiscard]] constexpr bool is_valid_mode(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(ThermalMode::Normal) &&
           raw <= static_cast<std::uint8_t>(ThermalMode::Isolated);
}

/// Severity rank of a mode; higher is more severe.
///
/// Maintenance and Recovery share a rank: both constrain authority without
/// asserting a thermal limit violation. Maintenance wins the tie because it is
/// the more restrictive of the two.
[[nodiscard]] constexpr std::uint8_t mode_severity(ThermalMode mode) noexcept {
    switch (mode) {
        case ThermalMode::Normal: return 0;
        case ThermalMode::Recovery: return 1;
        case ThermalMode::Maintenance: return 1;
        case ThermalMode::Constrained: return 2;
        case ThermalMode::Degraded: return 3;
        case ThermalMode::Emergency: return 4;
        case ThermalMode::Isolated: return 5;
    }
    return 6;
}

[[nodiscard]] constexpr bool is_more_severe(ThermalMode a, ThermalMode b) noexcept {
    const std::uint8_t sa = mode_severity(a);
    const std::uint8_t sb = mode_severity(b);
    if (sa != sb) {
        return sa > sb;
    }
    return a == ThermalMode::Maintenance && b != ThermalMode::Maintenance;
}

/// The mode that wins when several modes are simultaneously applicable.
[[nodiscard]] constexpr ThermalMode escalate(ThermalMode a, ThermalMode b) noexcept {
    if (a == b) {
        return a;
    }
    return is_more_severe(a, b) ? a : b;
}

/// Why the facility is in the mode it is in.
///
/// The numeric values are the precedence order: a lower value outranks a
/// higher one. Explicit interlocks and physical limit violations therefore
/// always outrank advisory optimisation, and the primary reason reported for
/// any decision is a pure function of the evidence.
enum class EscalationCause : std::uint8_t {
    SafetyInterlock = 1,
    IsolationInterlock = 2,
    ShutdownLimitViolation = 3,
    CriticalLimitViolation = 4,
    EvidenceIndeterminate = 5,
    AwaitingRevalidation = 6,
    EvidenceStale = 7,
    EvidenceUnknown = 8,
    GenerationMismatch = 9,
    DerateLimitViolation = 10,
    WarningLimitViolation = 11,
    RecoveryPending = 12,
    MaintenanceWindow = 13,
    AdvisoryMargin = 14,
    OperatorDirective = 15,
    Nominal = 16,
};

[[nodiscard]] constexpr std::uint8_t cause_precedence(EscalationCause cause) noexcept {
    return static_cast<std::uint8_t>(cause);
}

[[nodiscard]] constexpr std::string_view to_string(EscalationCause cause) noexcept {
    switch (cause) {
        case EscalationCause::SafetyInterlock: return "SAFETY_INTERLOCK";
        case EscalationCause::IsolationInterlock: return "ISOLATION_INTERLOCK";
        case EscalationCause::ShutdownLimitViolation: return "SHUTDOWN_LIMIT_VIOLATION";
        case EscalationCause::CriticalLimitViolation: return "CRITICAL_LIMIT_VIOLATION";
        case EscalationCause::EvidenceIndeterminate: return "EVIDENCE_INDETERMINATE";
        case EscalationCause::AwaitingRevalidation: return "AWAITING_REVALIDATION";
        case EscalationCause::EvidenceStale: return "EVIDENCE_STALE";
        case EscalationCause::EvidenceUnknown: return "EVIDENCE_UNKNOWN";
        case EscalationCause::GenerationMismatch: return "GENERATION_MISMATCH";
        case EscalationCause::DerateLimitViolation: return "DERATE_LIMIT_VIOLATION";
        case EscalationCause::WarningLimitViolation: return "WARNING_LIMIT_VIOLATION";
        case EscalationCause::RecoveryPending: return "RECOVERY_PENDING";
        case EscalationCause::MaintenanceWindow: return "MAINTENANCE_WINDOW";
        case EscalationCause::AdvisoryMargin: return "ADVISORY_MARGIN";
        case EscalationCause::OperatorDirective: return "OPERATOR_DIRECTIVE";
        case EscalationCause::Nominal: return "NOMINAL";
    }
    return "UNRECOGNISED_ESCALATION_CAUSE";
}

/// A fully attributed reason for a mode or authority decision.
struct ModeReason {
    EscalationCause cause = EscalationCause::Nominal;
    ScopeRef scope{};
    LimitKind limit = LimitKind::Advisory;
    bool limit_defined = false;
    SensorRef sensor{};

    friend constexpr bool operator==(const ModeReason& a, const ModeReason& b) noexcept {
        return a.cause == b.cause && a.scope == b.scope && a.limit == b.limit &&
               a.limit_defined == b.limit_defined && a.sensor == b.sensor;
    }
};

/// True when a is the primary reason over b.
///
/// The comparison is total: cause precedence, then canonical scope order, then
/// limit severity, then sensor identity. The same evidence therefore always
/// produces the same primary reason.
[[nodiscard]] bool reason_outranks(const ModeReason& a, const ModeReason& b) noexcept;

/// True when a and b attribute the primary reason to the same facts.
[[nodiscard]] bool same_reason(const ModeReason& a, const ModeReason& b) noexcept;

/// The mode that follows a completed recovery gate.
///
/// De-escalation moves exactly one rung and never skips a rung. The successor
/// of Constrained is Recovery, and the successor of Recovery is Normal, so
/// returning to Normal always requires two completed gates.
[[nodiscard]] constexpr ThermalMode step_down(ThermalMode mode) noexcept {
    switch (mode) {
        case ThermalMode::Isolated: return ThermalMode::Emergency;
        case ThermalMode::Emergency: return ThermalMode::Degraded;
        case ThermalMode::Degraded: return ThermalMode::Constrained;
        case ThermalMode::Constrained: return ThermalMode::Recovery;
        case ThermalMode::Recovery: return ThermalMode::Normal;
        case ThermalMode::Maintenance: return ThermalMode::Normal;
        case ThermalMode::Normal: return ThermalMode::Normal;
    }
    return ThermalMode::Normal;
}

/// True when the mode is left only by an explicit operator action.
[[nodiscard]] constexpr bool requires_operator_to_leave(ThermalMode mode) noexcept {
    return mode == ThermalMode::Isolated || mode == ThermalMode::Maintenance;
}

/// Authority provenance of a request.
enum class AuthorityClass : std::uint8_t {
    /// Emitted by an automated component.
    Automatic = 1,
    /// Emitted by, or explicitly attributed to, an operator.
    Operator = 2,
};

[[nodiscard]] constexpr std::string_view to_string(AuthorityClass klass) noexcept {
    switch (klass) {
        case AuthorityClass::Automatic: return "AUTOMATIC";
        case AuthorityClass::Operator: return "OPERATOR";
    }
    return "UNRECOGNISED_AUTHORITY_CLASS";
}

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_MODE_HPP
