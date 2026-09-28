// Thermal Control Plane — recovery hysteresis proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

const ScopeRef kScope = make_scope(1, 10, 100);

ScopeHeadroom scope_at(const ThermalLimitSet& limits, std::int32_t milli_celsius,
                       ObservationSequence sequence) {
    ScopeHeadroom scope;
    scope.scope = kScope;
    scope.has_limits = true;
    scope.has_hottest = true;
    scope.hottest = Temperature::from_milli_celsius_unchecked(milli_celsius);
    scope.hottest_sequence = sequence;
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        scope.by_kind[i] =
            compute_headroom(scope.hottest, limits.thresholds[i], static_cast<LimitKind>(i + 1));
    }
    scope.governing = scope.by_kind[limit_index(LimitKind::Derate)];
    return scope;
}

RecoveryGateInput gate_input(ThermalMode mode, Tick at, std::int64_t dwell, std::uint32_t samples,
                             std::int64_t hysteresis) {
    RecoveryGateInput input;
    input.current_mode = mode;
    input.now = at;
    input.dwell = Duration::from_millis_unchecked(dwell);
    input.min_observations = samples;
    input.hysteresis = TemperatureDelta{hysteresis};
    return input;
}

}  // namespace

TCPLANE_CASE(recovery, gate_requires_dwell_and_distinct_samples) {
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);
    const std::vector<ScopeHeadroom> favourable{scope_at(limits, 70000, ObservationSequence{1})};

    TCPLANE_PHASE("a single favourable observation opens the gate but does not complete it");
    RecoveryGateState gate = advance_recovery_gate(RecoveryGateState{},
                                                  gate_input(ThermalMode::Degraded, Tick{1000}, 30000, 2, 5000),
                                                  favourable);
    TCPLANE_CHECK(gate.armed);
    TCPLANE_CHECK_EQ(gate.samples, std::uint32_t{1});
    TCPLANE_CHECK(!gate.satisfied);

    TCPLANE_PHASE("re-evaluating unchanged evidence is idempotent");
    for (int repeat = 0; repeat < 16; ++repeat) {
        gate = advance_recovery_gate(gate,
                                     gate_input(ThermalMode::Degraded, Tick{1000}, 30000, 2, 5000),
                                     favourable);
        TCPLANE_CHECK_EQ(gate.samples, std::uint32_t{1});
        TCPLANE_CHECK(!gate.satisfied);
    }

    TCPLANE_PHASE("a second distinct advance satisfies the sample requirement but not the dwell");
    gate = advance_recovery_gate(gate, gate_input(ThermalMode::Degraded, Tick{2000}, 30000, 2, 5000),
                                 {scope_at(limits, 70000, ObservationSequence{2})});
    TCPLANE_CHECK_EQ(gate.samples, std::uint32_t{2});
    TCPLANE_CHECK(!gate.satisfied);

    TCPLANE_PHASE("the dwell completes the gate");
    gate = advance_recovery_gate(gate, gate_input(ThermalMode::Degraded, Tick{32000}, 30000, 2, 5000),
                                 {scope_at(limits, 70000, ObservationSequence{2})});
    TCPLANE_CHECK(gate.satisfied);

    TCPLANE_PHASE("a worsening observation resets the whole run");
    gate = advance_recovery_gate(gate, gate_input(ThermalMode::Degraded, Tick{33000}, 30000, 2, 5000),
                                 {scope_at(limits, 86000, ObservationSequence{3})});
    TCPLANE_CHECK(!gate.satisfied);
    TCPLANE_CHECK_EQ(gate.samples, std::uint32_t{0});
    TCPLANE_CHECK(gate.favorable_since.is_zero());
}

TCPLANE_CASE(recovery, hysteresis_prevents_oscillation_at_the_threshold) {
    const ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);

    TCPLANE_PHASE("exactly at the derate threshold the gate never becomes favourable");
    RecoveryGateState gate = advance_recovery_gate(RecoveryGateState{},
                                                  gate_input(ThermalMode::Degraded, Tick{0}, 0, 1, 5000),
                                                  {scope_at(limits, 85000, ObservationSequence{1})});
    TCPLANE_CHECK(!gate.satisfied);
    TCPLANE_CHECK(gate.favorable_since.is_zero());

    TCPLANE_PHASE("inside the hysteresis band the gate still refuses");
    gate = advance_recovery_gate(gate, gate_input(ThermalMode::Degraded, Tick{100}, 0, 1, 5000),
                                 {scope_at(limits, 81000, ObservationSequence{2})});
    TCPLANE_CHECK(!gate.satisfied);

    TCPLANE_PHASE("below the band the gate completes immediately with a zero dwell");
    gate = advance_recovery_gate(gate, gate_input(ThermalMode::Degraded, Tick{200}, 0, 1, 5000),
                                 {scope_at(limits, 80000, ObservationSequence{3})});
    TCPLANE_CHECK(gate.satisfied);

    TCPLANE_PHASE("a missing scope ceiling blocks recovery rather than assuming safety");
    ThermalLimitSet sparse = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);
    sparse[LimitKind::Derate].defined = false;
    sparse[LimitKind::Derate].milli_celsius = 0;
    sparse[LimitKind::Warning].defined = false;
    sparse[LimitKind::Warning].milli_celsius = 0;
    const RecoveryGateState blocked =
        advance_recovery_gate(RecoveryGateState{},
                              gate_input(ThermalMode::Degraded, Tick{400}, 0, 1, 5000),
                              {scope_at(sparse, 20000, ObservationSequence{4})});
    TCPLANE_CHECK(!blocked.satisfied);
    TCPLANE_CHECK(blocked.blocked_reason == HeadroomReason::LimitUndefined);
}

