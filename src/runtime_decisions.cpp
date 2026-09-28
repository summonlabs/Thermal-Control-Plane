// Thermal Control Plane — runtime configuration, evidence and authority decisions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "runtime_impl.hpp"

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

namespace thermal_control_plane {

using namespace runtime_detail;

namespace {

/// Compare a caller-supplied binding with the authoritative state.
///
/// The order is fixed and documented: incarnation, epoch, policy, topology,
/// evidence, revision. The first disagreement decides the refusal, so the same
/// stale attempt always returns the same primary code.
std::optional<Error> fence(const Impl& state, const AuthorityBinding& binding) {
    if (binding.process != state.incarnation) {
        return Error{ErrorCode::STALE_PROCESS_INCARNATION,
                     "binding names a different runtime incarnation"};
    }
    if (binding.epoch != state.epoch) {
        if (binding.epoch < state.epoch) {
            return Error{ErrorCode::STALE_EPOCH, "binding names a superseded control-plane epoch"};
        }
        return Error{ErrorCode::FUTURE_EPOCH, "binding names an epoch that has not been established"};
    }
    const PolicyGeneration installed =
        state.policy_present ? state.policy.generation : PolicyGeneration{};
    if (binding.policy != installed) {
        if (binding.policy < installed) {
            return Error{ErrorCode::STALE_POLICY_GENERATION, "binding names a superseded policy generation"};
        }
        return Error{ErrorCode::FUTURE_POLICY_GENERATION,
                     "binding names a policy generation that is not installed"};
    }
    if (binding.topology != state.topology_generation) {
        if (binding.topology < state.topology_generation) {
            return Error{ErrorCode::STALE_TOPOLOGY_GENERATION,
                         "binding names a superseded topology generation"};
        }
        return Error{ErrorCode::FUTURE_TOPOLOGY_GENERATION,
                     "binding names an unknown topology generation"};
    }
    if (binding.evidence != state.evidence_generation) {
        if (binding.evidence < state.evidence_generation) {
            return Error{ErrorCode::STALE_EVIDENCE_GENERATION,
                         "binding names a superseded evidence generation"};
        }
        return Error{ErrorCode::FUTURE_EVIDENCE_GENERATION,
                     "binding names an unestablished evidence generation"};
    }
    if (binding.revision != state.revision) {
        if (binding.revision < state.revision) {
            return Error{ErrorCode::STALE_STATE_REVISION, "binding names a superseded state revision"};
        }
        return Error{ErrorCode::FUTURE_STATE_REVISION, "binding names a state revision that does not exist"};
    }
    return std::nullopt;
}

BasisPoints scope_ceiling(const ThermalPolicy& policy, const ThermalLimitSet* limits) {
    BasisPoints ceiling = policy.max_derate;
    if (limits != nullptr && limits->max_derate < ceiling) {
        ceiling = limits->max_derate;
    }
    return ceiling;
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

Status ThermalRuntime::install_policy(const ThermalPolicy& policy, PolicyGeneration expected_current) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    const Status valid = validate_policy(policy);
    if (!valid.ok()) {
        return valid;
    }
    const PolicyGeneration installed = state.policy_present ? state.policy.generation : PolicyGeneration{};
    if (expected_current != installed) {
        if (expected_current < installed) {
            return Status::failure(ErrorCode::STALE_POLICY_GENERATION,
                                   "policy install expected generation is behind the installed generation");
        }
        return Status::failure(ErrorCode::FUTURE_POLICY_GENERATION,
                               "policy install expected generation is ahead of the installed generation");
    }
    if (policy.generation <= installed) {
        return Status::failure(ErrorCode::STALE_POLICY_GENERATION,
                               "a policy install must advance the policy generation");
    }
    const Tick at = state.clock->now();
    state.policy = policy;
    state.policy_present = true;
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::PolicyInstalled;
    audit.mode = state.mode_record.mode;
    audit.detail = "policy generation " + std::to_string(policy.generation.value);
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

Status ThermalRuntime::publish_limits(const ThermalLimitSet& limits) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    if (limits.topology_generation != state.topology_generation) {
        if (limits.topology_generation < state.topology_generation) {
            return Status::failure(ErrorCode::STALE_TOPOLOGY_GENERATION,
                                   "limits were published against a superseded topology generation");
        }
        return Status::failure(ErrorCode::FUTURE_TOPOLOGY_GENERATION,
                               "limits were published against an unknown topology generation");
    }
    const Status published = state.limits.put(limits);
    if (!published.ok()) {
        return published;
    }
    const Tick at = state.clock->now();
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::LimitsPublished;
    audit.mode = state.mode_record.mode;
    audit.detail = "limits for " + to_string(limits.scope);
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

Status ThermalRuntime::remove_limits(ScopeRef scope, LimitGeneration generation) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    const Status removed = state.limits.remove(scope, generation);
    if (!removed.ok()) {
        return removed;
    }
    const Tick at = state.clock->now();
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::LimitsRemoved;
    audit.mode = state.mode_record.mode;
    audit.detail = "limits withdrawn for " + to_string(scope);
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

Status ThermalRuntime::set_administrative_mode(ThermalMode mode, bool hold, const AuthorityBinding& binding) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    if (mode != ThermalMode::Normal && mode != ThermalMode::Maintenance && mode != ThermalMode::Isolated) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT,
                               "only Normal, Maintenance and Isolated are administrative modes");
    }
    const auto fenced = fence(state, binding);
    if (fenced.has_value()) {
        return Status{*fenced};
    }
    const Tick at = state.clock->now();
    state.administrative_mode = mode;
    state.administrative_hold = mode == ThermalMode::Normal ? false : hold;
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::AdministrativeModeSet;
    audit.mode = mode;
    audit.detail = std::string("administrative mode ") + std::string(to_string(mode));
    push_audit(state, at, audit);
    const PolicyOutcome outcome = evaluate_locked(state, at);
    if (apply_outcome(state, outcome, at)) {
        AuditInput transition;
        transition.kind = AuditKind::ModeTransition;
        transition.mode = outcome.mode;
        transition.detail = std::string("mode ") + std::string(to_string(outcome.mode));
        push_audit(state, at, transition);
    }
    return commit_locked(state);
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

Result<IngestReport> ThermalRuntime::ingest(const ObservationBatch& batch) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    if (state.store_faulted) {
        return Error{ErrorCode::STORE_IO, "durable store is faulted; recover the runtime before ingesting"};
    }
    const Tick at = state.clock->now();
    const auto report = state.evidence.ingest(batch, at);
    if (!report.has_value()) {
        return report.error();
    }
    AuditInput audit;
    audit.kind = AuditKind::EvidenceIngested;
    audit.mode = state.mode_record.mode;
    audit.detail = "evidence generation " + std::to_string(batch.generation.value) + " applied " +
                   std::to_string(report.value().applied) + " duplicate " +
                   std::to_string(report.value().duplicates);
    push_audit(state, at, audit);

