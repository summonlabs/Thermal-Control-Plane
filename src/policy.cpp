// Thermal Control Plane — deterministic thermal policy evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/policy.hpp"

#include <cstddef>

namespace thermal_control_plane {
namespace {

/// The limit kind that caps the operating band of a mode.
LimitKind ceiling_kind_for(ThermalMode mode) {
    switch (mode) {
        case ThermalMode::Normal:
        case ThermalMode::Recovery:
        case ThermalMode::Maintenance: return LimitKind::Warning;
        case ThermalMode::Constrained: return LimitKind::Derate;
        case ThermalMode::Degraded: return LimitKind::Critical;
        case ThermalMode::Emergency:
        case ThermalMode::Isolated: return LimitKind::Shutdown;
    }
    return LimitKind::Warning;
}

bool next_lower_kind(LimitKind kind, LimitKind& out) {
    switch (kind) {
        case LimitKind::Shutdown: out = LimitKind::Critical; return true;
        case LimitKind::Critical: out = LimitKind::Derate; return true;
        case LimitKind::Derate: out = LimitKind::Warning; return true;
        case LimitKind::Warning: out = LimitKind::Advisory; return true;
        case LimitKind::Advisory: return false;
    }
    return false;
}

ThermalMode mode_for_violation(LimitKind kind) {
    switch (kind) {
        case LimitKind::Shutdown:
        case LimitKind::Critical: return ThermalMode::Emergency;
        case LimitKind::Derate: return ThermalMode::Degraded;
        case LimitKind::Warning: return ThermalMode::Constrained;
        case LimitKind::Advisory: return ThermalMode::Normal;
    }
    return ThermalMode::Normal;
}

EscalationCause cause_for_violation(LimitKind kind) {
    switch (kind) {
        case LimitKind::Shutdown: return EscalationCause::ShutdownLimitViolation;
        case LimitKind::Critical: return EscalationCause::CriticalLimitViolation;
        case LimitKind::Derate: return EscalationCause::DerateLimitViolation;
        case LimitKind::Warning: return EscalationCause::WarningLimitViolation;
        case LimitKind::Advisory: return EscalationCause::AdvisoryMargin;
    }
    return EscalationCause::Nominal;
}

/// Map the reason a scope has no usable headroom onto an escalation cause.
///
/// Every mapping is conservative: a scope whose evidence is missing, stale,
/// faulted, unsupported or being revalidated degrades the facility. None of
/// them is ever treated as nominal.
EscalationCause cause_for_headroom(HeadroomReason reason) {
    switch (reason) {
        case HeadroomReason::AwaitingRevalidation: return EscalationCause::AwaitingRevalidation;
        case HeadroomReason::EvidenceIndeterminate: return EscalationCause::EvidenceIndeterminate;
        case HeadroomReason::EvidenceFuture: return EscalationCause::EvidenceIndeterminate;
        case HeadroomReason::EvidenceStale: return EscalationCause::EvidenceStale;
        case HeadroomReason::GenerationMismatch: return EscalationCause::GenerationMismatch;
        case HeadroomReason::Interlocked: return EscalationCause::SafetyInterlock;
        case HeadroomReason::SensorAbsent:
        case HeadroomReason::SensorFaulted:
        case HeadroomReason::SourceUnavailable:
        case HeadroomReason::NotSupported:
        case HeadroomReason::NoEvidence:
        case HeadroomReason::LimitUndefined: return EscalationCause::EvidenceUnknown;
        case HeadroomReason::None: return EscalationCause::Nominal;
    }
    return EscalationCause::EvidenceUnknown;
}

}  // namespace

ThermalPolicy ThermalPolicy::baseline(PolicyId id, PolicyGeneration generation) {
    ThermalPolicy policy;
    policy.id = id;
    policy.generation = generation;
    policy.freshness = FreshnessWindow{Duration::from_millis_unchecked(5000)};
    policy.recovery_dwell = Duration::from_millis_unchecked(30000);
    policy.recovery_min_observations = 3;
    policy.hysteresis = TemperatureDelta{5000};
    policy.constrained_margin = TemperatureDelta{2000};
    policy.max_derate = BasisPoints::from_value_unchecked(3000);
    policy.unknown_evidence_derate = BasisPoints::from_value_unchecked(3000);
    policy.ladder[LimitKind::Advisory] = BasisPoints::from_value_unchecked(0);
    policy.ladder[LimitKind::Warning] = BasisPoints::from_value_unchecked(500);
    policy.ladder[LimitKind::Derate] = BasisPoints::from_value_unchecked(1500);
    policy.ladder[LimitKind::Critical] = BasisPoints::from_value_unchecked(2500);
    policy.ladder[LimitKind::Shutdown] = BasisPoints::from_value_unchecked(3000);
    return policy;
}

Status validate_policy(const ThermalPolicy& policy) {
    if (policy.id.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "policy has no identity");
    }
    if (policy.generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "policy has no generation");
    }
    if (policy.freshness.max_age.millis < 0) {
        return Status::failure(ErrorCode::POLICY_INVALID, "freshness window must not be negative");
    }
    if (policy.recovery_dwell.millis < 0) {
        return Status::failure(ErrorCode::POLICY_INVALID, "recovery dwell must not be negative");
    }
    if (policy.recovery_min_observations == 0) {
        return Status::failure(ErrorCode::POLICY_INVALID,
                               "recovery must require at least one distinct observation");
    }
    if (policy.recovery_min_observations > 4096) {
        return Status::failure(ErrorCode::POLICY_LIMIT_EXCEEDED,
                               "recovery observation requirement exceeds the supported bound");
    }
    if (policy.hysteresis.milli_celsius < 0) {
        return Status::failure(ErrorCode::POLICY_INVALID, "hysteresis must not be negative");
    }
    if (policy.constrained_margin.milli_celsius < 0) {
        return Status::failure(ErrorCode::POLICY_INVALID, "constrained margin must not be negative");
    }
    if (policy.unknown_evidence_derate > policy.max_derate) {
        return Status::failure(ErrorCode::POLICY_INVALID,
                               "unknown-evidence derating must not exceed the policy derating ceiling");
    }
    for (std::size_t i = 1; i < kLimitKindCount; ++i) {
        if (policy.ladder.at[i] < policy.ladder.at[i - 1]) {
            if (!policy.allow_non_monotone_derating) {
                return Status::failure(ErrorCode::POLICY_NON_MONOTONE_DERATING,
                                       "derating ladder must not decrease with limit severity");
            }
        }
    }
    if (!policy.allow_non_monotone_derating) {
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            if (policy.ladder.at[i] > policy.unknown_evidence_derate) {
                return Status::failure(
                    ErrorCode::POLICY_NON_MONOTONE_DERATING,
                    "unusable evidence must derate at least as hard as any defined threshold");
            }
        }
    }
    return Status::success();
}

