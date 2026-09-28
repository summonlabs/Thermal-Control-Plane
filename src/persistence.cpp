// Thermal Control Plane — durable store, canonical codec and file backend.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#include "thermal_control_plane/wire.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace thermal_control_plane {
namespace {

using wire::crc32c;
using wire::kMaxCollectionElements;
using wire::Reader;
using wire::Writer;

// ---------------------------------------------------------------------------
// Record framing
// ---------------------------------------------------------------------------

constexpr char kMagic[8] = {'T', 'C', 'P', 'L', 'N', '0', '0', '1'};
constexpr std::size_t kHeaderBytes = 40;

void put_u32(std::byte* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out[static_cast<std::size_t>(shift / 8)] =
            static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
    }
}

void put_u64(std::byte* out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out[static_cast<std::size_t>(shift / 8)] =
            static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
    }
}

std::uint32_t get_u32(const std::byte* in) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(in[static_cast<std::size_t>(shift / 8)]) << static_cast<unsigned>(shift);
    }
    return value;
}

std::uint64_t get_u64(const std::byte* in) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(in[static_cast<std::size_t>(shift / 8)]) << static_cast<unsigned>(shift);
    }
    return value;
}

void write_scope(Writer& w, const ScopeRef& scope) {
    w.u32(scope.facility.value);
    w.u64(scope.zone.value);
    w.u64(scope.domain.value);
}

void read_scope(Reader& r, ScopeRef& scope) {
    scope.facility = FacilityId{r.u32()};
    scope.zone = ZoneRef{r.u64()};
    scope.domain = DomainRef{r.u64()};
}

void write_binding(Writer& w, const AuthorityBinding& binding) {
    w.u64(binding.process.value);
    w.u64(binding.epoch.value);
    w.u64(binding.policy.value);
    w.u64(binding.topology.value);
    w.u64(binding.evidence.value);
    w.u64(binding.revision.value);
}

AuthorityBinding read_binding(Reader& r) {
    AuthorityBinding binding;
    binding.process = ProcessIncarnation{r.u64()};
    binding.epoch = ControlPlaneEpoch{r.u64()};
    binding.policy = PolicyGeneration{r.u64()};
    binding.topology = TopologyGeneration{r.u64()};
    binding.evidence = EvidenceGeneration{r.u64()};
    binding.revision = StateRevision{r.u64()};
    return binding;
}

void write_threshold(Writer& w, const Threshold& threshold) {
    w.boolean(threshold.defined);
    w.i64(threshold.milli_celsius);
}

Threshold read_threshold(Reader& r) {
    Threshold threshold;
    threshold.defined = r.boolean();
    threshold.milli_celsius = static_cast<std::int32_t>(r.i64());
    return threshold;
}

void write_policy(Writer& w, const ThermalPolicy& policy) {
    w.u64(policy.id.value);
    w.u64(policy.generation.value);
    w.i64(policy.freshness.max_age.millis);
    w.i64(policy.recovery_dwell.millis);
    w.u32(policy.recovery_min_observations);
    w.i64(policy.hysteresis.milli_celsius);
    w.i64(policy.constrained_margin.milli_celsius);
    w.u32(policy.max_derate.value());
    w.u32(policy.unknown_evidence_derate.value());
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        w.u32(policy.ladder.at[i].value());
    }
    w.boolean(policy.allow_non_monotone_derating);
    w.boolean(policy.allow_automatic_isolation_exit);
}

ThermalPolicy read_policy(Reader& r) {
    ThermalPolicy policy;
    policy.id = PolicyId{r.u64()};
    policy.generation = PolicyGeneration{r.u64()};
    policy.freshness.max_age = Duration::from_millis_unchecked(r.i64());
    policy.recovery_dwell = Duration::from_millis_unchecked(r.i64());
    policy.recovery_min_observations = r.u32();
    policy.hysteresis = TemperatureDelta{r.i64()};
    policy.constrained_margin = TemperatureDelta{r.i64()};
    policy.max_derate = BasisPoints::from_value_unchecked(r.u32());
    policy.unknown_evidence_derate = BasisPoints::from_value_unchecked(r.u32());
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        policy.ladder.at[i] = BasisPoints::from_value_unchecked(r.u32());
    }
    policy.allow_non_monotone_derating = r.boolean();
    policy.allow_automatic_isolation_exit = r.boolean();
    return policy;
}

void write_envelope(Writer& w, const SafeOperatingEnvelope& envelope) {
    write_scope(w, envelope.scope);
    w.boolean(envelope.facility_wide);
    w.u8(static_cast<std::uint8_t>(envelope.mode));
    w.boolean(envelope.ceiling_defined);
    w.u8(static_cast<std::uint8_t>(envelope.ceiling_basis));
    write_threshold(w, envelope.ceiling);
    w.u32(envelope.max_derate.value());
    w.u64(envelope.policy_generation.value);
    w.u64(envelope.topology_generation.value);
    w.u64(envelope.evidence_generation.value);
    w.u64(envelope.revision.value);
    w.u64(envelope.issued_at.value);
}

SafeOperatingEnvelope read_envelope(Reader& r) {
    SafeOperatingEnvelope envelope;
    read_scope(r, envelope.scope);
    envelope.facility_wide = r.boolean();
    envelope.mode = static_cast<ThermalMode>(r.u8());
    envelope.ceiling_defined = r.boolean();
    envelope.ceiling_basis = static_cast<LimitKind>(r.u8());
    envelope.ceiling = read_threshold(r);
    envelope.max_derate = BasisPoints::from_value_unchecked(r.u32());
    envelope.policy_generation = PolicyGeneration{r.u64()};
    envelope.topology_generation = TopologyGeneration{r.u64()};
    envelope.evidence_generation = EvidenceGeneration{r.u64()};
    envelope.revision = StateRevision{r.u64()};
    envelope.issued_at = Tick{r.u64()};
    return envelope;
}

void write_coordination(Writer& w, const CoordinationRequest& request) {
    w.u64(request.request.value);
    w.u64(request.attempt.value);
    w.u64(request.directive.value);
    w.u8(static_cast<std::uint8_t>(request.kind));
    w.boolean(request.facility_wide);
    write_scope(w, request.scope);
    w.u8(static_cast<std::uint8_t>(request.mode));
    w.u8(static_cast<std::uint8_t>(request.required_mode));
    w.u32(request.derate.value());
    w.u8(static_cast<std::uint8_t>(request.headroom_state));
    w.u8(static_cast<std::uint8_t>(request.headroom_reason));
    w.i64(request.headroom.milli_celsius);
    write_binding(w, request.binding);
    w.u64(request.issued_at.value);
}