    const bool mode_changed =
        state.policy_present ? apply_outcome(state, evaluate_locked(state, at), at) : false;
    if (mode_changed) {
        state.revision = state.revision.next();
        AuditInput transition;
        transition.kind = AuditKind::ModeTransition;
        transition.mode = state.mode_record.mode;
        transition.detail = "mode escalated by incoming evidence";
        push_audit(state, at, transition);
    }
    const Status committed = commit_locked(state);
    if (!committed.ok()) {
        return committed.error();
    }
    return report;
}

Status ThermalRuntime::advance_evidence_generation(EvidenceGeneration expected_current,
                                                   EvidenceGeneration next,
                                                   TopologyGeneration topology_generation) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    const Status advanced = state.evidence.advance_generation(expected_current, next, topology_generation);
    if (!advanced.ok()) {
        return advanced;
    }
    const Tick at = state.clock->now();
    state.evidence_generation = next;
    state.topology_generation = topology_generation;
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::EvidenceGenerationAdvanced;
    audit.mode = state.mode_record.mode;
    audit.detail = "evidence generation " + std::to_string(next.value) + " topology " +
                   std::to_string(topology_generation.value);
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

Status ThermalRuntime::declare_interlock_baseline(TopologyGeneration topology_generation,
                                                  std::vector<Interlock> asserted) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    if (topology_generation != state.topology_generation) {
        if (topology_generation < state.topology_generation) {
            return Status::failure(ErrorCode::STALE_TOPOLOGY_GENERATION,
                                   "interlock baseline names a superseded topology generation");
        }
        return Status::failure(ErrorCode::FUTURE_TOPOLOGY_GENERATION,
                               "interlock baseline names an unknown topology generation");
    }
    if (asserted.size() > state.options.bounds.max_interlocks) {
        return Status::failure(ErrorCode::RESOURCE_EXHAUSTED, "interlock baseline exceeds the registry bound");
    }
    for (const Interlock& interlock : asserted) {
        if (interlock.state != InterlockState::Asserted) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                   "an interlock baseline states what is asserted, not what is clear");
        }
        if (interlock.topology_generation != topology_generation) {
            return Status::failure(ErrorCode::STALE_TOPOLOGY_GENERATION,
                                   "interlock names a different topology generation than the baseline");
        }
    }
    const Tick at = state.clock->now();
    const Status replaced = state.interlocks.replace_all(asserted);
    if (!replaced.ok()) {
        return replaced;
    }
    state.interlock_baseline_declared = true;
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::InterlockBaselineDeclared;
    audit.mode = state.mode_record.mode;
    audit.detail = "interlock baseline with " + std::to_string(asserted.size()) + " asserted";
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

