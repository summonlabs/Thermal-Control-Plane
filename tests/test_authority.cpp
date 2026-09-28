// Thermal Control Plane — authority fencing, idempotency and refusal proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include <string>

namespace {

using namespace thermal_control_plane;

SyntheticZoneSpec make_zone(std::uint64_t zone, std::uint64_t domain, std::uint64_t sensor) {
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{zone};
    spec.domain = DomainRef{domain};
    spec.sensor = SensorRef{sensor};
    spec.scope_generation = ScopeGeneration{1};
    spec.baseline_milli_c = 30000;
    spec.load_rise_milli_c = 45000;
    spec.derate_relief_milli_c = 60000;
    spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
    spec.max_derate = BasisPoints::from_value_unchecked(4000);
    return spec;
}

struct Harness {
    SyntheticFacility facility;
    std::unique_ptr<ThermalRuntime> runtime;
    ManualClock* clock = nullptr;

    explicit Harness(const std::string& store_path = std::string())
        : facility(FacilityId{1}, TopologyGeneration{1},
                   {make_zone(10, 100, 1), make_zone(11, 101, 2)}) {
        auto manual = std::make_unique<ManualClock>(Tick{1000});
        clock = manual.get();
        RuntimeOptions options;
        options.store_path = store_path;
        auto created = ThermalRuntime::create(options, std::move(manual));
        if (created.has_value()) {
            runtime = std::move(created.value());
        }
    }

    bool prepare(bool with_interlocks = true) {
        if (runtime == nullptr) {
            return false;
        }
        const ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
        if (!runtime->install_policy(policy, PolicyGeneration{}).ok()) {
            return false;
        }
        // Evidence can be produced before limits are published: the observation
        // stream does not depend on the facility's declared envelope. Publishing
        // limits into a runtime with no evidence correctly degrades the facility
        // until the evidence arrives, which is a separate case.
        if (!feed(0)) {
            return false;
        }
        const LimitGeneration generation{1};
        for (const ThermalLimitSet& limits : facility.all_limits(generation)) {
            if (!runtime->publish_limits(limits).ok()) {
                return false;
            }
        }
        if (!with_interlocks) {
            return true;
        }
        return runtime->declare_interlock_baseline(runtime->topology_generation(), {}).ok();
    }

    bool feed(std::uint32_t derate_bp) {
        const ObservationBatch batch = facility.observe(clock->peek(),
                                                        BasisPoints::from_value_unchecked(derate_bp),
                                                        runtime->incarnation(),
                                                        runtime->evidence_generation());
        return runtime->ingest(batch).has_value();
    }

    ScopeRef first_scope() const { return facility.scopes().at(0); }
};

AuthorityAttempt attempt_of(Harness& harness, RequestId request, AttemptId attempt, const char* key,
                            AttemptKind kind) {
    return make_attempt(*harness.runtime, request, attempt, key, kind);
}

/// Re-read the current binding. Every granted decision advances the state
/// revision, so a reused attempt envelope is refused as stale by design.
void rebind(Harness& harness, AuthorityAttempt& attempt) {
    attempt.binding = harness.runtime->binding().value();
}

}  // namespace

