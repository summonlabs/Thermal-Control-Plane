// Thermal Control Plane — policy precedence, determinism and escalation monotonicity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

const ScopeRef kScopeA = make_scope(1, 10, 100);
const ScopeRef kScopeB = make_scope(1, 11, 100);

/// A self-contained synthetic environment for pure policy evaluation.
struct Fixture {
    EvidenceStore evidence;
    LimitRegistry limits{8};
    InterlockRegistry interlocks{8};
    TopologyGeneration topology{1};
    EvidenceGeneration generation{1};
    ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});

    Fixture() {
        static_cast<void>(evidence.reset(generation, topology));
        static_cast<void>(limits.put(make_limits(kScopeA, 40000, 75000, 85000, 95000, 105000)));
        static_cast<void>(limits.put(make_limits(kScopeB, 40000, 76000, 86000, 96000, 106000)));
    }

    void observe(const ScopeRef& scope, SensorRef sensor, std::int32_t milli_celsius) {
        static std::uint64_t sequence = 0;
        sequence += 1;
        static_cast<void>(evidence.ingest(
            make_batch(generation, topology,
                       {make_observation(scope, sensor, Tick{100 + sequence}, milli_celsius,
                                         ObservationSequence{sequence}, generation)}),
            Tick{100 + sequence}));
    }

    ThermalEnvironment environment() {
        ThermalEnvironment env;
        env.evidence = &evidence;
        env.limits = &limits;
        env.interlocks = &interlocks;
        env.topology_generation = topology;
        return env;
    }

    PolicyOutcome evaluate(ThermalMode current, Tick at) {
        PolicyInput input;
        input.now = at;
        input.current_mode = current;
        input.evidence_generation = generation;
        input.topology_generation = topology;
        ThermalEnvironment env = environment();
        return evaluate_policy(policy, env, input);
    }
};

}  // namespace

TCPLANE_CASE(policy, interlock_outranks_a_limit_violation) {
    Fixture fixture;
    TCPLANE_PHASE("a shutdown violation alone escalates to emergency");
    fixture.observe(kScopeA, SensorRef{1}, 106000);
    PolicyOutcome outcome = fixture.evaluate(ThermalMode::Normal, Tick{200});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::ShutdownLimitViolation);

    TCPLANE_PHASE("an explicit safety interlock outranks the limit violation as the primary reason");
    Interlock interlock;
    interlock.id = InterlockId{1};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = true;
    interlock.topology_generation = fixture.topology;
    interlock.observed_at = Tick{150};
    TCPLANE_STATUS_OK(fixture.interlocks.apply(interlock));
    outcome = fixture.evaluate(ThermalMode::Normal, Tick{200});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::SafetyInterlock);

    TCPLANE_PHASE("an isolation interlock outranks everything and yields isolated");
    Interlock isolation = interlock;
    isolation.id = InterlockId{2};
    isolation.klass = InterlockClass::Isolation;
    TCPLANE_STATUS_OK(fixture.interlocks.apply(isolation));
    outcome = fixture.evaluate(ThermalMode::Normal, Tick{200});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Isolated);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::SafetyInterlock);
}