CoordinationRequest read_coordination(Reader& r) {
    CoordinationRequest request;
    request.request = RequestId{r.u64()};
    request.attempt = AttemptId{r.u64()};
    request.directive = DirectiveId{r.u64()};
    request.kind = static_cast<CoordinationKind>(r.u8());
    request.facility_wide = r.boolean();
    read_scope(r, request.scope);
    request.mode = static_cast<ThermalMode>(r.u8());
    request.required_mode = static_cast<ThermalMode>(r.u8());
    request.derate = BasisPoints::from_value_unchecked(r.u32());
    request.headroom_state = static_cast<HeadroomState>(r.u8());
    request.headroom_reason = static_cast<HeadroomReason>(r.u8());
    request.headroom = TemperatureDelta{r.i64()};
    request.binding = read_binding(r);
    request.issued_at = Tick{r.u64()};
    return request;
}

void write_outcome(Writer& w, const AuthorityOutcome& outcome) {
    w.u8(static_cast<std::uint8_t>(outcome.verdict));
    w.u32(static_cast<std::uint32_t>(outcome.code));
    w.string(outcome.detail);
    w.boolean(outcome.replayed);
    w.u64(outcome.request.value);
    w.u64(outcome.attempt.value);
    w.u64(outcome.directive.value);
    w.u64(outcome.revision_before.value);
    w.u64(outcome.revision_after.value);
    w.u8(static_cast<std::uint8_t>(outcome.mode));
    w.u8(static_cast<std::uint8_t>(outcome.required_mode));
    w.boolean(outcome.escalated);
    w.boolean(outcome.de_escalated);
    w.u8(static_cast<std::uint8_t>(outcome.cause));
    write_scope(w, outcome.reason_scope);
    w.u8(static_cast<std::uint8_t>(outcome.reason_limit));
    w.boolean(outcome.reason_limit_defined);
    w.u32(outcome.granted_derate.value());
    w.boolean(outcome.derate_from_unknown_evidence);
    w.boolean(outcome.derate_limited_by_policy);
    write_envelope(w, outcome.envelope);
    w.boolean(outcome.coordination.has_value());
    if (outcome.coordination.has_value()) {
        write_coordination(w, *outcome.coordination);
    }
    w.u64(outcome.decided_at.value);
    w.u64(outcome.commit);
    w.boolean(outcome.durable);
}

AuthorityOutcome read_outcome(Reader& r) {
    AuthorityOutcome outcome;
    outcome.verdict = static_cast<AuthorityVerdict>(r.u8());
    outcome.code = static_cast<ErrorCode>(r.u32());
    outcome.detail = r.string();
    outcome.replayed = r.boolean();
    outcome.request = RequestId{r.u64()};
    outcome.attempt = AttemptId{r.u64()};
    outcome.directive = DirectiveId{r.u64()};
    outcome.revision_before = StateRevision{r.u64()};
    outcome.revision_after = StateRevision{r.u64()};
    outcome.mode = static_cast<ThermalMode>(r.u8());
    outcome.required_mode = static_cast<ThermalMode>(r.u8());
    outcome.escalated = r.boolean();
    outcome.de_escalated = r.boolean();
    outcome.cause = static_cast<EscalationCause>(r.u8());
    read_scope(r, outcome.reason_scope);
    outcome.reason_limit = static_cast<LimitKind>(r.u8());
    outcome.reason_limit_defined = r.boolean();
    outcome.granted_derate = BasisPoints::from_value_unchecked(r.u32());
    outcome.derate_from_unknown_evidence = r.boolean();
    outcome.derate_limited_by_policy = r.boolean();
    outcome.envelope = read_envelope(r);
    if (r.boolean()) {
        outcome.coordination = read_coordination(r);
    }
    outcome.decided_at = Tick{r.u64()};
    outcome.commit = r.u64();
    outcome.durable = r.boolean();
    return outcome;
}

void write_directive(Writer& w, const DeratingDirective& directive) {
    w.u64(directive.id.value);
    w.u64(directive.request.value);
    w.u64(directive.attempt.value);
    write_scope(w, directive.scope);
    w.boolean(directive.facility_wide);
    w.u32(directive.requested.value());
    w.u32(directive.granted.value());
    w.u8(static_cast<std::uint8_t>(directive.basis));
    w.boolean(directive.basis_defined);
    w.boolean(directive.from_unknown_evidence);
    w.boolean(directive.limited_by_policy);
    w.i64(directive.observed.milli_celsius());
    write_threshold(w, directive.limit);
    w.u64(directive.policy_generation.value);
    w.u64(directive.topology_generation.value);
    w.u64(directive.evidence_generation.value);
    w.u64(directive.revision.value);
    w.u64(directive.issued_at.value);
    w.u8(static_cast<std::uint8_t>(directive.authority));
    w.u64(directive.observed_sequence.value);
    w.boolean(directive.acknowledged);
    w.u64(directive.acknowledged_at.value);
}

DeratingDirective read_directive(Reader& r) {
    DeratingDirective directive;
    directive.id = DirectiveId{r.u64()};
    directive.request = RequestId{r.u64()};
    directive.attempt = AttemptId{r.u64()};
    read_scope(r, directive.scope);
    directive.facility_wide = r.boolean();
    directive.requested = BasisPoints::from_value_unchecked(r.u32());
    directive.granted = BasisPoints::from_value_unchecked(r.u32());
    directive.basis = static_cast<LimitKind>(r.u8());
    directive.basis_defined = r.boolean();
    directive.from_unknown_evidence = r.boolean();
    directive.limited_by_policy = r.boolean();
    directive.observed =
        Temperature::from_milli_celsius_unchecked(static_cast<std::int32_t>(r.i64()));
    directive.limit = read_threshold(r);
    directive.policy_generation = PolicyGeneration{r.u64()};
    directive.topology_generation = TopologyGeneration{r.u64()};
    directive.evidence_generation = EvidenceGeneration{r.u64()};
    directive.revision = StateRevision{r.u64()};
    directive.issued_at = Tick{r.u64()};
    directive.authority = static_cast<AuthorityClass>(r.u8());
    directive.observed_sequence = ObservationSequence{r.u64()};
    directive.acknowledged = r.boolean();
    directive.acknowledged_at = Tick{r.u64()};
    return directive;
}

void write_interlock(Writer& w, const Interlock& interlock) {
    w.u64(interlock.id.value);
    w.u8(static_cast<std::uint8_t>(interlock.klass));
    write_scope(w, interlock.scope);
    w.boolean(interlock.facility_wide);
    w.u8(static_cast<std::uint8_t>(interlock.state));
    w.u64(interlock.scope_generation.value);
    w.u64(interlock.topology_generation.value);
    w.u64(interlock.author.value);
    w.u64(interlock.observed_at.value);
    w.string(interlock.source);
}

