// Thermal Control Plane — seeded property tests against independent reference models.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include "thermal_control_plane/wire.hpp"

#include <algorithm>
#include <random>
#include <string>

namespace {

using namespace thermal_control_plane;

const ScopeRef kScope = make_scope(1, 10, 100);

/// Independent reference: headroom is a plain subtraction of exact integers.
std::int64_t reference_headroom(std::int32_t limit, std::int32_t observed) {
    return static_cast<std::int64_t>(limit) - static_cast<std::int64_t>(observed);
}

/// Independent reference: the derating rung of the highest reached threshold.
std::uint32_t reference_derate(const std::array<std::int32_t, kLimitKindCount>& thresholds,
                               const std::array<std::uint32_t, kLimitKindCount>& ladder,
                               std::int32_t observed, std::uint32_t ceiling) {
    std::uint32_t selected = 0;
    for (std::size_t i = kLimitKindCount; i-- > 0;) {
        if (observed >= thresholds[i]) {
            selected = ladder[i];
            break;
        }
    }
    return selected > ceiling ? ceiling : selected;
}

/// Independent reference: mode precedence implemented from the documented rules.
ThermalMode reference_mode(const std::array<std::int32_t, kLimitKindCount>& thresholds,
                           std::int32_t observed, std::int32_t margin) {
    if (observed >= thresholds[limit_index(LimitKind::Shutdown)] ||
        observed >= thresholds[limit_index(LimitKind::Critical)]) {
        return ThermalMode::Emergency;
    }
    if (observed >= thresholds[limit_index(LimitKind::Derate)]) {
        return ThermalMode::Degraded;
    }
    if (observed >= thresholds[limit_index(LimitKind::Warning)]) {
        return ThermalMode::Constrained;
    }
    if (observed >= thresholds[limit_index(LimitKind::Warning)] - margin) {
        return ThermalMode::Constrained;
    }
    return ThermalMode::Normal;
}

/// Independent reference: ordered when every defined adjacent pair rises.
bool reference_ordered(const std::array<Threshold, kLimitKindCount>& thresholds) {
    for (std::size_t i = 1; i < kLimitKindCount; ++i) {
        if (!thresholds[i - 1].defined || !thresholds[i].defined) {
            continue;
        }
        if (thresholds[i].milli_celsius < thresholds[i - 1].milli_celsius) {
            return false;
        }
    }
    return true;
}

}  // namespace

TCPLANE_CASE(property, headroom_matches_the_reference_model) {
    std::mt19937_64 engine(0x5EED0001ULL);
    std::uniform_int_distribution<std::int32_t> limit_dist(20000, 110000);
    std::uniform_int_distribution<std::int32_t> observed_dist(0, 120000);

    TCPLANE_PHASE("10000 random pairs agree with the reference subtraction");
    for (std::uint32_t iteration = 0; iteration < 10000; ++iteration) {
        Threshold limit;
        limit.defined = true;
        limit.milli_celsius = limit_dist(engine);
        const std::int32_t observed = observed_dist(engine);
        const Headroom headroom =
            compute_headroom(Temperature::from_milli_celsius_unchecked(observed), limit, LimitKind::Derate);
        if (!headroom.known()) {
            TCPLANE_FAIL("headroom was not known for a defined limit at iteration " +
                         std::to_string(iteration));
        }
        const std::int64_t expected = reference_headroom(limit.milli_celsius, observed);
        if (headroom.delta.milli_celsius != expected) {
            TCPLANE_FAIL("headroom mismatch at iteration " + std::to_string(iteration) + ": got " +
                         std::to_string(headroom.delta.milli_celsius) + " want " + std::to_string(expected));
        }
        TCPLANE_CHECK(headroom.over_limit == (expected < 0));
    }
}