TCPLANE_CASE(policy, evidence_quality_precedence_and_degradation) {
    Fixture fixture;
    TCPLANE_PHASE("missing evidence degrades the facility, never normalises it");
    PolicyOutcome outcome = fixture.evaluate(ThermalMode::Normal, Tick{50});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::EvidenceUnknown);
    TCPLANE_CHECK(outcome.facility.state != HeadroomState::Known);

    TCPLANE_PHASE("a critical violation outranks an unknown scope");
    fixture.observe(kScopeA, SensorRef{1}, 96000);
    outcome = fixture.evaluate(ThermalMode::Normal, Tick{200});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::CriticalLimitViolation);
    TCPLANE_CHECK(outcome.reason.scope == kScopeA);

    TCPLANE_PHASE("the deterministic tie-break picks the lowest scope identity");
    Fixture tied;
    tied.observe(kScopeB, SensorRef{2}, 90000);
    tied.observe(kScopeA, SensorRef{1}, 90000);
    const PolicyOutcome both = tied.evaluate(ThermalMode::Normal, Tick{300});
    TCPLANE_CHECK(both.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(both.reason.scope == kScopeA);

    TCPLANE_PHASE("stale evidence outranks unknown evidence");
    Fixture stale;
    stale.observe(kScopeA, SensorRef{1}, 50000);
    stale.observe(kScopeB, SensorRef{2}, 50000);
    const PolicyOutcome aged = stale.evaluate(ThermalMode::Normal, Tick{100000});
    TCPLANE_CHECK(aged.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(aged.reason.cause == EscalationCause::EvidenceStale);
}

TCPLANE_CASE(policy, escalation_is_monotone_in_worsening_evidence) {
    TCPLANE_PHASE("raising the hottest zone never lowers the selected mode severity");
    std::uint8_t previous = 0;
    for (std::int32_t temperature = 20000; temperature <= 110000; temperature += 500) {
        Fixture fixture;
        fixture.observe(kScopeA, SensorRef{1}, temperature);
        fixture.observe(kScopeB, SensorRef{2}, temperature);
        const PolicyOutcome outcome = fixture.evaluate(ThermalMode::Normal, Tick{500});
        TCPLANE_CHECK(mode_severity(outcome.mode) >= previous);
        previous = mode_severity(outcome.mode);
    }

    TCPLANE_PHASE("losing evidence never lowers the selected mode severity");
    Fixture complete;
    complete.observe(kScopeA, SensorRef{1}, 50000);
    complete.observe(kScopeB, SensorRef{2}, 50000);
    const std::uint8_t healthy = mode_severity(complete.evaluate(ThermalMode::Normal, Tick{500}).mode);
    Fixture partial;
    partial.observe(kScopeA, SensorRef{1}, 50000);
    const std::uint8_t degraded = mode_severity(partial.evaluate(ThermalMode::Normal, Tick{500}).mode);
    TCPLANE_CHECK(degraded >= healthy);
}

TCPLANE_CASE(policy, escalation_is_mandatory_and_immediate) {
    Fixture fixture;
    fixture.observe(kScopeA, SensorRef{1}, 50000);
    fixture.observe(kScopeB, SensorRef{2}, 50000);
    TCPLANE_PHASE("a healthy facility remains normal");
    PolicyOutcome outcome = fixture.evaluate(ThermalMode::Normal, Tick{500});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Normal);
    TCPLANE_CHECK(outcome.facility.state == HeadroomState::Known);
    TCPLANE_CHECK_EQ(outcome.scopes.size(), std::size_t{2});
    TCPLANE_CHECK_EQ(
        outcome.scopes[0].by_kind[limit_index(LimitKind::Warning)].delta.milli_celsius,
        std::int64_t{25000});
    TCPLANE_CHECK_EQ(outcome.required_derate.value(), std::uint32_t{0});

    TCPLANE_PHASE("a critical reading escalates from normal in a single evaluation");
    fixture.observe(kScopeA, SensorRef{1}, 97000);
    outcome = fixture.evaluate(ThermalMode::Normal, Tick{600});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);
    TCPLANE_CHECK(outcome.escalated);
    TCPLANE_CHECK(outcome.required_mode == ThermalMode::Emergency);

    TCPLANE_PHASE("the advisory band constrains early, without a violation");
    Fixture advisory;
    advisory.observe(kScopeA, SensorRef{1}, 73500);
    advisory.observe(kScopeB, SensorRef{2}, 50000);
    const PolicyOutcome early = advisory.evaluate(ThermalMode::Normal, Tick{500});
    TCPLANE_CHECK(early.mode == ThermalMode::Constrained);
    TCPLANE_CHECK(early.reason.cause == EscalationCause::AdvisoryMargin);
}