TCPLANE_CASE(authority, binding_fences_are_deterministic_and_ordered) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());

    TCPLANE_PHASE("a correctly bound attempt is not fenced");
    AuthorityOutcome outcome = harness.runtime
                                   ->authorize(attempt_of(harness, RequestId{1}, AttemptId{1}, "k1",
                                                          AttemptKind::AssessFacility))
                                   .value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Granted);

    TCPLANE_PHASE("a stale state revision is refused");
    AuthorityAttempt attempt = attempt_of(harness, RequestId{2}, AttemptId{2}, "k2", AttemptKind::RequestDerate);
    attempt.binding.revision = StateRevision{attempt.binding.revision.value - 1};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Refused);
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::STALE_STATE_REVISION));

    TCPLANE_PHASE("a future state revision is refused with its own code");
    attempt = attempt_of(harness, RequestId{3}, AttemptId{3}, "k3", AttemptKind::RequestDerate);
    attempt.binding.revision = StateRevision{attempt.binding.revision.value + 5};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::FUTURE_STATE_REVISION));

    TCPLANE_PHASE("a stale epoch is refused");
    attempt = attempt_of(harness, RequestId{4}, AttemptId{4}, "k4", AttemptKind::RequestDerate);
    attempt.binding.epoch = ControlPlaneEpoch{attempt.binding.epoch.value - 1};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code), static_cast<std::uint32_t>(ErrorCode::STALE_EPOCH));

    TCPLANE_PHASE("a future epoch is refused");
    attempt = attempt_of(harness, RequestId{5}, AttemptId{5}, "k5", AttemptKind::RequestDerate);
    attempt.binding.epoch = ControlPlaneEpoch{attempt.binding.epoch.value + 1};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code), static_cast<std::uint32_t>(ErrorCode::FUTURE_EPOCH));

    TCPLANE_PHASE("a stale policy generation is refused");
    attempt = attempt_of(harness, RequestId{6}, AttemptId{6}, "k6", AttemptKind::RequestDerate);
    attempt.binding.policy = PolicyGeneration{0};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::STALE_POLICY_GENERATION));

    TCPLANE_PHASE("a stale topology generation is refused");
    attempt = attempt_of(harness, RequestId{7}, AttemptId{7}, "k7", AttemptKind::RequestDerate);
    attempt.binding.topology = TopologyGeneration{0};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::STALE_TOPOLOGY_GENERATION));

    TCPLANE_PHASE("a stale evidence generation is refused");
    attempt = attempt_of(harness, RequestId{8}, AttemptId{8}, "k8", AttemptKind::RequestDerate);
    attempt.binding.evidence = EvidenceGeneration{0};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::STALE_EVIDENCE_GENERATION));

    TCPLANE_PHASE("a future evidence generation is refused");
    attempt = attempt_of(harness, RequestId{9}, AttemptId{9}, "k9", AttemptKind::RequestDerate);
    attempt.binding.evidence = EvidenceGeneration{attempt.binding.evidence.value + 1};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::FUTURE_EVIDENCE_GENERATION));

    TCPLANE_PHASE("an attempt bound to another process incarnation is refused");
    attempt = attempt_of(harness, RequestId{10}, AttemptId{10}, "k10", AttemptKind::RequestDerate);
    attempt.binding.process = ProcessIncarnation{attempt.binding.process.value + 1};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::STALE_PROCESS_INCARNATION));

    TCPLANE_PHASE("no directive and no coordination escaped any of those refusals");
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), std::size_t{0});
    TCPLANE_CHECK_EQ(harness.runtime->outbound_size(), std::size_t{0});
}

TCPLANE_CASE(authority, missing_interlock_statement_is_not_permission) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare(false));
    TCPLANE_PHASE("authority is refused until an interlock baseline is declared");
    const auto outcome = harness.runtime
                             ->authorize(attempt_of(harness, RequestId{1}, AttemptId{1}, "k1",
                                                    AttemptKind::RequestDerate))
                             .value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Refused);
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::EVIDENCE_REQUIRED));
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), std::size_t{0});

    TCPLANE_PHASE("an individual interlock update is refused before a baseline exists");
    Interlock interlock;
    interlock.id = InterlockId{1};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = true;
    interlock.topology_generation = harness.runtime->topology_generation();
    interlock.observed_at = harness.clock->peek();
    TCPLANE_STATUS_ERROR_CODE(harness.runtime->apply_interlock(interlock), ErrorCode::EVIDENCE_REQUIRED);
}