TCPLANE_CASE(property, derating_matches_the_reference_ladder) {
    std::mt19937_64 engine(0x5EED0002ULL);
    std::uniform_int_distribution<std::int32_t> threshold_dist(30000, 110000);
    std::uniform_int_distribution<std::int32_t> observed_dist(0, 120000);
    std::uniform_int_distribution<std::uint32_t> ceiling_dist(0, 10000);

    TCPLANE_PHASE("10000 random ladders agree with the reference selection");
    for (std::uint32_t iteration = 0; iteration < 10000; ++iteration) {
        std::array<std::int32_t, kLimitKindCount> thresholds{};
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            thresholds[i] = threshold_dist(engine);
        }
        std::sort(thresholds.begin(), thresholds.end());
        std::array<std::uint32_t, kLimitKindCount> ladder{};
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            ladder[i] = static_cast<std::uint32_t>(i) * 700U;
        }
        const std::uint32_t ceiling = ceiling_dist(engine);
        const std::int32_t observed = observed_dist(engine);

        ThermalLimitSet limits = make_limits(kScope, thresholds[0], thresholds[1], thresholds[2],
                                             thresholds[3], thresholds[4], 10000);
        DeratingLadder policy_ladder;
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            policy_ladder.at[i] = BasisPoints::from_value_unchecked(ladder[i]);
        }

        ScopeHeadroom scope;
        scope.scope = kScope;
        scope.has_limits = true;
        scope.has_hottest = true;
        scope.hottest = Temperature::from_milli_celsius_unchecked(observed);
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            scope.by_kind[i] =
                compute_headroom(scope.hottest, limits.thresholds[i], static_cast<LimitKind>(i + 1));
        }
        scope.governing = scope.by_kind[limit_index(LimitKind::Advisory)];

        const BasisPoints policy_ceiling = BasisPoints::from_value_unchecked(ceiling);
        const DerateSelection selection =
            select_derating(policy_ladder, policy_ceiling, BasisPoints::full(), scope, &limits);
        const std::uint32_t expected_ceiling = ceiling < 10000U ? ceiling : 10000U;
        const std::uint32_t expected =
            reference_derate(thresholds, ladder, observed, expected_ceiling);
        if (selection.derate.value() != expected) {
            TCPLANE_FAIL("derate mismatch at iteration " + std::to_string(iteration) + ": got " +
                         std::to_string(selection.derate.value()) + " want " + std::to_string(expected));
        }
    }
}

TCPLANE_CASE(property, limit_ordering_matches_the_reference_predicate) {
    std::mt19937_64 engine(0x5EED0003ULL);
    std::uniform_int_distribution<std::int32_t> value_dist(20000, 110000);
    std::uniform_int_distribution<int> defined_dist(0, 3);

    TCPLANE_PHASE("5000 random limit sets agree with the reference ordering test");
    for (std::uint32_t iteration = 0; iteration < 5000; ++iteration) {
        ThermalLimitSet limits = make_limits(kScope, 40000, 75000, 85000, 95000, 105000);
        for (std::size_t i = 0; i < kLimitKindCount; ++i) {
            limits.thresholds[i].milli_celsius = value_dist(engine);
            limits.thresholds[i].defined = defined_dist(engine) != 0;
            if (!limits.thresholds[i].defined) {
                limits.thresholds[i].milli_celsius = 0;
            }
        }
        const bool expected = reference_ordered(limits.thresholds) && limits.any_defined();
        const Status actual = validate(limits);
        if (expected != actual.ok()) {
            TCPLANE_FAIL("limit validation disagreed with the reference model at iteration " +
                         std::to_string(iteration));
        }
    }
}

