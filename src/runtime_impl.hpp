// Thermal Control Plane — shared runtime internals.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_SRC_RUNTIME_IMPL_HPP
#define THERMAL_CONTROL_PLANE_SRC_RUNTIME_IMPL_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/wire.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace thermal_control_plane {

/// Every bound the runtime enforces on caller-controlled resources.
constexpr std::size_t kMaxBoundEntries = 65536;

namespace detail {

/// The runtime's private state.
///
/// Deliberately not a nested type of ThermalRuntime: the runtime's own
/// translation units need the complete definition while the public header must
/// keep it opaque.
struct RuntimeImpl {
    RuntimeOptions options{};
    std::unique_ptr<Clock> clock;
    std::unique_ptr<DurableStore> store;
    mutable std::shared_mutex mutex;

    ProcessIncarnation incarnation{};
    ControlPlaneEpoch epoch{};
    StateRevision revision{StateRevision{1}};
    CommitSequence commit{};

    TopologyGeneration topology_generation{};
    EvidenceGeneration evidence_generation{};

    bool policy_present = false;
    ThermalPolicy policy{};

    EvidenceStore evidence{};
    LimitRegistry limits{};
    InterlockRegistry interlocks{};
    bool interlock_baseline_declared = false;

    ThermalMode administrative_mode = ThermalMode::Normal;
    bool administrative_hold = false;
    ModeRecord mode_record{};
    RecoveryGateState gate{};

    std::vector<DeratingDirective> directives;
    std::vector<AuditRecord> audit;
    std::vector<ReplayRecord> replay;
    std::vector<CoordinationRequest> outbound;
    std::map<DirectiveId, VerificationResult> verifications;

    std::uint64_t next_directive_id = 1;
    std::uint64_t next_audit_sequence = 1;

    PolicyOutcome last_outcome{};
    bool have_last_outcome = false;

    bool closed = false;
    bool store_faulted = false;
    std::string fault_detail;
};

}  // namespace detail

namespace runtime_detail {

using Impl = detail::RuntimeImpl;

inline std::string sanitize(std::string_view text, std::size_t limit) {
    std::string out;
    out.reserve(text.size() < limit ? text.size() : limit);
    for (const char raw : text) {
        if (out.size() >= limit) {
            break;
        }
        const auto byte = static_cast<unsigned char>(raw);
        out.push_back((byte >= 0x20U && byte <= 0x7EU) ? raw : '?');
    }
    return out;
}

inline ProcessIncarnation make_incarnation() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    const std::uint64_t sequence = counter.fetch_add(1);
#if defined(_WIN32)
    const std::uint64_t pid = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
#endif
    const std::uint64_t value = (pid << 32U) ^ (sequence & 0xFFFFFFFFULL);
    return ProcessIncarnation{value == 0 ? 1U : value};
}

inline ErrorCode code_for_headroom(HeadroomReason reason) {
    switch (reason) {
        case HeadroomReason::EvidenceStale: return ErrorCode::EVIDENCE_STALE;
        case HeadroomReason::EvidenceFuture: return ErrorCode::EVIDENCE_FUTURE;
        case HeadroomReason::EvidenceIndeterminate: return ErrorCode::EVIDENCE_INDETERMINATE;
        case HeadroomReason::AwaitingRevalidation: return ErrorCode::EVIDENCE_NOT_REVALIDATED;
        case HeadroomReason::Interlocked: return ErrorCode::INTERLOCK_ASSERTED;
        case HeadroomReason::GenerationMismatch: return ErrorCode::STALE_OBJECT_GENERATION;
        case HeadroomReason::SensorAbsent: return ErrorCode::EVIDENCE_UNKNOWN;
        case HeadroomReason::SensorFaulted: return ErrorCode::EVIDENCE_UNKNOWN;
        case HeadroomReason::SourceUnavailable: return ErrorCode::EVIDENCE_UNKNOWN;
        case HeadroomReason::NotSupported: return ErrorCode::EVIDENCE_UNSUPPORTED;
        case HeadroomReason::NoEvidence: return ErrorCode::EVIDENCE_UNKNOWN;
        case HeadroomReason::LimitUndefined: return ErrorCode::LIMIT_UNDEFINED;
        case HeadroomReason::None: return ErrorCode::NONE;
    }
    return ErrorCode::EVIDENCE_UNKNOWN;
}

inline std::string describe_headroom(const FacilityHeadroom& headroom) {
    std::string out("facility headroom ");
    out.append(to_string(headroom.state));
    out.push_back(' ');
    out.append(to_string(headroom.reason));
    return out;
}