PolicyOutcome evaluate_policy(const ThermalPolicy& policy,
                              const ThermalEnvironment& environment,
                              const PolicyInput& input) {
    PolicyOutcome out;
    out.previous_mode = input.current_mode;
    out.mode = input.current_mode;
    out.policy_generation = policy.generation;
    out.evidence_generation = input.evidence_generation;
    out.topology_generation = input.topology_generation;
    out.revision = input.revision;
    out.evaluated_at = input.now;

    // 1. Headroom for every scope that has published limits, in canonical
    //    order, so downstream selection is order-independent.
    if (environment.limits != nullptr) {
        for (const ScopeRef& scope : environment.limits->scopes()) {
            const ThermalLimitSet* limits = environment.limits->find(scope);
            ScopeEvidenceView view;
            view.scope = scope;
            if (environment.evidence != nullptr) {
                view = environment.evidence->view(scope, input.now, policy.freshness);
            }
            out.scopes.push_back(evaluate_headroom(limits, view));
        }
    }
    out.facility = aggregate_headroom(out.scopes);

    // 2. Collect every applicable cause and let precedence pick the winner.
    bool have_candidate = false;
    ThermalMode candidate_mode = ThermalMode::Normal;
    ModeReason candidate_reason{};
    const auto consider = [&have_candidate, &candidate_mode, &candidate_reason](ThermalMode mode,
                                                                              const ModeReason& reason) {
        if (!have_candidate) {
            have_candidate = true;
            candidate_mode = mode;
            candidate_reason = reason;
            return;
        }
        candidate_mode = escalate(candidate_mode, mode);
        if (reason_outranks(reason, candidate_reason)) {
            candidate_reason = reason;
        }
    };

    if (environment.interlocks != nullptr) {
        for (const Interlock& interlock : environment.interlocks->asserted()) {
            ModeReason reason;
            reason.scope = interlock.scope;
            switch (interlock.klass) {
                case InterlockClass::Safety:
                    reason.cause = EscalationCause::SafetyInterlock;
                    consider(ThermalMode::Emergency, reason);
                    break;
                case InterlockClass::Isolation:
                    reason.cause = EscalationCause::IsolationInterlock;
                    consider(ThermalMode::Isolated, reason);
                    break;
                case InterlockClass::Maintenance:
                    reason.cause = EscalationCause::MaintenanceWindow;
                    consider(ThermalMode::Maintenance, reason);
                    break;
            }
        }
    }

    for (const ScopeHeadroom& scope : out.scopes) {
        if (!scope.has_limits) {
            continue;
        }
        if (!scope.governing.known()) {
            ModeReason reason;
            reason.cause = cause_for_headroom(scope.governing.reason);
            reason.scope = scope.scope;
            consider(ThermalMode::Degraded, reason);
            continue;
        }
        for (std::size_t i = kLimitKindCount; i-- > 0;) {
            const Headroom& headroom = scope.by_kind[i];
            if (!headroom.limit.defined || !headroom.known()) {
                continue;
            }
            if (headroom.delta.milli_celsius > 0) {
                continue;
            }
            const auto kind = static_cast<LimitKind>(i + 1);
            ModeReason reason;
            reason.cause = cause_for_violation(kind);
            reason.scope = scope.scope;
            reason.limit = kind;
            reason.limit_defined = true;
            reason.sensor = scope.hottest_sensor;
            consider(mode_for_violation(kind), reason);
            break;
        }
        const Headroom& warning = scope.by_kind[limit_index(LimitKind::Warning)];
        if (warning.limit.defined && warning.known() &&
            warning.delta.milli_celsius > 0 &&
            warning.delta.milli_celsius <= policy.constrained_margin.milli_celsius) {
            ModeReason reason;
            reason.cause = EscalationCause::AdvisoryMargin;
            reason.scope = scope.scope;
            reason.limit = LimitKind::Warning;
            reason.limit_defined = true;
            reason.sensor = scope.hottest_sensor;
            consider(ThermalMode::Constrained, reason);
        }
    }

    bool administrative_isolated_ignored = false;
    if (input.administrative_mode != ThermalMode::Normal) {
        if (input.administrative_mode == ThermalMode::Isolated && policy.allow_automatic_isolation_exit &&
            (environment.interlocks == nullptr ||
             !environment.interlocks->any_asserted(InterlockClass::Isolation))) {
            // The policy allows the runtime to leave an administrative
            // isolation on its own once the thermal state permits it.
            administrative_isolated_ignored = true;
        } else {
            ModeReason reason;
            reason.cause = input.administrative_mode == ThermalMode::Maintenance
                               ? EscalationCause::MaintenanceWindow
                               : EscalationCause::OperatorDirective;
            consider(input.administrative_mode, reason);
        }
    }

    const ThermalMode required = have_candidate ? candidate_mode : ThermalMode::Normal;
    out.required_mode = required;

    // 3. The gate is advanced on every evaluation so that a favourable run is
    //    reset the moment the evidence stops supporting it.
    RecoveryGateInput gate_input;
    gate_input.current_mode = input.current_mode;
    gate_input.dwell = policy.recovery_dwell;
    gate_input.min_observations = policy.recovery_min_observations;
    gate_input.hysteresis = policy.hysteresis;
    gate_input.now = input.now;
    out.gate = advance_recovery_gate(input.gate, gate_input, out.scopes);
    out.recovery_eligible = out.gate.satisfied;

    // 4. Movement. Escalation is mandatory and immediate; de-escalation is
    //    gated, one rung at a time, and never automatic out of a held mode.
    bool held = input.administrative_hold;
    if (!administrative_isolated_ignored && requires_operator_to_leave(input.current_mode) &&
        input.administrative_mode == input.current_mode) {
        held = true;
    }

    ThermalMode mode = input.current_mode;
    bool escalated = false;
    bool de_escalated = false;
    bool deferred = false;
    if (is_more_severe(required, mode)) {
        mode = required;
        escalated = true;
    } else if (required != mode) {
        if (held) {
            deferred = true;
        } else if (mode_severity(required) == mode_severity(mode)) {
            de_escalated = mode_severity(required) < mode_severity(mode);
            mode = required;
        } else if (out.gate.satisfied) {
            const ThermalMode stepped = escalate(step_down(mode), required);
            de_escalated = mode_severity(stepped) < mode_severity(mode);
            mode = stepped;
        } else {
            deferred = true;
        }
    }
    out.mode = mode;
    out.escalated = escalated;
    out.de_escalated = de_escalated;
    out.transition_deferred = deferred;
    out.reason = have_candidate ? candidate_reason
                                : ModeReason{EscalationCause::Nominal, ScopeRef{}, LimitKind::Advisory, false,
                                             SensorRef{}};
    if (!held && mode_severity(mode) > mode_severity(required)) {
        ModeReason pending;
        pending.cause = EscalationCause::RecoveryPending;
        pending.scope = out.facility.has_worst ? out.facility.worst_scope : ScopeRef{};
        if (reason_outranks(pending, out.reason)) {
            out.reason = pending;
        }
    }

    // 5. Bounded derating for the worst scope.
    bool have_derate = false;
    for (const ScopeHeadroom& scope : out.scopes) {
        if (!scope.has_limits) {
            continue;
        }
        const ThermalLimitSet* limits =
            environment.limits == nullptr ? nullptr : environment.limits->find(scope.scope);
        const DerateSelection selection = select_derating(policy.ladder, policy.max_derate,
                                                          policy.unknown_evidence_derate, scope, limits);
        if (!have_derate || selection.derate > out.required_derate ||
            (selection.derate == out.required_derate && scope.scope < out.derate_scope)) {
            have_derate = true;
            out.required_derate = selection.derate;
            out.derate_scope = scope.scope;
            out.derate_from_unknown_evidence = selection.from_unknown_evidence;
            out.derate_limited_by_policy = selection.limited_by_policy;
        }
    }

    // 6. The envelope issued with any authority decision.
    out.envelope.facility_wide = true;
    out.envelope.mode = mode;
    out.envelope.policy_generation = policy.generation;
    out.envelope.topology_generation = input.topology_generation;
    out.envelope.evidence_generation = input.evidence_generation;
    out.envelope.revision = input.revision;
    out.envelope.issued_at = input.now;
    ScopeRef governing{};
    bool have_governing = false;
    if (out.facility.has_worst) {
        governing = out.facility.worst_scope;
        have_governing = true;
    } else if (!out.scopes.empty()) {
        governing = out.scopes.front().scope;
        have_governing = true;
    }
    if (have_governing) {
        out.envelope.scope = governing;
        const ThermalLimitSet* limits =
            environment.limits == nullptr ? nullptr : environment.limits->find(governing);
        if (limits != nullptr) {
            LimitKind kind = ceiling_kind_for(mode);
            while (true) {
                const Threshold& threshold = (*limits)[kind];
                if (threshold.defined) {
                    out.envelope.ceiling_defined = true;
                    out.envelope.ceiling = threshold;
                    out.envelope.ceiling_basis = kind;
                    break;
                }
                LimitKind lower = LimitKind::Advisory;
                if (!next_lower_kind(kind, lower)) {
                    out.envelope.ceiling_defined = false;
                    out.envelope.ceiling_basis = kind;
                    break;
                }
                kind = lower;
            }
            out.envelope.max_derate = policy.max_derate <= limits->max_derate ? policy.max_derate
                                                                             : limits->max_derate;
        }
    }
    return out;
}

}  // namespace thermal_control_plane
