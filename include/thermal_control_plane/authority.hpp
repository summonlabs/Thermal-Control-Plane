// Thermal Control Plane — authority attempts, outcomes and audit.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_AUTHORITY_HPP
#define THERMAL_CONTROL_PLANE_AUTHORITY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "thermal_control_plane/derating.hpp"
#include "thermal_control_plane/evidence.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/interlock.hpp"
#include "thermal_control_plane/mode.hpp"
#include "thermal_control_plane/policy.hpp"
#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// What the caller is asking the thermal authority layer to do.
enum class AttemptKind : std::uint8_t {
    /// Ask for the current assessment without changing anything.
    AssessFacility = 1,
    /// Ask for a bounded derating directive.
    RequestDerate = 2,
    /// Ask for a thermal mode transition.
    RequestMode = 3,
    /// Ask for a coordination request to another facility control plane.
    RequestCoordination = 4,
    /// Acknowledge a previously issued directive.
    AcknowledgeDirective = 5,
};

[[nodiscard]] constexpr std::string_view to_string(AttemptKind kind) noexcept {
    switch (kind) {
        case AttemptKind::AssessFacility: return "ASSESS_FACILITY";
        case AttemptKind::RequestDerate: return "REQUEST_DERATE";
        case AttemptKind::RequestMode: return "REQUEST_MODE";
        case AttemptKind::RequestCoordination: return "REQUEST_COORDINATION";
        case AttemptKind::AcknowledgeDirective: return "ACKNOWLEDGE_DIRECTIVE";
    }
    return "UNRECOGNISED_ATTEMPT_KIND";
}

/// The verdict of an authority attempt.
enum class AuthorityVerdict : std::uint8_t {
    Granted = 1,
    Refused = 2,
    /// The attempt reused an idempotency key; the original outcome is replayed
    /// verbatim and nothing was actuated again.
    Replayed = 3,
};

[[nodiscard]] constexpr std::string_view to_string(AuthorityVerdict verdict) noexcept {
    switch (verdict) {
        case AuthorityVerdict::Granted: return "GRANTED";
        case AuthorityVerdict::Refused: return "REFUSED";
        case AuthorityVerdict::Replayed: return "REPLAYED";
    }
    return "UNRECOGNISED_AUTHORITY_VERDICT";
}

/// A request to another facility control plane.
///
/// This runtime never places workloads and never sheds load. It publishes
/// typed notices that the owning planes consume on their own authority.
enum class CoordinationKind : std::uint8_t {
    PlacementConstraint = 1,
    PlacementRelease = 2,
    PowerDerateNotice = 3,
    PowerHeadroomNotice = 4,
    ThermalStateNotice = 5,
    MaintenanceWindowNotice = 6,
};

[[nodiscard]] constexpr std::string_view to_string(CoordinationKind kind) noexcept {
    switch (kind) {
        case CoordinationKind::PlacementConstraint: return "PLACEMENT_CONSTRAINT";
        case CoordinationKind::PlacementRelease: return "PLACEMENT_RELEASE";
        case CoordinationKind::PowerDerateNotice: return "POWER_DERATE_NOTICE";
        case CoordinationKind::PowerHeadroomNotice: return "POWER_HEADROOM_NOTICE";
        case CoordinationKind::ThermalStateNotice: return "THERMAL_STATE_NOTICE";
        case CoordinationKind::MaintenanceWindowNotice: return "MAINTENANCE_WINDOW_NOTICE";
    }
    return "UNRECOGNISED_COORDINATION_KIND";
}

/// The exact state an attempt was planned against.
///
/// Every field must match the runtime's current state or the attempt is
/// refused. Nothing is inferred and nothing is upgraded: a stale binding is a
/// refusal, never a refresh.
struct AuthorityBinding {
    ProcessIncarnation process{};
    ControlPlaneEpoch epoch{};
    PolicyGeneration policy{};
    TopologyGeneration topology{};
    EvidenceGeneration evidence{};
    StateRevision revision{};
};