TCPLANE_CASE(authority, asserted_interlocks_refuse_authority) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());

    TCPLANE_PHASE("a scoped safety interlock refuses an attempt on that scope");
    Interlock interlock;
    interlock.id = InterlockId{1};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = false;
    interlock.scope = harness.first_scope();
    interlock.scope_generation = ScopeGeneration{1};
    interlock.topology_generation = harness.runtime->topology_generation();
    interlock.observed_at = harness.clock->peek();
    interlock.source = "synthetic-bms";
    TCPLANE_STATUS_OK(harness.runtime->apply_interlock(interlock));

    AuthorityAttempt attempt = attempt_of(harness, RequestId{1}, AttemptId{1}, "k1", AttemptKind::RequestDerate);
    attempt.facility_wide = false;
    attempt.scope = harness.first_scope();
    AuthorityOutcome outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::INTERLOCK_ASSERTED));
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), std::size_t{0});
    TCPLANE_CHECK_EQ(harness.runtime->outbound_size(), std::size_t{0});

    TCPLANE_PHASE("a facility-wide attempt is refused by the same scoped interlock");
    attempt.facility_wide = true;
    attempt.scope = ScopeRef{};
    attempt.request = RequestId{2};
    attempt.attempt = AttemptId{2};
    attempt.key = IdempotencyKey::parse("k2").value();
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::INTERLOCK_ASSERTED));

    TCPLANE_PHASE("clearing the interlock restores the ability to decide");
    attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
    interlock.state = InterlockState::Cleared;
    interlock.observed_at = harness.clock->peek();
    TCPLANE_STATUS_OK(harness.runtime->apply_interlock(interlock));
    attempt.request = RequestId{3};
    attempt.attempt = AttemptId{3};
    attempt.key = IdempotencyKey::parse("k3").value();
    rebind(harness, attempt);
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict != AuthorityVerdict::Refused);

    TCPLANE_PHASE("an interlock cannot be cleared twice out of order");
    Interlock older = interlock;
    older.observed_at = Tick{1};
    TCPLANE_STATUS_ERROR_CODE(harness.runtime->apply_interlock(older), ErrorCode::STALE_DIRECTIVE);
}

TCPLANE_CASE(authority, derating_bounds_and_insufficiency) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());
    TCPLANE_PHASE("both synthetic zones are driven above the derate threshold");
    for (std::size_t index = 0; index < harness.facility.zone_count(); ++index) {
        harness.facility.override_temperature(index, 90000);
    }
    TCPLANE_CHECK(harness.feed(0));
    TCPLANE_CHECK(harness.runtime->mode() == ThermalMode::Degraded);

    TCPLANE_PHASE("a derating below the evidence requirement is refused as insufficient");
    AuthorityAttempt attempt = attempt_of(harness, RequestId{1}, AttemptId{1}, "a1", AttemptKind::RequestDerate);
    attempt.requested_derate = BasisPoints::from_value_unchecked(500);
    AuthorityOutcome outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::DERATE_INSUFFICIENT));

    TCPLANE_PHASE("a derating above the authority ceiling is refused as out of bounds");
    attempt.request = RequestId{2};
    attempt.attempt = AttemptId{2};
    attempt.key = IdempotencyKey::parse("a2").value();
    attempt.requested_derate = BasisPoints::from_value_unchecked(4001);
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::DERATE_OUT_OF_BOUNDS));

    TCPLANE_PHASE("a scope-bound attempt naming no scope is refused");
    attempt.request = RequestId{3};
    attempt.attempt = AttemptId{3};
    attempt.key = IdempotencyKey::parse("a3").value();
    attempt.requested_derate = BasisPoints::from_value_unchecked(0);
    attempt.facility_wide = false;
    attempt.scope = ScopeRef{};
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::INVALID_ARGUMENT));

    TCPLANE_PHASE("a scope with no published limits is refused");
    attempt.facility_wide = false;
    attempt.scope = make_scope(1, 99, 99);
    attempt.request = RequestId{4};
    attempt.attempt = AttemptId{4};
    attempt.key = IdempotencyKey::parse("a4").value();
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::UNKNOWN_LIMIT_SET));

    TCPLANE_PHASE("nothing was actuated by any refusal");
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), std::size_t{0});
    TCPLANE_CHECK_EQ(harness.runtime->outbound_size(), std::size_t{0});
}