TCPLANE_CASE(property, escalation_matches_the_reference_precedence) {
    std::mt19937_64 engine(0x5EED0004ULL);
    std::uniform_int_distribution<std::int32_t> observed_dist(0, 120000);

    EvidenceStore evidence;
    LimitRegistry limits{4};
    InterlockRegistry interlocks{4};
    const TopologyGeneration topology{1};
    const EvidenceGeneration generation{1};
    TCPLANE_STATUS_OK(evidence.reset(generation, topology));

    const std::array<std::int32_t, kLimitKindCount> thresholds{{40000, 75000, 85000, 95000, 105000}};
    TCPLANE_STATUS_OK(limits.put(make_limits(kScope, thresholds[0], thresholds[1], thresholds[2],
                                             thresholds[3], thresholds[4])));

    ThermalEnvironment env;
    env.evidence = &evidence;
    env.limits = &limits;
    env.interlocks = &interlocks;
    env.topology_generation = topology;

    ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    policy.freshness = FreshnessWindow{Duration::from_millis_unchecked(1000000)};

    TCPLANE_PHASE("2500 random observations agree with the reference mode model");
    std::uint64_t sequence = 0;
    for (std::uint32_t iteration = 0; iteration < 2500; ++iteration) {
        const std::int32_t observed = observed_dist(engine);
        ++sequence;
        TCPLANE_OK(evidence.ingest(
            make_batch(generation, topology,
                       {make_observation(kScope, SensorRef{1}, Tick{sequence}, observed,
                                         ObservationSequence{sequence}, generation)}),
            Tick{sequence}));

        PolicyInput input;
        input.now = Tick{sequence};
        input.current_mode = ThermalMode::Normal;
        input.evidence_generation = generation;
        input.topology_generation = topology;
        const PolicyOutcome outcome = evaluate_policy(policy, env, input);
        const ThermalMode expected = reference_mode(
            thresholds, observed, static_cast<std::int32_t>(policy.constrained_margin.milli_celsius));
        if (outcome.required_mode != expected) {
            TCPLANE_FAIL("mode mismatch at iteration " + std::to_string(iteration) + " for " +
                         std::to_string(observed) + "mC: got " +
                         std::string(to_string(outcome.required_mode)) + " want " +
                         std::string(to_string(expected)));
        }
    }
}

TCPLANE_CASE(property, semantic_and_byte_determinism_are_separate_claims) {
    TCPLANE_PHASE("semantic determinism: repeated evaluation is identical");
    EvidenceStore evidence;
    LimitRegistry limits{4};
    InterlockRegistry interlocks{4};
    const TopologyGeneration topology{1};
    const EvidenceGeneration generation{1};
    TCPLANE_STATUS_OK(evidence.reset(generation, topology));
    TCPLANE_STATUS_OK(limits.put(make_limits(kScope, 40000, 75000, 85000, 95000, 105000)));
    TCPLANE_OK(evidence.ingest(
        make_batch(generation, topology,
                   {make_observation(kScope, SensorRef{1}, Tick{10}, 91000, ObservationSequence{1},
                                     generation)}),
        Tick{10}));

    ThermalEnvironment env;
    env.evidence = &evidence;
    env.limits = &limits;
    env.interlocks = &interlocks;
    env.topology_generation = topology;
    const ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});

    PolicyInput input;
    input.now = Tick{10};
    input.evidence_generation = generation;
    input.topology_generation = topology;
    const PolicyOutcome first = evaluate_policy(policy, env, input);
    for (int repeat = 0; repeat < 32; ++repeat) {
        const PolicyOutcome again = evaluate_policy(policy, env, input);
        TCPLANE_CHECK(again.mode == first.mode);
        TCPLANE_CHECK(again.reason.cause == first.reason.cause);
        TCPLANE_CHECK(again.required_derate == first.required_derate);
        TCPLANE_CHECK(again.scopes.size() == first.scopes.size());
    }

    TCPLANE_PHASE("byte determinism: the canonical encoding is stable");
    ThermalSnapshot snapshot;
    snapshot.commit_sequence = CommitSequence{1};
    snapshot.epoch = ControlPlaneEpoch{1};
    snapshot.revision = StateRevision{1};
    snapshot.policy_present = true;
    snapshot.policy = policy;
    const auto encoded = encode_snapshot(snapshot);
    TCPLANE_OK(encoded);
    const std::uint64_t digest =
        wire::fnv1a64(std::span<const std::byte>(encoded.value().data(), encoded.value().size()));
    for (int repeat = 0; repeat < 16; ++repeat) {
        const auto again = encode_snapshot(snapshot);
        TCPLANE_OK(again);
        TCPLANE_CHECK(again.value() == encoded.value());
    }
    TCPLANE_NOTE("canonical snapshot digest " + wire::to_hex(digest));
    TCPLANE_CHECK(digest != 0);
}