/// An authority-bearing request.
struct AuthorityAttempt {
    RequestId request{};
    AttemptId attempt{};
    IdempotencyKey key{};
    AttemptKind kind = AttemptKind::RequestDerate;
    AuthorityClass authority = AuthorityClass::Automatic;
    AuthorityBinding binding{};
    bool facility_wide = true;
    ScopeRef scope{};
    BasisPoints requested_derate{};
    ThermalMode requested_mode = ThermalMode::Normal;
    CoordinationKind coordination_kind = CoordinationKind::ThermalStateNotice;
    DirectiveId directive{};
};

/// An outbound coordination notice. Advisory, typed, generation-stamped.
struct CoordinationRequest {
    RequestId request{};
    AttemptId attempt{};
    DirectiveId directive{};
    CoordinationKind kind = CoordinationKind::ThermalStateNotice;
    bool facility_wide = true;
    ScopeRef scope{};
    ThermalMode mode = ThermalMode::Normal;
    ThermalMode required_mode = ThermalMode::Normal;
    BasisPoints derate{};
    HeadroomState headroom_state = HeadroomState::Unknown;
    HeadroomReason headroom_reason = HeadroomReason::NoEvidence;
    TemperatureDelta headroom{};
    AuthorityBinding binding{};
    Tick issued_at{};
};

/// The outcome of an authority attempt.
struct AuthorityOutcome {
    AuthorityVerdict verdict = AuthorityVerdict::Refused;
    ErrorCode code = ErrorCode::AUTHORITY_REFUSED;
    std::string detail;
    bool replayed = false;

    RequestId request{};
    AttemptId attempt{};
    DirectiveId directive{};
    StateRevision revision_before{};
    StateRevision revision_after{};

    ThermalMode mode = ThermalMode::Normal;
    ThermalMode required_mode = ThermalMode::Normal;
    bool escalated = false;
    bool de_escalated = false;
    EscalationCause cause = EscalationCause::Nominal;
    ScopeRef reason_scope{};
    LimitKind reason_limit = LimitKind::Advisory;
    bool reason_limit_defined = false;

    BasisPoints granted_derate{};
    bool derate_from_unknown_evidence = false;
    bool derate_limited_by_policy = false;

    SafeOperatingEnvelope envelope{};
    std::optional<CoordinationRequest> coordination{};

    Tick decided_at{};
    std::uint64_t commit = 0;
    bool durable = false;

    [[nodiscard]] bool granted() const noexcept { return verdict != AuthorityVerdict::Refused; }
};

/// Whether an issued directive was actually borne out by later evidence.
///
/// A directive is never proof that a physical condition changed. Only fresh
/// evidence observed after the directive can establish that, so the default
/// answer for an unobserved directive is Unproven.
enum class VerificationState : std::uint8_t {
    Proven = 1,
    Unproven = 2,
    Contradicted = 3,
    Superseded = 4,
};

[[nodiscard]] constexpr std::string_view to_string(VerificationState state) noexcept {
    switch (state) {
        case VerificationState::Proven: return "PROVEN";
        case VerificationState::Unproven: return "UNPROVEN";
        case VerificationState::Contradicted: return "CONTRADICTED";
        case VerificationState::Superseded: return "SUPERSEDED";
    }
    return "UNRECOGNISED_VERIFICATION_STATE";
}

struct VerificationResult {
    VerificationState state = VerificationState::Unproven;
    ErrorCode code = ErrorCode::VERIFICATION_UNPROVEN;
    std::string detail;
    DirectiveId directive{};
    bool observed = false;
    Temperature observed_temperature{};
    SensorRef sensor{};
    ObservationSequence sequence{};
    Tick observed_at{};
};

