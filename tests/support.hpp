// Thermal Control Plane — shared test builders.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_TESTS_SUPPORT_HPP
#define THERMAL_CONTROL_PLANE_TESTS_SUPPORT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

namespace tcptest {

using namespace thermal_control_plane;

inline ScopeRef make_scope(std::uint32_t facility, std::uint64_t zone, std::uint64_t domain) {
    return ScopeRef{FacilityId{facility}, ZoneRef{zone}, DomainRef{domain}};
}

inline ThermalLimitSet make_limits(ScopeRef scope,
                                   std::int32_t advisory,
                                   std::int32_t warning,
                                   std::int32_t derate,
                                   std::int32_t critical,
                                   std::int32_t shutdown,
                                   std::uint32_t max_derate_bp = 4000,
                                   LimitGeneration generation = LimitGeneration{1},
                                   ScopeGeneration scope_generation = ScopeGeneration{1},
                                   TopologyGeneration topology = TopologyGeneration{1}) {
    ThermalLimitSet limits;
    limits.id = LimitSetId{1};
    limits.generation = generation;
    limits.scope = scope;
    limits.scope_generation = scope_generation;
    limits.topology_generation = topology;
    limits.max_derate = BasisPoints::from_value_unchecked(max_derate_bp);
    const std::int32_t values[kLimitKindCount] = {advisory, warning, derate, critical, shutdown};
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        limits.thresholds[i].defined = true;
        limits.thresholds[i].milli_celsius = values[i];
    }
    return limits;
}

inline TemperatureObservation make_observation(ScopeRef scope,
                                               SensorRef sensor,
                                               Tick at,
                                               std::int32_t milli_celsius,
                                               ObservationSequence sequence,
                                               EvidenceGeneration generation = EvidenceGeneration{1},
                                               ScopeGeneration scope_generation = ScopeGeneration{1}) {
    TemperatureObservation observation;
    observation.id = ObservationId{sequence.value};
    observation.sensor = sensor;
    observation.scope = scope;
    observation.scope_generation = scope_generation;
    observation.generation = generation;
    observation.sequence = sequence;
    observation.source = ProcessIncarnation{7};
    observation.observed_at = at;
    observation.temperature = Temperature::from_milli_celsius_unchecked(milli_celsius);
    return observation;
}

inline ObservationBatch make_batch(EvidenceGeneration generation,
                                   TopologyGeneration topology,
                                   std::vector<TemperatureObservation> observations) {
    ObservationBatch batch;
    batch.generation = generation;
    batch.topology_generation = topology;
    batch.source = ProcessIncarnation{7};
    batch.observations = std::move(observations);
    return batch;
}

inline std::unique_ptr<ThermalRuntime> make_runtime(std::unique_ptr<Clock> clock,
                                                    const std::string& store_path = std::string()) {
    RuntimeOptions options;
    options.store_path = store_path;
    auto created = ThermalRuntime::create(options, std::move(clock));
    if (!created.has_value()) {
        return nullptr;
    }
    return std::move(created.value());
}

/// Install baseline limits and a baseline interlock statement so that an
/// authority attempt can reach the decision logic.
inline bool prepare(ThermalRuntime& runtime, ScopeRef scope, Tick at) {
    const ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    if (!runtime.install_policy(policy, PolicyGeneration{}).ok()) {
        return false;
    }
    if (!runtime.publish_limits(make_limits(scope, 40000, 75000, 85000, 95000, 105000)).ok()) {
        return false;
    }
    if (!runtime.declare_interlock_baseline(runtime.topology_generation(), {}).ok()) {
        return false;
    }
    static_cast<void>(at);
    return true;
}

inline AuthorityAttempt make_attempt(const ThermalRuntime& runtime,
                                     RequestId request,
                                     AttemptId attempt,
                                     std::string key,
                                     AttemptKind kind) {
    AuthorityAttempt out;
    out.request = request;
    out.attempt = attempt;
    const auto parsed = IdempotencyKey::parse(key);
    if (parsed.has_value()) {
        out.key = parsed.value();
    }
    out.kind = kind;
    const auto binding = const_cast<ThermalRuntime&>(runtime).binding();
    if (binding.has_value()) {
        out.binding = binding.value();
    }
    return out;
}

}  // namespace tcptest

// Test sources use these builders unqualified throughout. The header is
// test-only and never installed, so the convenience import stays contained.
using namespace tcptest;

#endif  // THERMAL_CONTROL_PLANE_TESTS_SUPPORT_HPP