Status ThermalRuntime::apply_interlock(const Interlock& interlock) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    const Status writable = require_writable(state);
    if (!writable.ok()) {
        return writable;
    }
    if (!state.interlock_baseline_declared) {
        return Status::failure(ErrorCode::EVIDENCE_REQUIRED,
                               "an interlock baseline must be declared before individual updates");
    }
    if (interlock.topology_generation != state.topology_generation) {
        if (interlock.topology_generation < state.topology_generation) {
            return Status::failure(ErrorCode::STALE_TOPOLOGY_GENERATION,
                                   "interlock update names a superseded topology generation");
        }
        return Status::failure(ErrorCode::FUTURE_TOPOLOGY_GENERATION,
                               "interlock update names an unknown topology generation");
    }
    const Status applied = state.interlocks.apply(interlock);
    if (!applied.ok()) {
        return applied;
    }
    const Tick at = state.clock->now();
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::InterlockChanged;
    audit.mode = state.mode_record.mode;
    audit.detail = std::string("interlock ") + std::string(to_string(interlock.klass)) + " " +
                   std::string(to_string(interlock.state));
    push_audit(state, at, audit);
    static_cast<void>(apply_outcome(state, evaluate_locked(state, at), at));
    return commit_locked(state);
}

// ---------------------------------------------------------------------------
// Assessment
// ---------------------------------------------------------------------------