/// Kinds of durable audit record.
enum class AuditKind : std::uint8_t {
    Created = 1,
    Recovered = 2,
    PolicyInstalled = 3,
    LimitsPublished = 4,
    LimitsRemoved = 5,
    ScopeDeclared = 6,
    EvidenceGenerationAdvanced = 7,
    EvidenceIngested = 8,
    InterlockBaselineDeclared = 9,
    InterlockChanged = 10,
    AdministrativeModeSet = 11,
    ModeTransition = 12,
    AuthorityGranted = 13,
    AuthorityRefused = 14,
    AuthorityReplayed = 15,
    CoordinationEmitted = 16,
    VerificationRecorded = 17,
    Closed = 18,
    FaultInjected = 19,
};

[[nodiscard]] constexpr std::string_view to_string(AuditKind kind) noexcept {
    switch (kind) {
        case AuditKind::Created: return "CREATED";
        case AuditKind::Recovered: return "RECOVERED";
        case AuditKind::PolicyInstalled: return "POLICY_INSTALLED";
        case AuditKind::LimitsPublished: return "LIMITS_PUBLISHED";
        case AuditKind::LimitsRemoved: return "LIMITS_REMOVED";
        case AuditKind::ScopeDeclared: return "SCOPE_DECLARED";
        case AuditKind::EvidenceGenerationAdvanced: return "EVIDENCE_GENERATION_ADVANCED";
        case AuditKind::EvidenceIngested: return "EVIDENCE_INGESTED";
        case AuditKind::InterlockBaselineDeclared: return "INTERLOCK_BASELINE_DECLARED";
        case AuditKind::InterlockChanged: return "INTERLOCK_CHANGED";
        case AuditKind::AdministrativeModeSet: return "ADMINISTRATIVE_MODE_SET";
        case AuditKind::ModeTransition: return "MODE_TRANSITION";
        case AuditKind::AuthorityGranted: return "AUTHORITY_GRANTED";
        case AuditKind::AuthorityRefused: return "AUTHORITY_REFUSED";
        case AuditKind::AuthorityReplayed: return "AUTHORITY_REPLAYED";
        case AuditKind::CoordinationEmitted: return "COORDINATION_EMITTED";
        case AuditKind::VerificationRecorded: return "VERIFICATION_RECORDED";
        case AuditKind::Closed: return "CLOSED";
        case AuditKind::FaultInjected: return "FAULT_INJECTED";
    }
    return "UNRECOGNISED_AUDIT_KIND";
}

[[nodiscard]] constexpr bool is_valid_audit_kind(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(AuditKind::Created) &&
           raw <= static_cast<std::uint8_t>(AuditKind::FaultInjected);
}

/// Maximum number of characters retained in an audit detail string.
inline constexpr std::size_t kMaxAuditDetailBytes = 96;

struct AuditRecord {
    std::uint64_t sequence = 0;
    Tick at{};
    StateRevision revision{};
    AuditKind kind = AuditKind::Created;
    RequestId request{};
    AttemptId attempt{};
    ErrorCode code = ErrorCode::NONE;
    DirectiveId directive{};
    ThermalMode mode = ThermalMode::Normal;
    BasisPoints derate{};
    std::string detail;
};

/// A recorded idempotency key and the outcome it produced.
///
/// The digest is the canonical encoding of the attempt's semantic fields, not
/// a hash: a reused key with different semantics is detected by comparing
/// bytes, so a hash collision can never turn a conflicting request into a
/// silent replay.
struct ReplayRecord {
    IdempotencyKey key{};
    std::vector<std::byte> digest;
    AuthorityOutcome outcome{};
    std::uint64_t recorded_at_commit = 0;
};

/// Maximum number of entries retained in the replay ledger.
inline constexpr std::size_t kMaxReplayEntries = 256;
/// Maximum number of directives retained in the durable tail.
inline constexpr std::size_t kMaxDirectiveTail = 256;
/// Maximum size of a canonical request digest.
inline constexpr std::size_t kMaxRequestDigestBytes = 128;

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_AUTHORITY_HPP