TCPLANE_CASE(authority, grants_are_bounded_recorded_and_replayable) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());
    TCPLANE_CHECK(harness.feed(0));
    // Heat both zones above the derate threshold so the assessment requires one.
    TCPLANE_CHECK(harness.feed(0));
    const StateRevision before = harness.runtime->revision();
    TCPLANE_CHECK(harness.runtime->mode() == ThermalMode::Constrained);
    TCPLANE_NOTE("commissioned at revision " + std::to_string(before.value) + " mode " +
                 std::string(to_string(harness.runtime->mode())));

    TCPLANE_PHASE("an attempt that meets the requirement is granted with a directive");
    AuthorityAttempt attempt = attempt_of(harness, RequestId{1}, AttemptId{1}, "grant-1", AttemptKind::RequestDerate);
    attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
    AuthorityOutcome outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(!outcome.directive.is_zero());
    TCPLANE_CHECK_EQ(outcome.granted_derate.value(), std::uint32_t{3000});
    TCPLANE_CHECK(harness.runtime->revision() != before);
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), std::size_t{1});

    const std::size_t directives_after_grant = harness.runtime->directive_tail(64).size();
    const StateRevision revision_after_grant = harness.runtime->revision();

    TCPLANE_PHASE("a lost-response retry replays the original outcome without re-actuating");
    AuthorityAttempt retry = attempt;
    retry.attempt = AttemptId{2};
    const AuthorityOutcome replayed = harness.runtime->authorize(retry).value();
    TCPLANE_CHECK(replayed.verdict == AuthorityVerdict::Replayed);
    TCPLANE_CHECK(replayed.replayed);
    TCPLANE_CHECK(replayed.directive == outcome.directive);
    TCPLANE_CHECK_EQ(replayed.granted_derate.value(), outcome.granted_derate.value());
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), directives_after_grant);
    TCPLANE_CHECK(harness.runtime->revision() == revision_after_grant);

    TCPLANE_PHASE("reusing the key with different semantics conflicts instead of replaying");
    AuthorityAttempt conflicting = attempt;
    conflicting.attempt = AttemptId{3};
    conflicting.requested_derate = BasisPoints::from_value_unchecked(2500);
    const AuthorityOutcome conflict = harness.runtime->authorize(conflicting).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(conflict.code),
                     static_cast<std::uint32_t>(ErrorCode::IDEMPOTENCY_CONFLICT));
    TCPLANE_CHECK_EQ(harness.runtime->directive_tail(64).size(), directives_after_grant);

    TCPLANE_PHASE("a directive is not proof that anything changed");
    const auto verification = harness.runtime->verify_directive(outcome.directive);
    TCPLANE_OK(verification);
    TCPLANE_CHECK(verification.value().state == VerificationState::Unproven);

    TCPLANE_PHASE("applying the derating and re-observing proves the outcome");
    TCPLANE_CHECK(harness.feed(3000));
    harness.clock->advance(500);
    const auto proven = harness.runtime->verify_directive(outcome.directive);
    TCPLANE_OK(proven);
    TCPLANE_CHECK(proven.value().state == VerificationState::Proven);
    TCPLANE_CHECK(proven.value().observed_temperature.milli_celsius() < 75000);

    TCPLANE_PHASE("an unknown directive is an error and a superseded one is reported as such");
    TCPLANE_ERROR_CODE(harness.runtime->verify_directive(DirectiveId{9999}), ErrorCode::UNKNOWN_DIRECTIVE);
}