Interlock read_interlock(Reader& r) {
    Interlock interlock;
    interlock.id = InterlockId{r.u64()};
    interlock.klass = static_cast<InterlockClass>(r.u8());
    read_scope(r, interlock.scope);
    interlock.facility_wide = r.boolean();
    interlock.state = static_cast<InterlockState>(r.u8());
    interlock.scope_generation = ScopeGeneration{r.u64()};
    interlock.topology_generation = TopologyGeneration{r.u64()};
    interlock.author = ProcessIncarnation{r.u64()};
    interlock.observed_at = Tick{r.u64()};
    interlock.source = r.string();
    return interlock;
}

void write_audit(Writer& w, const AuditRecord& record) {
    w.u64(record.sequence);
    w.u64(record.at.value);
    w.u64(record.revision.value);
    w.u8(static_cast<std::uint8_t>(record.kind));
    w.u64(record.request.value);
    w.u64(record.attempt.value);
    w.u32(static_cast<std::uint32_t>(record.code));
    w.u64(record.directive.value);
    w.u8(static_cast<std::uint8_t>(record.mode));
    w.u32(record.derate.value());
    w.string(record.detail);
}

AuditRecord read_audit(Reader& r) {
    AuditRecord record;
    record.sequence = r.u64();
    record.at = Tick{r.u64()};
    record.revision = StateRevision{r.u64()};
    record.kind = static_cast<AuditKind>(r.u8());
    record.request = RequestId{r.u64()};
    record.attempt = AttemptId{r.u64()};
    record.code = static_cast<ErrorCode>(r.u32());
    record.directive = DirectiveId{r.u64()};
    record.mode = static_cast<ThermalMode>(r.u8());
    record.derate = BasisPoints::from_value_unchecked(r.u32());
    record.detail = r.string();
    return record;
}

bool known_error_code(std::uint32_t raw) {
    switch (static_cast<ErrorCode>(raw)) {
        case ErrorCode::NONE:
        case ErrorCode::INVALID_ARGUMENT:
        case ErrorCode::INVALID_KEY:
        case ErrorCode::OUT_OF_RANGE:
        case ErrorCode::ARITHMETIC_OVERFLOW:
        case ErrorCode::STALE_EPOCH:
        case ErrorCode::FUTURE_EPOCH:
        case ErrorCode::STALE_POLICY_GENERATION:
        case ErrorCode::FUTURE_POLICY_GENERATION:
        case ErrorCode::STALE_TOPOLOGY_GENERATION:
        case ErrorCode::FUTURE_TOPOLOGY_GENERATION:
        case ErrorCode::STALE_EVIDENCE_GENERATION:
        case ErrorCode::FUTURE_EVIDENCE_GENERATION:
        case ErrorCode::STALE_OBJECT_GENERATION:
        case ErrorCode::FUTURE_OBJECT_GENERATION:
        case ErrorCode::STALE_STATE_REVISION:
        case ErrorCode::FUTURE_STATE_REVISION:
        case ErrorCode::STALE_PROCESS_INCARNATION:
        case ErrorCode::STALE_DIRECTIVE:
        case ErrorCode::STALE_ATTEMPT:
        case ErrorCode::UNKNOWN_SCOPE:
        case ErrorCode::UNKNOWN_SENSOR:
        case ErrorCode::UNKNOWN_POLICY:
        case ErrorCode::UNKNOWN_LIMIT_SET:
        case ErrorCode::UNKNOWN_DIRECTIVE:
        case ErrorCode::UNKNOWN_INTERLOCK:
        case ErrorCode::DUPLICATE_SCOPE:
        case ErrorCode::DUPLICATE_SENSOR:
        case ErrorCode::DUPLICATE_INTERLOCK:
        case ErrorCode::EVIDENCE_UNKNOWN:
        case ErrorCode::EVIDENCE_STALE:
        case ErrorCode::EVIDENCE_FUTURE:
        case ErrorCode::EVIDENCE_INDETERMINATE:
        case ErrorCode::EVIDENCE_CONFLICT:
        case ErrorCode::EVIDENCE_SUPERSEDED:
        case ErrorCode::EVIDENCE_UNSUPPORTED:
        case ErrorCode::EVIDENCE_DUPLICATE:
        case ErrorCode::EVIDENCE_REQUIRED:
        case ErrorCode::EVIDENCE_NOT_REVALIDATED:
        case ErrorCode::LIMIT_UNDEFINED:
        case ErrorCode::LIMIT_ORDER_INVALID:
        case ErrorCode::POLICY_INVALID:
        case ErrorCode::POLICY_NON_MONOTONE_DERATING:
        case ErrorCode::POLICY_LIMIT_EXCEEDED:
        case ErrorCode::LIMIT_SET_CONFLICT:
        case ErrorCode::INTERLOCK_ASSERTED:
        case ErrorCode::AUTHORITY_INSUFFICIENT:
        case ErrorCode::DERATE_OUT_OF_BOUNDS:
        case ErrorCode::DERATE_INSUFFICIENT:
        case ErrorCode::MODE_TRANSITION_INVALID:
        case ErrorCode::RECOVERY_NOT_ELIGIBLE:
        case ErrorCode::ESCALATION_REQUIRED:
        case ErrorCode::COORDINATION_UNAVAILABLE:
        case ErrorCode::AUTHORITY_REFUSED:
        case ErrorCode::VERIFICATION_UNPROVEN:
        case ErrorCode::VERIFICATION_CONTRADICTED:
        case ErrorCode::IDEMPOTENCY_CONFLICT:
        case ErrorCode::RUNTIME_CLOSED:
        case ErrorCode::RESOURCE_EXHAUSTED:
        case ErrorCode::CONCURRENCY_CONFLICT:
        case ErrorCode::STORE_LOCKED:
        case ErrorCode::STORE_IO:
        case ErrorCode::STORE_CORRUPT:
        case ErrorCode::STORE_TRUNCATED:
        case ErrorCode::STORE_TRAILING_BYTES:
        case ErrorCode::STORE_INTEGRITY:
        case ErrorCode::STORE_UNSUPPORTED_VERSION:
        case ErrorCode::STORE_RESERVED_FIELD:
        case ErrorCode::STORE_SLOT_UNAVAILABLE:
        case ErrorCode::STORE_PATH_INVALID:
        case ErrorCode::STORE_READBACK_MISMATCH:
        case ErrorCode::ENCODING_INVALID:
        case ErrorCode::ENCODING_TRUNCATED:
        case ErrorCode::ENCODING_TOO_LARGE:
        case ErrorCode::ENCODING_TRAILING_BYTES:
        case ErrorCode::ENCODING_UNKNOWN_ENUM:
        case ErrorCode::INTERNAL: return true;
    }
    return false;
}