TCPLANE_CASE(recovery, steps_down_one_rung_at_a_time) {
    TCPLANE_PHASE("each mode has exactly one successor");
    TCPLANE_CHECK(step_down(ThermalMode::Isolated) == ThermalMode::Emergency);
    TCPLANE_CHECK(step_down(ThermalMode::Emergency) == ThermalMode::Degraded);
    TCPLANE_CHECK(step_down(ThermalMode::Degraded) == ThermalMode::Constrained);
    TCPLANE_CHECK(step_down(ThermalMode::Constrained) == ThermalMode::Recovery);
    TCPLANE_CHECK(step_down(ThermalMode::Recovery) == ThermalMode::Normal);
    TCPLANE_CHECK(step_down(ThermalMode::Normal) == ThermalMode::Normal);

    TCPLANE_PHASE("severity ordering is strict for the thermal rungs");
    TCPLANE_CHECK(mode_severity(ThermalMode::Normal) < mode_severity(ThermalMode::Constrained));
    TCPLANE_CHECK(mode_severity(ThermalMode::Constrained) < mode_severity(ThermalMode::Degraded));
    TCPLANE_CHECK(mode_severity(ThermalMode::Degraded) < mode_severity(ThermalMode::Emergency));
    TCPLANE_CHECK(mode_severity(ThermalMode::Emergency) < mode_severity(ThermalMode::Isolated));

    TCPLANE_PHASE("maintenance and isolation require an operator to leave");
    TCPLANE_CHECK(requires_operator_to_leave(ThermalMode::Maintenance));
    TCPLANE_CHECK(requires_operator_to_leave(ThermalMode::Isolated));
    TCPLANE_CHECK(!requires_operator_to_leave(ThermalMode::Degraded));
}

TCPLANE_CASE(recovery, evaluation_needs_two_completed_gates_to_reach_normal) {
    EvidenceStore evidence;
    LimitRegistry limits{4};
    InterlockRegistry interlocks{4};
    const TopologyGeneration topology{1};
    const EvidenceGeneration generation{1};
    TCPLANE_STATUS_OK(evidence.reset(generation, topology));
    TCPLANE_STATUS_OK(limits.put(make_limits(kScope, 40000, 75000, 85000, 95000, 105000)));

    ThermalEnvironment env;
    env.evidence = &evidence;
    env.limits = &limits;
    env.interlocks = &interlocks;
    env.topology_generation = topology;

    ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    policy.freshness = FreshnessWindow{Duration::from_millis_unchecked(1000000)};
    // Zero dwell and a single sample keep each evaluation to exactly one
    // completed gate; the dedicated gate case proves the dwell semantics.
    policy.recovery_dwell = Duration::from_millis_unchecked(0);
    policy.recovery_min_observations = 1;
    policy.hysteresis = TemperatureDelta{5000};

    const auto observe = [&](std::int32_t milli_celsius, Tick at, std::uint64_t sequence) {
        TCPLANE_OK(evidence.ingest(
            make_batch(generation, topology,
                       {make_observation(kScope, SensorRef{1}, at, milli_celsius,
                                         ObservationSequence{sequence}, generation)}),
            at));
    };

    PolicyInput input;
    input.current_mode = ThermalMode::Normal;
    input.evidence_generation = generation;
    input.topology_generation = topology;

    TCPLANE_PHASE("a critical reading escalates to emergency");
    observe(97000, Tick{1000}, 1);
    input.now = Tick{1000};
    PolicyOutcome outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);

    TCPLANE_PHASE("one favourable evaluation steps down exactly one rung");
    observe(50000, Tick{2000}, 2);
    input.current_mode = outcome.mode;
    input.gate = outcome.gate;
    input.now = Tick{2000};
    outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(outcome.de_escalated);

    TCPLANE_PHASE("a second completion reaches constrained, not normal");
    observe(50000, Tick{3000}, 3);
    input.current_mode = outcome.mode;
    input.gate = outcome.gate;
    input.now = Tick{3000};
    outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Constrained);

    TCPLANE_PHASE("a third completion reaches recovery");
    observe(50000, Tick{4000}, 4);
    input.current_mode = outcome.mode;
    input.gate = outcome.gate;
    input.now = Tick{4000};
    outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Recovery);

    TCPLANE_PHASE("only a fourth completion returns the facility to normal");
    observe(50000, Tick{5000}, 5);
    input.current_mode = outcome.mode;
    input.gate = outcome.gate;
    input.now = Tick{5000};
    outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Normal);

    TCPLANE_PHASE("a single unfavourable observation immediately re-escalates");
    observe(97000, Tick{6000}, 6);
    input.current_mode = outcome.mode;
    input.gate = outcome.gate;
    input.now = Tick{6000};
    outcome = evaluate_policy(policy, env, input);
    TCPLANE_CHECK(outcome.mode == ThermalMode::Emergency);
    TCPLANE_CHECK(outcome.escalated);
}