struct AuditInput {
    AuditKind kind = AuditKind::Created;
    ErrorCode code = ErrorCode::NONE;
    RequestId request{};
    AttemptId attempt{};
    DirectiveId directive{};
    ThermalMode mode = ThermalMode::Normal;
    BasisPoints derate{};
    std::string detail;
};

inline void push_audit(Impl& state, Tick at, const AuditInput& input) {
    AuditRecord record;
    record.sequence = state.next_audit_sequence;
    state.next_audit_sequence += 1;
    record.at = at;
    record.revision = state.revision;
    record.kind = input.kind;
    record.request = input.request;
    record.attempt = input.attempt;
    record.code = input.code;
    record.directive = input.directive;
    record.mode = input.mode;
    record.derate = input.derate;
    record.detail = sanitize(input.detail, kMaxAuditDetailBytes);
    state.audit.push_back(std::move(record));
    while (state.audit.size() > state.options.bounds.max_audit_records) {
        state.audit.erase(state.audit.begin());
    }
}

inline void push_directive(Impl& state, DeratingDirective directive) {
    state.directives.push_back(std::move(directive));
    while (state.directives.size() > state.options.bounds.max_directives) {
        state.directives.erase(state.directives.begin());
    }
}

inline void push_replay(Impl& state, ReplayRecord record) {
    for (ReplayRecord& existing : state.replay) {
        if (existing.key == record.key) {
            existing = std::move(record);
            return;
        }
    }
    state.replay.push_back(std::move(record));
    while (state.replay.size() > state.options.bounds.max_replay_records) {
        state.replay.erase(state.replay.begin());
    }
}

inline const ReplayRecord* find_replay(const Impl& state, const IdempotencyKey& key) {
    for (const ReplayRecord& record : state.replay) {
        if (record.key == key) {
            return &record;
        }
    }
    return nullptr;
}

inline ThermalSnapshot build_snapshot(const Impl& state) {
    ThermalSnapshot snapshot;
    snapshot.epoch = state.epoch;
    snapshot.owner = state.incarnation;
    snapshot.revision = state.revision;
    snapshot.policy_present = state.policy_present;
    snapshot.policy = state.policy;
    snapshot.topology_generation = state.topology_generation;
    snapshot.evidence_generation = state.evidence_generation;
    snapshot.administrative_mode = state.administrative_mode;
    snapshot.administrative_hold = state.administrative_hold;
    snapshot.mode = state.mode_record;
    snapshot.gate = state.gate;
    snapshot.interlock_baseline_declared = state.interlock_baseline_declared;
    snapshot.interlocks = state.interlocks.all();
    snapshot.next_directive_id = state.next_directive_id;
    snapshot.next_audit_sequence = state.next_audit_sequence;
    snapshot.directives = state.directives;
    snapshot.audit = state.audit;
    snapshot.replay = state.replay;
    return snapshot;
}

/// Publish the current authoritative state.
///
/// A failed commit leaves the runtime faulted: further authority mutations are
/// refused until recover() re-reads the store, so in-memory authority can
/// never drift ahead of durable authority.
inline Status commit_locked(Impl& state) {
    if (state.store == nullptr) {
        return Status::success();
    }
    const auto committed = state.store->commit(build_snapshot(state));
    if (!committed.has_value()) {
        state.store_faulted = true;
        state.fault_detail = committed.error().render();
        return Status{committed.error()};
    }
    state.commit = committed.value();
    return Status::success();
}

inline Status require_writable(const Impl& state) {
    if (state.closed) {
        return Status::failure(ErrorCode::RUNTIME_CLOSED, "runtime is closed");
    }
    if (state.store_faulted) {
        return Status::failure(ErrorCode::STORE_IO,
                               "durable store is faulted; recover the runtime before mutating: " +
                                   state.fault_detail);
    }
    return Status::success();
}

inline PolicyOutcome evaluate_locked(const Impl& state, Tick at) {
    ThermalEnvironment environment;
    environment.evidence = &state.evidence;
    environment.limits = &state.limits;
    environment.interlocks = &state.interlocks;
    environment.topology_generation = state.topology_generation;

    PolicyInput input;
    input.now = at;
    input.current_mode = state.mode_record.mode;
    input.administrative_mode = state.administrative_mode;
    input.administrative_hold = state.administrative_hold;
    input.evidence_generation = state.evidence_generation;
    input.topology_generation = state.topology_generation;
    input.revision = state.revision;
    input.gate = state.gate;
    return evaluate_policy(state.policy, environment, input);
}