bool known_escalation_cause(std::uint8_t raw) {
    return raw >= static_cast<std::uint8_t>(EscalationCause::SafetyInterlock) &&
           raw <= static_cast<std::uint8_t>(EscalationCause::Nominal);
}

bool known_headroom_state(std::uint8_t raw) {
    return raw >= static_cast<std::uint8_t>(HeadroomState::Known) &&
           raw <= static_cast<std::uint8_t>(HeadroomState::Indeterminate);
}

bool known_headroom_reason(std::uint8_t raw) {
    return raw <= static_cast<std::uint8_t>(HeadroomReason::None);
}

bool known_verdict(std::uint8_t raw) {
    return raw >= static_cast<std::uint8_t>(AuthorityVerdict::Granted) &&
           raw <= static_cast<std::uint8_t>(AuthorityVerdict::Replayed);
}

bool known_coordination_kind(std::uint8_t raw) {
    return raw >= static_cast<std::uint8_t>(CoordinationKind::PlacementConstraint) &&
           raw <= static_cast<std::uint8_t>(CoordinationKind::MaintenanceWindowNotice);
}

bool known_authority_class(std::uint8_t raw) {
    return raw == static_cast<std::uint8_t>(AuthorityClass::Automatic) ||
           raw == static_cast<std::uint8_t>(AuthorityClass::Operator);
}

bool known_interlock_class(std::uint8_t raw) {
    return raw >= static_cast<std::uint8_t>(InterlockClass::Safety) &&
           raw <= static_cast<std::uint8_t>(InterlockClass::Maintenance);
}

bool known_interlock_state(std::uint8_t raw) {
    return raw == static_cast<std::uint8_t>(InterlockState::Asserted) ||
           raw == static_cast<std::uint8_t>(InterlockState::Cleared);
}

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot encoding
// ---------------------------------------------------------------------------

Result<std::vector<std::byte>> encode_snapshot(const ThermalSnapshot& snapshot) {
    Writer w;
    w.u32(snapshot.format_version);
    w.u64(snapshot.commit_sequence.value);
    w.u64(snapshot.epoch.value);
    w.u64(snapshot.owner.value);
    w.u64(snapshot.revision.value);
    w.boolean(snapshot.policy_present);
    if (snapshot.policy_present) {
        write_policy(w, snapshot.policy);
    }
    w.u64(snapshot.topology_generation.value);
    w.u64(snapshot.evidence_generation.value);
    w.u8(static_cast<std::uint8_t>(snapshot.administrative_mode));
    w.boolean(snapshot.administrative_hold);

    w.u8(static_cast<std::uint8_t>(snapshot.mode.mode));
    w.u8(static_cast<std::uint8_t>(snapshot.mode.required));
    w.u8(static_cast<std::uint8_t>(snapshot.mode.cause));
    write_scope(w, snapshot.mode.reason_scope);
    w.u8(static_cast<std::uint8_t>(snapshot.mode.reason_limit));
    w.boolean(snapshot.mode.reason_limit_defined);
    w.u64(snapshot.mode.changed_at.value);

    w.boolean(snapshot.gate.armed);
    w.u8(static_cast<std::uint8_t>(snapshot.gate.armed_for));
    w.u8(static_cast<std::uint8_t>(snapshot.gate.target));
    w.u64(snapshot.gate.favorable_since.value);
    w.u32(snapshot.gate.samples);
    w.u64(snapshot.gate.last_counted.value);
    w.boolean(snapshot.gate.satisfied);
    w.u8(static_cast<std::uint8_t>(snapshot.gate.blocked_reason));

    w.boolean(snapshot.interlock_baseline_declared);
    w.u32(static_cast<std::uint32_t>(snapshot.interlocks.size()));
    for (const Interlock& interlock : snapshot.interlocks) {
        write_interlock(w, interlock);
    }

    w.u64(snapshot.next_directive_id);
    w.u64(snapshot.next_audit_sequence);

    w.u32(static_cast<std::uint32_t>(snapshot.directives.size()));
    for (const DeratingDirective& directive : snapshot.directives) {
        write_directive(w, directive);
    }
    w.u32(static_cast<std::uint32_t>(snapshot.audit.size()));
    for (const AuditRecord& record : snapshot.audit) {
        write_audit(w, record);
    }
    w.u32(static_cast<std::uint32_t>(snapshot.replay.size()));
    for (const ReplayRecord& record : snapshot.replay) {
        w.u8(static_cast<std::uint8_t>(record.key.view().size()));
        w.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(record.key.view().data()),
                                         record.key.view().size()));
        w.bytes(std::span<const std::byte>(record.digest.data(), record.digest.size()));
        write_outcome(w, record.outcome);
        w.u64(record.recorded_at_commit);
    }
    return std::move(w).take();
}