TCPLANE_CASE(authority, mode_requests_follow_the_recovery_gate) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());
    TCPLANE_CHECK(harness.feed(0));
    TCPLANE_CHECK(harness.feed(0));

    TCPLANE_PHASE("a de-escalation request is refused while evidence requires a worse mode");
    AuthorityAttempt attempt = attempt_of(harness, RequestId{1}, AttemptId{1}, "m1", AttemptKind::RequestMode);
    attempt.requested_mode = ThermalMode::Normal;
    AuthorityOutcome outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(outcome.code),
                     static_cast<std::uint32_t>(ErrorCode::ESCALATION_REQUIRED));

    TCPLANE_PHASE("an escalation request is granted and applied");
    attempt.request = RequestId{2};
    attempt.attempt = AttemptId{2};
    attempt.key = IdempotencyKey::parse("m2").value();
    attempt.requested_mode = ThermalMode::Emergency;
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(harness.runtime->mode() == ThermalMode::Emergency);

    TCPLANE_PHASE("an operator isolation is held and cannot be left automatically");
    rebind(harness, attempt);
    attempt.request = RequestId{3};
    attempt.attempt = AttemptId{3};
    attempt.key = IdempotencyKey::parse("m3").value();
    attempt.requested_mode = ThermalMode::Isolated;
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(harness.runtime->mode() == ThermalMode::Isolated);
    TCPLANE_CHECK(harness.runtime->administrative_mode() == ThermalMode::Isolated);

    TCPLANE_PHASE("asking for the current mode again is a stable no-op grant");
    rebind(harness, attempt);
    attempt.request = RequestId{4};
    attempt.attempt = AttemptId{4};
    attempt.key = IdempotencyKey::parse("m4").value();
    outcome = harness.runtime->authorize(attempt).value();
    TCPLANE_CHECK(outcome.verdict == AuthorityVerdict::Granted);
    TCPLANE_CHECK(harness.runtime->mode() == ThermalMode::Isolated);
}

TCPLANE_CASE(authority, structural_errors_are_errors_not_refusals) {
    Harness harness;
    TCPLANE_CHECK(harness.prepare());
    AuthorityAttempt attempt = attempt_of(harness, RequestId{1}, AttemptId{1}, "x", AttemptKind::RequestDerate);

    TCPLANE_PHASE("an attempt without a request identity is an error");
    attempt.request = RequestId{0};
    TCPLANE_ERROR_CODE(harness.runtime->authorize(attempt), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an attempt without an attempt identity is an error");
    attempt.request = RequestId{1};
    attempt.attempt = AttemptId{0};
    TCPLANE_ERROR_CODE(harness.runtime->authorize(attempt), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an attempt without an idempotency key is an error");
    attempt.attempt = AttemptId{1};
    attempt.key = IdempotencyKey{};
    TCPLANE_ERROR_CODE(harness.runtime->authorize(attempt), ErrorCode::INVALID_KEY);

    TCPLANE_PHASE("malformed idempotency keys are refused by the key itself");
    TCPLANE_ERROR_CODE(IdempotencyKey::parse(""), ErrorCode::INVALID_KEY);
    TCPLANE_ERROR_CODE(IdempotencyKey::parse(std::string(65, 'a')), ErrorCode::INVALID_KEY);
    TCPLANE_ERROR_CODE(IdempotencyKey::parse("with space"), ErrorCode::INVALID_KEY);
    TCPLANE_ERROR_CODE(IdempotencyKey::parse(std::string("tab\there")), ErrorCode::INVALID_KEY);
    TCPLANE_OK(IdempotencyKey::parse("good-key_1"));

    TCPLANE_PHASE("a closed runtime refuses attempts with an explicit code");
    TCPLANE_STATUS_OK(harness.runtime->close());
    attempt.key = IdempotencyKey::parse("after-close").value();
    TCPLANE_ERROR_CODE(harness.runtime->authorize(attempt), ErrorCode::RUNTIME_CLOSED);
    TCPLANE_STATUS_OK(harness.runtime->close());
}