/// Apply an evaluation to the authoritative mode and gate.
///
/// Returns true when the authoritative mode changed.
inline bool apply_outcome(Impl& state, const PolicyOutcome& outcome, Tick at) {
    bool changed = false;
    if (outcome.mode != state.mode_record.mode) {
        state.mode_record.mode = outcome.mode;
        state.mode_record.changed_at = at;
        changed = true;
    }
    state.mode_record.required = outcome.required_mode;
    state.mode_record.cause = outcome.reason.cause;
    state.mode_record.reason_scope = outcome.reason.scope;
    state.mode_record.reason_limit = outcome.reason.limit;
    state.mode_record.reason_limit_defined = outcome.reason.limit_defined;
    state.gate = outcome.gate;
    state.last_outcome = outcome;
    state.have_last_outcome = true;
    return changed;
}

inline Result<std::vector<std::byte>> encode_attempt_digest(const AuthorityAttempt& attempt) {
    wire::Writer writer;
    writer.u64(attempt.request.value);
    writer.u8(static_cast<std::uint8_t>(attempt.kind));
    writer.u8(static_cast<std::uint8_t>(attempt.authority));
    writer.u64(attempt.binding.process.value);
    writer.u64(attempt.binding.epoch.value);
    writer.u64(attempt.binding.policy.value);
    writer.u64(attempt.binding.topology.value);
    writer.u64(attempt.binding.evidence.value);
    writer.u64(attempt.binding.revision.value);
    writer.boolean(attempt.facility_wide);
    writer.u32(attempt.scope.facility.value);
    writer.u64(attempt.scope.zone.value);
    writer.u64(attempt.scope.domain.value);
    writer.u32(attempt.requested_derate.value());
    writer.u8(static_cast<std::uint8_t>(attempt.requested_mode));
    writer.u8(static_cast<std::uint8_t>(attempt.coordination_kind));
    writer.u64(attempt.directive.value);
    auto encoded = std::move(writer).take();
    if (!encoded.has_value()) {
        return encoded.error();
    }
    if (encoded.value().size() > kMaxRequestDigestBytes) {
        return Error{ErrorCode::ENCODING_TOO_LARGE, "attempt digest exceeds the configured bound"};
    }
    return encoded;
}

inline void restore_authority(Impl& state, const ThermalSnapshot& snapshot, Tick at,
                              bool preserve_observations) {
    state.epoch = snapshot.epoch.next();
    state.revision = snapshot.revision;
    state.policy_present = snapshot.policy_present;
    state.policy = snapshot.policy;
    state.administrative_mode = snapshot.administrative_mode;
    state.administrative_hold = snapshot.administrative_hold;
    state.mode_record = snapshot.mode;
    // Dwell accumulated before a restart is not evidence that conditions have
    // been sustained since. The gate restarts from zero.
    state.gate = RecoveryGateState{};
    state.gate.target = step_down(snapshot.mode.mode);
    state.interlock_baseline_declared = snapshot.interlock_baseline_declared;
    const Status replaced = state.interlocks.replace_all(snapshot.interlocks);
    static_cast<void>(replaced);
    state.next_directive_id = snapshot.next_directive_id;
    state.next_audit_sequence = snapshot.next_audit_sequence;
    state.directives = snapshot.directives;
    state.audit = snapshot.audit;
    state.replay = snapshot.replay;
    state.have_last_outcome = false;

    if (preserve_observations && state.evidence.generation() == snapshot.evidence_generation &&
        state.evidence.topology_generation() == snapshot.topology_generation) {
        state.evidence.require_revalidation(at);
    } else {
        const Status reset = state.evidence.reset(snapshot.evidence_generation, snapshot.topology_generation);
        static_cast<void>(reset);
    }
    state.evidence_generation = snapshot.evidence_generation;
    state.topology_generation = snapshot.topology_generation;
}

/// Reach every scope a request applies to.
inline bool scope_affected(const Interlock& interlock, bool facility_wide, const ScopeRef& scope) {
    if (interlock.state != InterlockState::Asserted) {
        return false;
    }
    if (interlock.facility_wide) {
        return true;
    }
    if (facility_wide) {
        return true;
    }
    return interlock.scope == scope;
}

}  // namespace runtime_detail
}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_SRC_RUNTIME_IMPL_HPP