TCPLANE_CASE(policy, maintenance_and_isolation_are_operator_held) {
    Fixture fixture;
    fixture.observe(kScopeA, SensorRef{1}, 50000);
    fixture.observe(kScopeB, SensorRef{2}, 50000);

    TCPLANE_PHASE("an operator maintenance window is entered and held");
    PolicyInput input;
    input.now = Tick{500};
    input.current_mode = ThermalMode::Normal;
    input.administrative_mode = ThermalMode::Maintenance;
    input.administrative_hold = true;
    ThermalEnvironment env = fixture.environment();
    PolicyOutcome outcome = evaluate_policy(fixture.policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Maintenance);
    TCPLANE_CHECK(outcome.reason.cause == EscalationCause::MaintenanceWindow);

    TCPLANE_PHASE("a held maintenance window survives a re-evaluation");
    input.current_mode = ThermalMode::Maintenance;
    outcome = evaluate_policy(fixture.policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Maintenance);
    TCPLANE_CHECK(!outcome.de_escalated);

    TCPLANE_PHASE("an operator hold defers an otherwise permitted step down");
    PolicyInput released = input;
    released.administrative_mode = ThermalMode::Normal;
    outcome = evaluate_policy(fixture.policy, env, released);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Maintenance);
    TCPLANE_CHECK(outcome.transition_deferred);

    TCPLANE_PHASE("a critical violation still outranks a maintenance window");
    fixture.observe(kScopeA, SensorRef{1}, 97000);
    outcome = evaluate_policy(fixture.policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);

    TCPLANE_PHASE("an isolation policy flag is refused by default");
    TCPLANE_CHECK(!fixture.policy.allow_automatic_isolation_exit);
}

TCPLANE_CASE(policy, evaluation_is_deterministic) {
    Fixture fixture;
    fixture.observe(kScopeA, SensorRef{1}, 90000);
    fixture.observe(kScopeB, SensorRef{2}, 87000);
    TCPLANE_PHASE("the same inputs produce the same mode, reason and derating");
    const PolicyOutcome first = fixture.evaluate(ThermalMode::Normal, Tick{900});
    for (int repeat = 0; repeat < 64; ++repeat) {
        const PolicyOutcome again = fixture.evaluate(ThermalMode::Normal, Tick{900});
        TCPLANE_CHECK(again.mode == first.mode);
        TCPLANE_CHECK(again.required_mode == first.required_mode);
        TCPLANE_CHECK(again.reason.cause == first.reason.cause);
        TCPLANE_CHECK(again.reason.scope == first.reason.scope);
        TCPLANE_CHECK(again.required_derate == first.required_derate);
        TCPLANE_CHECK(again.derate_scope == first.derate_scope);
    }
    TCPLANE_PHASE("the reported reason is the derate violation, not the warning band");
    TCPLANE_CHECK(first.reason.cause == EscalationCause::DerateLimitViolation);
    TCPLANE_CHECK(first.required_derate.value() >= 1500);
}

TCPLANE_CASE(policy, envelope_follows_the_mode_band) {
    Fixture fixture;
    fixture.observe(kScopeA, SensorRef{1}, 50000);
    fixture.observe(kScopeB, SensorRef{2}, 50000);
    TCPLANE_PHASE("normal mode caps the envelope at the warning threshold");
    PolicyOutcome outcome = fixture.evaluate(ThermalMode::Normal, Tick{500});
    TCPLANE_CHECK(outcome.envelope.ceiling_defined);
    TCPLANE_CHECK(outcome.envelope.ceiling_basis == LimitKind::Warning);
    TCPLANE_CHECK_EQ(outcome.envelope.ceiling.milli_celsius, 75000);
    TCPLANE_CHECK_EQ(outcome.envelope.max_derate.value(), std::uint32_t{3000});

    TCPLANE_PHASE("degraded mode caps the envelope at the critical threshold");
    fixture.observe(kScopeA, SensorRef{1}, 91000);
    outcome = fixture.evaluate(ThermalMode::Normal, Tick{600});
    TCPLANE_CHECK(outcome.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(outcome.envelope.ceiling_basis == LimitKind::Critical);
    TCPLANE_CHECK_EQ(outcome.envelope.ceiling.milli_celsius, 95000);
}