Result<ThermalSnapshot> decode_snapshot(std::span<const std::byte> payload) {
    Reader r(payload);
    const auto check = [&r]() -> std::optional<Error> {
        return r.ok() ? std::nullopt : std::optional<Error>(r.error());
    };

    ThermalSnapshot snapshot;
    snapshot.format_version = r.u32();
    snapshot.commit_sequence = CommitSequence{r.u64()};
    snapshot.epoch = ControlPlaneEpoch{r.u64()};
    snapshot.owner = ProcessIncarnation{r.u64()};
    snapshot.revision = StateRevision{r.u64()};
    snapshot.policy_present = r.boolean();
    if (snapshot.policy_present) {
        snapshot.policy = read_policy(r);
    }
    snapshot.topology_generation = TopologyGeneration{r.u64()};
    snapshot.evidence_generation = EvidenceGeneration{r.u64()};
    const std::uint8_t administrative_mode = r.u8();
    snapshot.administrative_hold = r.boolean();

    const std::uint8_t mode_raw = r.u8();
    const std::uint8_t required_raw = r.u8();
    const std::uint8_t cause_raw = r.u8();
    read_scope(r, snapshot.mode.reason_scope);
    const std::uint8_t limit_raw = r.u8();
    snapshot.mode.reason_limit_defined = r.boolean();
    snapshot.mode.changed_at = Tick{r.u64()};

    snapshot.gate.armed = r.boolean();
    const std::uint8_t armed_for_raw = r.u8();
    const std::uint8_t gate_target_raw = r.u8();
    snapshot.gate.favorable_since = Tick{r.u64()};
    snapshot.gate.samples = r.u32();
    snapshot.gate.last_counted = ObservationSequence{r.u64()};
    snapshot.gate.satisfied = r.boolean();
    const std::uint8_t blocked_raw = r.u8();

    snapshot.interlock_baseline_declared = r.boolean();
    const std::uint32_t interlock_count = r.count(48);
    for (std::uint32_t i = 0; i < interlock_count && r.ok(); ++i) {
        snapshot.interlocks.push_back(read_interlock(r));
    }

    snapshot.next_directive_id = r.u64();
    snapshot.next_audit_sequence = r.u64();

    const std::uint32_t directive_count = r.count(80);
    for (std::uint32_t i = 0; i < directive_count && r.ok(); ++i) {
        snapshot.directives.push_back(read_directive(r));
    }
    const std::uint32_t audit_count = r.count(48);
    for (std::uint32_t i = 0; i < audit_count && r.ok(); ++i) {
        snapshot.audit.push_back(read_audit(r));
    }
    const std::uint32_t replay_count = r.count(70);
    for (std::uint32_t i = 0; i < replay_count && r.ok(); ++i) {
        ReplayRecord record;
        const std::uint8_t key_length = r.u8();
        if (!r.ok()) {
            break;
        }
        if (key_length == 0 || key_length > IdempotencyKey::kMaxLength) {
            return Error{ErrorCode::STORE_CORRUPT, "replay entry holds an invalid idempotency key length"};
        }
        if (r.remaining() < key_length) {
            return Error{ErrorCode::STORE_TRUNCATED, "replay key runs past the end of the payload"};
        }
        std::string key_text;
        key_text.reserve(key_length);
        for (std::uint8_t k = 0; k < key_length; ++k) {
            key_text.push_back(static_cast<char>(r.u8()));
        }
        if (!r.ok()) {
            break;
        }
        const auto parsed = IdempotencyKey::parse(key_text);
        if (!parsed.has_value()) {
            return parsed.error();
        }
        record.key = parsed.value();
        record.digest = r.bytes();
        record.outcome = read_outcome(r);
        record.recorded_at_commit = r.u64();
        snapshot.replay.push_back(std::move(record));
    }
    if (const auto error = check()) {
        return *error;
    }
    const Status end = r.require_end();
    if (!end.ok()) {
        return end.error();
    }

    // Semantic validation. A record that decodes but cannot be trusted is
    // refused, never repaired.
    if (snapshot.format_version != kStoreFormatVersion) {
        return Error{ErrorCode::STORE_UNSUPPORTED_VERSION, "snapshot declares an unsupported format version"};
    }
    if (!is_valid_mode(administrative_mode)) {
        return Error{ErrorCode::STORE_CORRUPT, "administrative mode is not a known mode"};
    }
    if (!is_valid_mode(mode_raw) || !is_valid_mode(required_raw) || !is_valid_mode(armed_for_raw) ||
        !is_valid_mode(gate_target_raw)) {
        return Error{ErrorCode::STORE_CORRUPT, "snapshot holds an unknown thermal mode"};
    }
    if (!known_escalation_cause(cause_raw)) {
        return Error{ErrorCode::STORE_CORRUPT, "snapshot holds an unknown escalation cause"};
    }
    if (!is_valid_limit_kind(limit_raw)) {
        return Error{ErrorCode::STORE_CORRUPT, "snapshot holds an unknown limit kind"};
    }
    if (!known_headroom_reason(blocked_raw)) {
        return Error{ErrorCode::STORE_CORRUPT, "snapshot holds an unknown headroom reason"};
    }
    snapshot.administrative_mode = static_cast<ThermalMode>(administrative_mode);
    snapshot.mode.mode = static_cast<ThermalMode>(mode_raw);
    snapshot.mode.required = static_cast<ThermalMode>(required_raw);
    snapshot.mode.cause = static_cast<EscalationCause>(cause_raw);
    snapshot.mode.reason_limit = static_cast<LimitKind>(limit_raw);
    snapshot.gate.armed_for = static_cast<ThermalMode>(armed_for_raw);
    snapshot.gate.target = static_cast<ThermalMode>(gate_target_raw);
    snapshot.gate.blocked_reason = static_cast<HeadroomReason>(blocked_raw);
    if (!snapshot.mode.reason_limit_defined && !(snapshot.mode.reason_limit == LimitKind::Advisory)) {
        return Error{ErrorCode::STORE_RESERVED_FIELD,
                     "undefined reason limit must encode the default limit kind"};
    }
    if (snapshot.policy_present) {
        const Status valid = validate_policy(snapshot.policy);
        if (!valid.ok()) {
            return Error{ErrorCode::STORE_CORRUPT, "recovered policy is not valid"};
        }
        if (snapshot.policy.generation.is_zero() || snapshot.policy.id.is_zero()) {
            return Error{ErrorCode::STORE_CORRUPT, "recovered policy has no identity or generation"};
        }
    }
    if (snapshot.next_directive_id == 0 || snapshot.next_audit_sequence == 0) {
        return Error{ErrorCode::STORE_RESERVED_FIELD, "identifier counters must start at one"};
    }
    if (snapshot.interlocks.size() > kMaxCollectionElements ||
        snapshot.directives.size() > kMaxCollectionElements || snapshot.audit.size() > kMaxCollectionElements ||
        snapshot.replay.size() > kMaxCollectionElements) {
        return Error{ErrorCode::STORE_CORRUPT, "snapshot holds more entries than the format allows"};
    }

    std::set<std::uint64_t> interlock_ids;
    for (const Interlock& interlock : snapshot.interlocks) {
        if (interlock.id.is_zero()) {
            return Error{ErrorCode::STORE_CORRUPT, "interlock record has no identity"};
        }
        if (!known_interlock_class(static_cast<std::uint8_t>(interlock.klass)) ||
            !known_interlock_state(static_cast<std::uint8_t>(interlock.state))) {
            return Error{ErrorCode::STORE_CORRUPT, "interlock record holds an unknown enum value"};
        }
        if (!interlock_ids.insert(interlock.id.value).second) {
            return Error{ErrorCode::STORE_CORRUPT, "duplicate interlock in snapshot"};
        }
    }
    std::set<std::uint64_t> directive_ids;
    for (const DeratingDirective& directive : snapshot.directives) {
        if (directive.id.is_zero()) {
            return Error{ErrorCode::STORE_CORRUPT, "directive record has no identity"};
        }
        if (!is_valid_limit_kind(static_cast<std::uint8_t>(directive.basis)) ||
            !known_authority_class(static_cast<std::uint8_t>(directive.authority))) {
            return Error{ErrorCode::STORE_CORRUPT, "directive record holds an unknown enum value"};
        }
        if (directive.limit.defined == false && directive.limit.milli_celsius != 0) {
            return Error{ErrorCode::STORE_RESERVED_FIELD, "undefined directive limit carries a temperature"};
        }
        if (!directive_ids.insert(directive.id.value).second) {
            return Error{ErrorCode::STORE_CORRUPT, "duplicate directive in snapshot"};
        }
    }
    std::uint64_t previous_audit = 0;
    for (const AuditRecord& record : snapshot.audit) {
        if (!is_valid_audit_kind(static_cast<std::uint8_t>(record.kind)) ||
            !is_valid_mode(static_cast<std::uint8_t>(record.mode)) || !known_error_code(static_cast<std::uint32_t>(record.code))) {
            return Error{ErrorCode::STORE_CORRUPT, "audit record holds an unknown enum value"};
        }
        if (record.detail.size() > kMaxAuditDetailBytes) {
            return Error{ErrorCode::STORE_CORRUPT, "audit detail exceeds the retained length"};
        }
        if (previous_audit != 0 && record.sequence <= previous_audit) {
            return Error{ErrorCode::STORE_CORRUPT, "audit sequence is not strictly increasing"};
        }
        previous_audit = record.sequence;
    }
    std::set<std::string> replay_keys;
    for (const ReplayRecord& record : snapshot.replay) {
        if (!record.key.valid()) {
            return Error{ErrorCode::STORE_CORRUPT, "replay entry has an empty idempotency key"};
        }
        if (record.digest.empty() || record.digest.size() > kMaxRequestDigestBytes) {
            return Error{ErrorCode::STORE_CORRUPT, "replay entry digest is out of range"};
        }
        const HeadroomState headroom_state =
            record.outcome.coordination.has_value() ? record.outcome.coordination->headroom_state
                                                    : HeadroomState::Known;
        const HeadroomReason headroom_reason =
            record.outcome.coordination.has_value() ? record.outcome.coordination->headroom_reason
                                                    : HeadroomReason::None;
        const CoordinationKind coordination_kind =
            record.outcome.coordination.has_value() ? record.outcome.coordination->kind
                                                    : CoordinationKind::ThermalStateNotice;
        if (!known_verdict(static_cast<std::uint8_t>(record.outcome.verdict)) ||
            !known_error_code(static_cast<std::uint32_t>(record.outcome.code)) ||
            !is_valid_mode(static_cast<std::uint8_t>(record.outcome.mode)) ||
            !is_valid_mode(static_cast<std::uint8_t>(record.outcome.required_mode)) ||
            !known_escalation_cause(static_cast<std::uint8_t>(record.outcome.cause)) ||
            !is_valid_limit_kind(static_cast<std::uint8_t>(record.outcome.reason_limit)) ||
            !is_valid_mode(static_cast<std::uint8_t>(record.outcome.envelope.mode)) ||
            !is_valid_limit_kind(static_cast<std::uint8_t>(record.outcome.envelope.ceiling_basis)) ||
            !known_headroom_state(static_cast<std::uint8_t>(headroom_state)) ||
            !known_headroom_reason(static_cast<std::uint8_t>(headroom_reason)) ||
            !known_coordination_kind(static_cast<std::uint8_t>(coordination_kind))) {
            return Error{ErrorCode::STORE_CORRUPT, "replay entry holds an unknown enum value"};
        }
        if (record.outcome.detail.size() > kMaxAuditDetailBytes) {
            return Error{ErrorCode::STORE_CORRUPT, "replay entry detail exceeds the retained length"};
        }
        if (!replay_keys.insert(record.key.str()).second) {
            return Error{ErrorCode::STORE_CORRUPT, "duplicate idempotency key in the replay ledger"};
        }
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// FileStorageBackend
// ---------------------------------------------------------------------------

FileStorageBackend::FileStorageBackend(std::string path) : requested_path_(std::move(path)) {}

FileStorageBackend::~FileStorageBackend() { close(); }

std::filesystem::path FileStorageBackend::slot_path(std::size_t slot) const {
    return slot_directory_ / (slot_stem_ + "." + std::to_string(slot) + ".tcpslot");
}

Status FileStorageBackend::open() {
    if (open_) {
        return Status::failure(ErrorCode::STORE_IO, "storage backend is already open");
    }
    if (requested_path_.empty()) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path must not be empty");
    }
    if (requested_path_.size() > 512) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path exceeds the supported length");
    }
    if (requested_path_.find('\0') != std::string::npos) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path contains a NUL byte");
    }
    // On Windows only the drive letter may carry a colon; anything after it is
    // an alternate data stream and would resolve to a different file than the
    // one the lock is taken on.
    const auto colon = requested_path_.find(':', 2);
    if (colon != std::string::npos) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID,
                               "store path contains an alternate data stream separator");
    }

    std::error_code ec;
    const std::filesystem::path requested(requested_path_);
    if (!requested.is_absolute()) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path must be absolute");
    }
    const std::filesystem::path parent = requested.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Status::failure(ErrorCode::STORE_IO, "cannot create the store directory: " + ec.message());
        }
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(requested, ec);
    if (ec) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path cannot be canonicalised: " + ec.message());
    }
    if (std::filesystem::is_symlink(canonical, ec)) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path resolves to a symbolic link");
    }
    ec.clear();
    if (std::filesystem::exists(canonical, ec) && std::filesystem::is_directory(canonical, ec)) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path is a directory");
    }

    canonical_root_ = canonical.string();
    slot_directory_ = canonical.parent_path();
    slot_stem_ = canonical.filename().string();
    if (slot_stem_.empty()) {
        return Status::failure(ErrorCode::STORE_PATH_INVALID, "store path has no file name component");
    }

    // Remove any staged slot file left behind by an interrupted write. Staged
    // bytes are never authoritative.
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        std::error_code remove_ec;
        std::filesystem::remove(std::filesystem::path(slot_path(slot).string() + ".tmp"), remove_ec);
    }