Result<PolicyOutcome> ThermalRuntime::assess() {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    if (!state.policy_present) {
        return Error{ErrorCode::UNKNOWN_POLICY, "no thermal policy is installed"};
    }
    return evaluate_locked(state, state.clock->now());
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

Result<AuthorityOutcome> ThermalRuntime::authorize(const AuthorityAttempt& attempt) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    if (state.store_faulted) {
        return Error{ErrorCode::STORE_IO, "durable store is faulted; recover the runtime before deciding"};
    }
    if (attempt.request.is_zero()) {
        return Error{ErrorCode::INVALID_ARGUMENT, "attempt has no request identity"};
    }
    if (attempt.attempt.is_zero()) {
        return Error{ErrorCode::INVALID_ARGUMENT, "attempt has no attempt identity"};
    }
    if (!attempt.key.valid()) {
        return Error{ErrorCode::INVALID_KEY, "attempt has no idempotency key"};
    }
    const auto digest = encode_attempt_digest(attempt);
    if (!digest.has_value()) {
        return digest.error();
    }

    const Tick at = state.clock->now();

    const auto refuse = [&](ErrorCode code, std::string detail,
                            bool record) -> Result<AuthorityOutcome> {
        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Refused;
        outcome.code = code;
        outcome.detail = std::move(detail);
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.revision_before = state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = state.mode_record.mode;
        outcome.required_mode = state.mode_record.required;
        outcome.cause = state.mode_record.cause;
        outcome.reason_scope = state.mode_record.reason_scope;
        outcome.reason_limit = state.mode_record.reason_limit;
        outcome.reason_limit_defined = state.mode_record.reason_limit_defined;
        outcome.decided_at = at;
        AuditInput audit;
        audit.kind = AuditKind::AuthorityRefused;
        audit.code = code;
        audit.request = attempt.request;
        audit.attempt = attempt.attempt;
        audit.mode = state.mode_record.mode;
        audit.detail = outcome.detail;
        push_audit(state, at, audit);
        if (record) {
            ReplayRecord entry;
            entry.key = attempt.key;
            entry.digest = digest.value();
            entry.outcome = outcome;
            entry.recorded_at_commit = state.commit.value;
            push_replay(state, std::move(entry));
        }
        const Status committed = commit_locked(state);
        if (!committed.ok()) {
            return committed.error();
        }
        outcome.commit = state.commit.value;
        outcome.durable = state.store != nullptr;
        return outcome;
    };

    const auto finish_grant = [&](AuthorityOutcome outcome,
                                  bool record) -> Result<AuthorityOutcome> {
        if (record) {
            ReplayRecord entry;
            entry.key = attempt.key;
            entry.digest = digest.value();
            entry.outcome = outcome;
            entry.recorded_at_commit = state.commit.value;
            push_replay(state, std::move(entry));
        }
        const Status committed = commit_locked(state);
        if (!committed.ok()) {
            return committed.error();
        }
        outcome.commit = state.commit.value;
        outcome.durable = state.store != nullptr;
        return outcome;
    };

    // Idempotency is resolved before any fence: a replay returns the recorded
    // outcome and re-actuates nothing.
    if (const ReplayRecord* existing = find_replay(state, attempt.key); existing != nullptr) {
        if (!(existing->digest == digest.value())) {
            return refuse(ErrorCode::IDEMPOTENCY_CONFLICT,
                          "idempotency key was already used for a semantically different attempt", false);
        }
        AuthorityOutcome outcome = existing->outcome;
        outcome.verdict = AuthorityVerdict::Replayed;
        outcome.replayed = true;
        outcome.attempt = attempt.attempt;
        outcome.decided_at = at;
        AuditInput audit;
        audit.kind = AuditKind::AuthorityReplayed;
        audit.code = outcome.code;
        audit.request = attempt.request;
        audit.attempt = attempt.attempt;
        audit.directive = outcome.directive;
        audit.mode = outcome.mode;
        audit.derate = outcome.granted_derate;
        audit.detail = "idempotent replay of an earlier attempt";
        push_audit(state, at, audit);
        const Status committed = commit_locked(state);
        if (!committed.ok()) {
            return committed.error();
        }
        outcome.commit = state.commit.value;
        outcome.durable = state.store != nullptr;
        return outcome;
    }

    if (const auto fenced = fence(state, attempt.binding); fenced.has_value()) {
        return refuse(fenced->code, fenced->detail, true);
    }
    if (!state.interlock_baseline_declared) {
        return refuse(ErrorCode::EVIDENCE_REQUIRED,
                      "no interlock baseline has been declared for this facility", true);
    }
    for (const Interlock& interlock : state.interlocks.asserted()) {
        if (!scope_affected(interlock, attempt.facility_wide, attempt.scope)) {
            continue;
        }
        if (interlock.klass == InterlockClass::Maintenance && attempt.authority == AuthorityClass::Operator) {
            continue;
        }
        return refuse(ErrorCode::INTERLOCK_ASSERTED,
                      std::string("interlock ") + std::string(to_string(interlock.klass)) + " is asserted",
                      true);
    }
    if (!state.policy_present) {
        return refuse(ErrorCode::UNKNOWN_POLICY, "no thermal policy is installed", true);
    }

    const bool needs_evidence = attempt.kind == AttemptKind::RequestDerate ||
                                attempt.kind == AttemptKind::RequestMode ||
                                attempt.kind == AttemptKind::RequestCoordination;
    const PolicyOutcome assessment = evaluate_locked(state, at);
    if (needs_evidence && assessment.facility.state != HeadroomState::Known) {
        return refuse(code_for_headroom(assessment.facility.reason),
                      describe_headroom(assessment.facility), true);
    }

    const bool mode_changed = apply_outcome(state, assessment, at);
    if (mode_changed) {
        state.revision = state.revision.next();
        AuditInput transition;
        transition.kind = AuditKind::ModeTransition;
        transition.mode = assessment.mode;
        transition.detail = "mode changed by policy evaluation";
        push_audit(state, at, transition);
    }

    if (attempt.kind == AttemptKind::AssessFacility) {
        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Granted;
        outcome.code = ErrorCode::NONE;
        outcome.detail = "assessment";
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.revision_before = state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = assessment.mode;
        outcome.required_mode = assessment.required_mode;
        outcome.escalated = assessment.escalated;
        outcome.de_escalated = assessment.de_escalated;
        outcome.cause = assessment.reason.cause;
        outcome.reason_scope = assessment.reason.scope;
        outcome.reason_limit = assessment.reason.limit;
        outcome.reason_limit_defined = assessment.reason.limit_defined;
        outcome.granted_derate = assessment.required_derate;
        outcome.derate_from_unknown_evidence = assessment.derate_from_unknown_evidence;
        outcome.derate_limited_by_policy = assessment.derate_limited_by_policy;
        outcome.envelope = assessment.envelope;
        outcome.decided_at = at;
        if (mode_changed) {
            AuditInput audit;
            audit.kind = AuditKind::AuthorityGranted;
            audit.request = attempt.request;
            audit.attempt = attempt.attempt;
            audit.mode = assessment.mode;
            audit.detail = "assessment with a mandatory mode escalation";
            push_audit(state, at, audit);
            return finish_grant(outcome, false);
        }
        outcome.commit = state.commit.value;
        outcome.durable = state.store != nullptr;
        return outcome;
    }

    if (attempt.kind == AttemptKind::RequestDerate) {
        if (attempt.facility_wide && !attempt.scope.is_zero()) {
            return refuse(ErrorCode::INVALID_ARGUMENT,
                          "a facility-wide attempt must not name a scope", true);
        }
        if (!attempt.facility_wide && attempt.scope.is_zero()) {
            return refuse(ErrorCode::INVALID_ARGUMENT, "a scope-bound attempt must name a scope", true);
        }
        const ThermalLimitSet* scoped_limits = nullptr;
        if (!attempt.facility_wide) {
            scoped_limits = state.limits.find(attempt.scope);
            if (scoped_limits == nullptr) {
                return refuse(ErrorCode::UNKNOWN_LIMIT_SET,
                              "no limits are published for the requested scope", true);
            }
        }
        if (attempt.requested_derate > scope_ceiling(state.policy, scoped_limits)) {
            return refuse(ErrorCode::DERATE_OUT_OF_BOUNDS,
                          "requested derating exceeds the authority ceiling", true);
        }

        BasisPoints required = assessment.required_derate;
        ScopeRef target_scope = assessment.derate_scope;
        bool from_unknown = assessment.derate_from_unknown_evidence;
        bool limited = assessment.derate_limited_by_policy;
        if (!attempt.facility_wide) {
            const ScopeHeadroom* headroom = nullptr;
            for (const ScopeHeadroom& scope : assessment.scopes) {
                if (scope.scope == attempt.scope) {
                    headroom = &scope;
                    break;
                }
            }
            if (headroom == nullptr) {
                return refuse(ErrorCode::UNKNOWN_SCOPE,
                              "the requested scope has no published limits", true);
            }
            const DerateSelection selection = select_derating(
                state.policy.ladder, state.policy.max_derate, state.policy.unknown_evidence_derate,
                *headroom, scoped_limits);
            required = selection.derate;
            target_scope = attempt.scope;
            from_unknown = selection.from_unknown_evidence;
            limited = selection.limited_by_policy;
        }
        if (attempt.requested_derate < required) {
            return refuse(ErrorCode::DERATE_INSUFFICIENT,
                          "requested derating is below what current evidence requires", true);
        }

        state.revision = state.revision.next();
        DeratingDirective directive;
        directive.id = DirectiveId{state.next_directive_id};
        state.next_directive_id += 1;
        directive.request = attempt.request;
        directive.attempt = attempt.attempt;
        directive.scope = target_scope;
        directive.facility_wide = attempt.facility_wide;
        directive.requested = attempt.requested_derate;
        directive.granted = attempt.requested_derate;
        directive.basis = assessment.envelope.ceiling_basis;
        directive.basis_defined = assessment.envelope.ceiling_defined;
        directive.from_unknown_evidence = from_unknown;
        directive.limited_by_policy = limited;
        directive.limit = assessment.envelope.ceiling;
        directive.policy_generation = state.policy.generation;
        directive.topology_generation = state.topology_generation;
        directive.evidence_generation = state.evidence_generation;
        directive.revision = state.revision;
        directive.issued_at = at;
        directive.authority = attempt.authority;
        for (const ScopeHeadroom& scope : assessment.scopes) {
            if (scope.scope == target_scope && scope.has_hottest) {
                directive.observed = scope.hottest;
                directive.observed_sequence = scope.hottest_sequence;
                break;
            }
        }
        push_directive(state, directive);

        AuditInput audit;
        audit.kind = AuditKind::AuthorityGranted;
        audit.request = attempt.request;
        audit.attempt = attempt.attempt;
        audit.directive = directive.id;
        audit.mode = assessment.mode;
        audit.derate = directive.granted;
        audit.detail = "derating directive granted";
        push_audit(state, at, audit);

        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Granted;
        outcome.code = ErrorCode::NONE;
        outcome.detail = "derating directive granted";
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.directive = directive.id;
        outcome.revision_before = state.revision.value == 0 ? state.revision : state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = assessment.mode;
        outcome.required_mode = assessment.required_mode;
        outcome.escalated = assessment.escalated;
        outcome.de_escalated = assessment.de_escalated;
        outcome.cause = assessment.reason.cause;
        outcome.reason_scope = assessment.reason.scope;
        outcome.reason_limit = assessment.reason.limit;
        outcome.reason_limit_defined = assessment.reason.limit_defined;
        outcome.granted_derate = directive.granted;
        outcome.derate_from_unknown_evidence = from_unknown;
        outcome.derate_limited_by_policy = limited;
        outcome.envelope = assessment.envelope;
        outcome.envelope.revision = state.revision;
        outcome.decided_at = at;
        return finish_grant(outcome, true);
    }

    if (attempt.kind == AttemptKind::RequestMode) {
        const ThermalMode requested = attempt.requested_mode;
        if (requested == state.mode_record.mode) {
            AuthorityOutcome outcome;
            outcome.verdict = AuthorityVerdict::Granted;
            outcome.code = ErrorCode::NONE;
            outcome.detail = "requested mode is already authoritative";
            outcome.request = attempt.request;
            outcome.attempt = attempt.attempt;
            outcome.revision_before = state.revision;
            outcome.revision_after = state.revision;
            outcome.mode = state.mode_record.mode;
            outcome.required_mode = assessment.required_mode;
            outcome.cause = state.mode_record.cause;
            outcome.envelope = assessment.envelope;
            outcome.decided_at = at;
            outcome.commit = state.commit.value;
            outcome.durable = state.store != nullptr;
            return outcome;
        }
        if (!is_more_severe(requested, state.mode_record.mode)) {
            if (is_more_severe(assessment.required_mode, requested)) {
                return refuse(ErrorCode::ESCALATION_REQUIRED,
                              "current evidence requires a more severe mode than the one requested", true);
            }
            return refuse(ErrorCode::RECOVERY_NOT_ELIGIBLE,
                          "the recovery gate has not completed for a step-down to the requested mode",
                          true);
        }
        state.mode_record.mode = requested;
        state.mode_record.changed_at = at;
        if (requested == ThermalMode::Maintenance || requested == ThermalMode::Isolated) {
            state.administrative_mode = requested;
            state.administrative_hold = true;
        }
        state.revision = state.revision.next();
        AuditInput transition;
        transition.kind = AuditKind::ModeTransition;
        transition.request = attempt.request;
        transition.attempt = attempt.attempt;
        transition.mode = requested;
        transition.detail = std::string("mode ") + std::string(to_string(requested)) + " requested and applied";
        push_audit(state, at, transition);

        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Granted;
        outcome.code = ErrorCode::NONE;
        outcome.detail = "mode transition granted";
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.revision_before = state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = requested;
        outcome.required_mode = assessment.required_mode;
        outcome.escalated = true;
        outcome.cause = assessment.reason.cause;
        outcome.envelope = assessment.envelope;
        outcome.envelope.mode = requested;
        outcome.envelope.revision = state.revision;
        outcome.decided_at = at;
        return finish_grant(outcome, true);
    }

    if (attempt.kind == AttemptKind::RequestCoordination) {
        if (state.outbound.size() >= state.options.bounds.max_outbound) {
            return refuse(ErrorCode::RESOURCE_EXHAUSTED,
                          "the outbound coordination queue is full", true);
        }
        state.revision = state.revision.next();
        CoordinationRequest request;
        request.request = attempt.request;
        request.attempt = attempt.attempt;
        request.kind = attempt.coordination_kind;
        request.facility_wide = attempt.facility_wide;
        request.scope = attempt.facility_wide ? assessment.envelope.scope : attempt.scope;
        request.mode = assessment.mode;
        request.required_mode = assessment.required_mode;
        request.derate = assessment.required_derate;
        request.headroom_state = assessment.facility.state;
        request.headroom_reason = assessment.facility.reason;
        request.headroom = assessment.facility.worst_delta;
        request.binding.process = state.incarnation;
        request.binding.epoch = state.epoch;
        request.binding.policy = state.policy.generation;
        request.binding.topology = state.topology_generation;
        request.binding.evidence = state.evidence_generation;
        request.binding.revision = state.revision;
        request.issued_at = at;
        state.outbound.push_back(request);

        AuditInput audit;
        audit.kind = AuditKind::CoordinationEmitted;
        audit.request = attempt.request;
        audit.attempt = attempt.attempt;
        audit.mode = assessment.mode;
        audit.derate = assessment.required_derate;
        audit.detail = std::string("coordination ") + std::string(to_string(attempt.coordination_kind));
        push_audit(state, at, audit);

        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Granted;
        outcome.code = ErrorCode::NONE;
        outcome.detail = "coordination request emitted";
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.revision_before = state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = assessment.mode;
        outcome.required_mode = assessment.required_mode;
        outcome.cause = assessment.reason.cause;
        outcome.granted_derate = assessment.required_derate;
        outcome.envelope = assessment.envelope;
        outcome.envelope.revision = state.revision;
        outcome.coordination = request;
        outcome.decided_at = at;
        return finish_grant(outcome, true);
    }

    // AcknowledgeDirective.
    if (attempt.directive.is_zero()) {
        return refuse(ErrorCode::INVALID_ARGUMENT, "acknowledgement names no directive", true);
    }
    DeratingDirective* target = nullptr;
    for (DeratingDirective& directive : state.directives) {
        if (directive.id == attempt.directive) {
            target = &directive;
            break;
        }
    }
    if (target == nullptr) {
        return refuse(ErrorCode::UNKNOWN_DIRECTIVE,
                      "directive is not present in the retained directive tail", true);
    }
    if (target->acknowledged) {
        AuthorityOutcome outcome;
        outcome.verdict = AuthorityVerdict::Granted;
        outcome.code = ErrorCode::NONE;
        outcome.detail = "directive was already acknowledged";
        outcome.request = attempt.request;
        outcome.attempt = attempt.attempt;
        outcome.directive = target->id;
        outcome.revision_before = state.revision;
        outcome.revision_after = state.revision;
        outcome.mode = state.mode_record.mode;
        outcome.decided_at = at;
        outcome.commit = state.commit.value;
        outcome.durable = state.store != nullptr;
        return outcome;
    }
    target->acknowledged = true;
    target->acknowledged_at = at;
    state.revision = state.revision.next();
    AuditInput audit;
    audit.kind = AuditKind::AuthorityGranted;
    audit.request = attempt.request;
    audit.attempt = attempt.attempt;
    audit.directive = target->id;
    audit.mode = state.mode_record.mode;
    audit.detail = "directive acknowledged";
    push_audit(state, at, audit);

    AuthorityOutcome outcome;
    outcome.verdict = AuthorityVerdict::Granted;
    outcome.code = ErrorCode::NONE;
    outcome.detail = "directive acknowledged";
    outcome.request = attempt.request;
    outcome.attempt = attempt.attempt;
    outcome.directive = target->id;
    outcome.revision_before = state.revision;
    outcome.revision_after = state.revision;
    outcome.mode = state.mode_record.mode;
    outcome.decided_at = at;
    return finish_grant(outcome, true);
}