#if defined(_WIN32)
    const std::wstring lock_path = (canonical.wstring() + L".lock");
    HANDLE handle = ::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD last = ::GetLastError();
        if (last == ERROR_SHARING_VIOLATION || last == ERROR_LOCK_VIOLATION) {
            return Status::failure(ErrorCode::STORE_LOCKED, "another process holds the store lock");
        }
        return Status::failure(ErrorCode::STORE_IO,
                               "cannot acquire the store lock (windows error " + std::to_string(last) + ")");
    }
    lock_handle_ = handle;
#else
    const std::string lock_path = canonical_root_ + ".lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Status::failure(ErrorCode::STORE_IO, "cannot open the store lock file");
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return Status::failure(ErrorCode::STORE_LOCKED, "another process holds the store lock");
    }
    lock_handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif
    open_ = true;
    return Status::success();
}

void FileStorageBackend::close() noexcept {
    if (!open_) {
        return;
    }
#if defined(_WIN32)
    if (lock_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(lock_handle_));
    }
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(lock_handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    lock_handle_ = nullptr;
    open_ = false;
}

Result<std::vector<std::byte>> FileStorageBackend::read_slot(std::size_t slot) const {
    if (!open_) {
        return Error{ErrorCode::STORE_IO, "storage backend is not open"};
    }
    if (slot >= kSlots) {
        return Error{ErrorCode::STORE_SLOT_UNAVAILABLE, "slot index is out of range"};
    }
    const std::filesystem::path path = slot_path(slot);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::vector<std::byte>{};
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Error{ErrorCode::STORE_IO, "cannot size the store slot: " + ec.message()};
    }
    if (size > kMaxSlotBytes) {
        return Error{ErrorCode::STORE_CORRUPT, "store slot is larger than the format allows"};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Error{ErrorCode::STORE_IO, "cannot open the store slot for reading"};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size != 0) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream || static_cast<std::size_t>(stream.gcount()) != static_cast<std::size_t>(size)) {
            return Error{ErrorCode::STORE_IO, "store slot read did not return the expected byte count"};
        }
    }
    return bytes;
}