Result<VerificationResult> ThermalRuntime::verify_directive(DirectiveId directive_id) {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    const Tick at = state.clock->now();
    const DeratingDirective* directive = nullptr;
    for (const DeratingDirective& candidate : state.directives) {
        if (candidate.id == directive_id) {
            directive = &candidate;
            break;
        }
    }
    if (directive == nullptr) {
        return Error{ErrorCode::UNKNOWN_DIRECTIVE, "directive is not present in the retained directive tail"};
    }

    VerificationResult result;
    result.directive = directive_id;

    const auto record = [&](VerificationResult value) -> Result<VerificationResult> {
        state.verifications[value.directive] = value;
        while (state.verifications.size() > state.options.bounds.max_verifications) {
            state.verifications.erase(state.verifications.begin());
        }
        AuditInput audit;
        audit.kind = AuditKind::VerificationRecorded;
        audit.code = value.code;
        audit.directive = value.directive;
        audit.mode = state.mode_record.mode;
        audit.detail = std::string("verification ") + std::string(to_string(value.state));
        push_audit(state, at, audit);
        const Status committed = commit_locked(state);
        if (!committed.ok()) {
            return committed.error();
        }
        return value;
    };

    for (const DeratingDirective& candidate : state.directives) {
        if (candidate.id > directive->id && candidate.scope == directive->scope) {
            result.state = VerificationState::Superseded;
            result.code = ErrorCode::STALE_DIRECTIVE;
            result.detail = "a newer directive exists for the same scope";
            return record(result);
        }
    }

    const ThermalLimitSet* limits = state.limits.find(directive->scope);
    if (limits == nullptr) {
        result.state = VerificationState::Unproven;
        result.code = ErrorCode::UNKNOWN_LIMIT_SET;
        result.detail = "limits for the directive scope are no longer published";
        return record(result);
    }
    const FreshnessWindow window = state.policy_present ? state.policy.freshness : FreshnessWindow{};
    const ScopeEvidenceView view = state.evidence.view(directive->scope, at, window);
    if (!view.has_hottest) {
        result.state = VerificationState::Unproven;
        result.code = ErrorCode::VERIFICATION_UNPROVEN;
        result.detail = "no fresh observation exists for the directive scope";
        return record(result);
    }
    result.observed = true;
    result.observed_temperature = view.hottest;
    result.sensor = view.hottest_sensor;
    result.sequence = view.hottest_sequence;
    result.observed_at = view.hottest_at;
    if (view.hottest_sequence <= directive->observed_sequence || view.hottest_at < directive->issued_at) {
        result.state = VerificationState::Unproven;
        result.code = ErrorCode::VERIFICATION_UNPROVEN;
        result.detail = "the newest observation is not newer than the directive";
        return record(result);
    }

    Threshold threshold = directive->limit;
    if (!threshold.defined) {
        threshold = (*limits)[directive->basis];
    }
    if (!threshold.defined) {
        result.state = VerificationState::Unproven;
        result.code = ErrorCode::LIMIT_UNDEFINED;
        result.detail = "the directive has no defined basis limit";
        return record(result);
    }
    if (view.hottest.milli_celsius() < threshold.milli_celsius) {
        result.state = VerificationState::Proven;
        result.code = ErrorCode::NONE;
        result.detail = "the scope is back below the directive basis limit";
        return record(result);
    }
    result.state = VerificationState::Contradicted;
    result.code = ErrorCode::VERIFICATION_CONTRADICTED;
    result.detail = "the scope is still at or above the directive basis limit";
    return record(result);
}

}  // namespace thermal_control_plane