Status FileStorageBackend::write_slot(std::size_t slot, std::span<const std::byte> bytes) {
    if (!open_) {
        return Status::failure(ErrorCode::STORE_IO, "storage backend is not open");
    }
    if (slot >= kSlots) {
        return Status::failure(ErrorCode::STORE_SLOT_UNAVAILABLE, "slot index is out of range");
    }
    if (bytes.size() > kMaxSlotBytes) {
        return Status::failure(ErrorCode::ENCODING_TOO_LARGE, "slot payload exceeds the configured bound");
    }
    const std::filesystem::path path = slot_path(slot);
    const std::filesystem::path staged = std::filesystem::path(path.string() + ".tmp");

    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (::_wfopen_s(&file, staged.wstring().c_str(), L"wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(staged.string().c_str(), "wb");
#endif
    if (file == nullptr) {
        return Status::failure(ErrorCode::STORE_IO, "cannot open the staged slot for writing");
    }
    if (!bytes.empty()) {
        const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
        if (written != bytes.size()) {
            std::fclose(file);
            std::error_code remove_ec;
            std::filesystem::remove(staged, remove_ec);
            return Status::failure(ErrorCode::STORE_IO, "short write to the staged slot");
        }
    }
    if (std::fflush(file) != 0) {
        std::fclose(file);
        return Status::failure(ErrorCode::STORE_IO, "cannot flush the staged slot");
    }
#if defined(_WIN32)
    if (::_commit(::_fileno(file)) != 0) {
        std::fclose(file);
        return Status::failure(ErrorCode::STORE_IO, "cannot commit the staged slot to stable storage");
    }
#else
    if (::fsync(::fileno(file)) != 0) {
        std::fclose(file);
        return Status::failure(ErrorCode::STORE_IO, "cannot commit the staged slot to stable storage");
    }
#endif
    if (std::fclose(file) != 0) {
        return Status::failure(ErrorCode::STORE_IO, "cannot close the staged slot");
    }

#if defined(_WIN32)
    if (::MoveFileExW(staged.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD last = ::GetLastError();
        return Status::failure(ErrorCode::STORE_IO,
                               "cannot publish the store slot (windows error " + std::to_string(last) + ")");
    }
#else
    if (std::rename(staged.string().c_str(), path.string().c_str()) != 0) {
        return Status::failure(ErrorCode::STORE_IO, "cannot publish the store slot");
    }
#endif
    return Status::success();
}

Status FileStorageBackend::erase_slot(std::size_t slot) {
    if (!open_) {
        return Status::failure(ErrorCode::STORE_IO, "storage backend is not open");
    }
    if (slot >= kSlots) {
        return Status::failure(ErrorCode::STORE_SLOT_UNAVAILABLE, "slot index is out of range");
    }
    std::error_code ec;
    std::filesystem::remove(slot_path(slot), ec);
    if (ec) {
        return Status::failure(ErrorCode::STORE_IO, "cannot erase the store slot: " + ec.message());
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------

DurableStore::DurableStore(std::unique_ptr<StorageBackend> backend) : backend_(std::move(backend)) {}

Status DurableStore::open() {
    if (backend_ == nullptr) {
        return Status::failure(ErrorCode::INTERNAL, "durable store has no backend");
    }
    if (open_) {
        return Status::failure(ErrorCode::STORE_IO, "durable store is already open");
    }
    const Status acquired = backend_->open();
    if (!acquired.ok()) {
        return acquired;
    }
    open_ = true;
    resolved_ = false;
    have_generation_ = false;
    last_commit_ = CommitSequence{};
    active_slot_ = 0;
    return Status::success();
}

void DurableStore::close() noexcept {
    if (!open_) {
        return;
    }
    if (backend_ != nullptr) {
        backend_->close();
    }
    open_ = false;
    have_generation_ = false;
}

Result<StoreRecord> DurableStore::decode_record(std::span<const std::byte> raw) const {
    if (raw.size() < kHeaderBytes) {
        return Error{ErrorCode::STORE_TRUNCATED, "store record is shorter than its header"};
    }
    if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) != 0) {
        return Error{ErrorCode::STORE_CORRUPT, "store record magic does not match"};
    }
    const std::uint32_t format_version = get_u32(raw.data() + 8);
    const std::uint32_t flags = get_u32(raw.data() + 12);
    const std::uint64_t commit_sequence = get_u64(raw.data() + 16);
    const std::uint64_t payload_length = get_u64(raw.data() + 24);
    const std::uint32_t payload_crc = get_u32(raw.data() + 32);
    const std::uint32_t header_crc = get_u32(raw.data() + 36);
    if (flags != 0) {
        return Error{ErrorCode::STORE_RESERVED_FIELD, "store record header reserved field is not zero"};
    }
    if (crc32c(raw.first(36)) != header_crc) {
        return Error{ErrorCode::STORE_INTEGRITY, "store record header failed its integrity check"};
    }
    if (format_version != kStoreFormatVersion) {
        return Error{ErrorCode::STORE_UNSUPPORTED_VERSION, "store record declares an unsupported format version"};
    }
    if (commit_sequence == 0) {
        return Error{ErrorCode::STORE_CORRUPT, "store record has a zero commit sequence"};
    }
    if (payload_length == 0 || payload_length > kMaxStorePayloadBytes) {
        return Error{ErrorCode::STORE_CORRUPT, "store record declares an implausible payload length"};
    }
    const std::size_t total = kHeaderBytes + static_cast<std::size_t>(payload_length);
    if (raw.size() < total) {
        return Error{ErrorCode::STORE_TRUNCATED, "store record payload is shorter than declared"};
    }
    if (raw.size() > total) {
        return Error{ErrorCode::STORE_TRAILING_BYTES, "store record has trailing bytes"};
    }
    const std::span<const std::byte> payload = raw.subspan(kHeaderBytes, static_cast<std::size_t>(payload_length));
    if (crc32c(payload) != payload_crc) {
        return Error{ErrorCode::STORE_INTEGRITY, "store record payload failed its integrity check"};
    }
    StoreRecord record;
    record.commit_sequence = CommitSequence{commit_sequence};
    record.format_version = format_version;
    record.payload.assign(payload.begin(), payload.end());
    return record;
}

Result<LoadOutcome> DurableStore::load() {
    if (!open_) {
        return Error{ErrorCode::STORE_IO, "durable store is not open"};
    }
    struct Candidate {
        std::size_t slot = 0;
        StoreRecord record;
        ThermalSnapshot snapshot;
    };
    std::vector<Candidate> candidates;
    std::optional<Error> first_failure;
    bool saw_content = false;

    for (std::size_t slot = 0; slot < backend_->slot_count(); ++slot) {
        const auto raw = backend_->read_slot(slot);
        if (!raw.has_value()) {
            return raw.error();
        }
        if (raw.value().empty()) {
            continue;
        }
        saw_content = true;
        const auto record = decode_record(raw.value());
        if (!record.has_value()) {
            if (!first_failure.has_value()) {
                first_failure = record.error();
            }
            continue;
        }
        const auto snapshot =
            decode_snapshot(std::span<const std::byte>(record.value().payload.data(), record.value().payload.size()));
        if (!snapshot.has_value()) {
            if (!first_failure.has_value()) {
                first_failure = snapshot.error();
            }
            continue;
        }
        if (snapshot.value().commit_sequence != record.value().commit_sequence) {
            if (!first_failure.has_value()) {
                first_failure = Error{ErrorCode::STORE_CORRUPT,
                                      "store record and snapshot disagree about the commit sequence"};
            }
            continue;
        }
        Candidate candidate;
        candidate.slot = slot;
        candidate.record = record.value();
        candidate.snapshot = snapshot.value();
        candidates.push_back(std::move(candidate));
    }

    if (candidates.empty()) {
        if (saw_content && first_failure.has_value()) {
            return *first_failure;
        }
        LoadOutcome outcome;
        outcome.present = false;
        have_generation_ = false;
        last_commit_ = CommitSequence{};
        active_slot_ = 0;
        resolved_ = true;
        return outcome;
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.record.commit_sequence != b.record.commit_sequence) {
            return a.record.commit_sequence > b.record.commit_sequence;
        }
        return a.slot < b.slot;
    });
    if (candidates.size() > 1 && candidates[0].record.commit_sequence == candidates[1].record.commit_sequence &&
        candidates[0].record.payload != candidates[1].record.payload) {
        return Error{ErrorCode::STORE_CORRUPT,
                     "two store slots claim the same commit sequence with different content"};
    }

    LoadOutcome outcome;
    outcome.present = true;
    outcome.slot = candidates.front().slot;
    outcome.snapshot = std::move(candidates.front().snapshot);
    have_generation_ = true;
    last_commit_ = candidates.front().record.commit_sequence;
    active_slot_ = outcome.slot;
    resolved_ = true;
    return outcome;
}

Result<CommitSequence> DurableStore::commit(ThermalSnapshot snapshot) {
    if (!open_) {
        return Error{ErrorCode::STORE_IO, "durable store is not open"};
    }
    if (!resolved_) {
        return Error{ErrorCode::STORE_IO,
                     "the durable store must be resolved with load() before it can be committed"};
    }
    const CommitSequence next =
        have_generation_ ? last_commit_.next() : CommitSequence{1};
    if (next == last_commit_ && have_generation_) {
        return Error{ErrorCode::STORE_CORRUPT, "commit sequence is exhausted"};
    }
    snapshot.format_version = kStoreFormatVersion;
    snapshot.commit_sequence = next;

    const auto payload = encode_snapshot(snapshot);
    if (!payload.has_value()) {
        return payload.error();
    }
    if (payload.value().size() > kMaxStorePayloadBytes) {
        return Error{ErrorCode::ENCODING_TOO_LARGE, "snapshot payload exceeds the configured bound"};
    }

    std::vector<std::byte> record(kHeaderBytes + payload.value().size());
    std::memcpy(record.data(), kMagic, sizeof(kMagic));
    put_u32(record.data() + 8, kStoreFormatVersion);
    put_u32(record.data() + 12, 0);
    put_u64(record.data() + 16, next.value);
    put_u64(record.data() + 24, static_cast<std::uint64_t>(payload.value().size()));
    put_u32(record.data() + 32, crc32c(std::span<const std::byte>(payload.value().data(), payload.value().size())));
    put_u32(record.data() + 36, crc32c(std::span<const std::byte>(record.data(), 36)));
    std::memcpy(record.data() + kHeaderBytes, payload.value().data(), payload.value().size());

    const std::size_t slot_count = backend_->slot_count();
    const std::size_t target = have_generation_ ? (active_slot_ + 1) % slot_count : 0;
    const Status written = backend_->write_slot(target, std::span<const std::byte>(record.data(), record.size()));
    if (!written.ok()) {
        return written.error();
    }

    const auto read_back = backend_->read_slot(target);
    if (!read_back.has_value()) {
        return read_back.error();
    }
    if (read_back.value() != record) {
        const Status erased = backend_->erase_slot(target);
        static_cast<void>(erased);
        return Error{ErrorCode::STORE_READBACK_MISMATCH,
                     "store slot did not read back byte-identical to what was written"};
    }
    const auto verified = decode_record(std::span<const std::byte>(read_back.value().data(), read_back.value().size()));
    if (!verified.has_value()) {
        const Status erased = backend_->erase_slot(target);
        static_cast<void>(erased);
        return verified.error();
    }
    if (verified.value().commit_sequence != next) {
        const Status erased = backend_->erase_slot(target);
        static_cast<void>(erased);
        return Error{ErrorCode::STORE_READBACK_MISMATCH, "store slot read back a different commit sequence"};
    }

    have_generation_ = true;
    last_commit_ = next;
    active_slot_ = target;
    return next;
}

}  // namespace thermal_control_plane
