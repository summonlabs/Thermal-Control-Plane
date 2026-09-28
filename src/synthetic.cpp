// Thermal Control Plane — synthetic facility model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/synthetic.hpp"

#include <utility>

namespace thermal_control_plane {

SyntheticFacility::SyntheticFacility(FacilityId facility,
                                     TopologyGeneration topology_generation,
                                     std::vector<SyntheticZoneSpec> zones)
    : facility_(facility),
      topology_generation_(topology_generation),
      zones_(std::move(zones)),
      overrides_(zones_.size()),
      faults_(zones_.size(), {ObservationQuality::Measured, EvidenceReason::None}),
      sequences_(zones_.size(), 0) {}

ScopeRef SyntheticFacility::scope_of(std::size_t index) const {
    const SyntheticZoneSpec& spec = zones_.at(index);
    return ScopeRef{facility_, spec.zone, spec.domain};
}

std::vector<ScopeRef> SyntheticFacility::scopes() const {
    std::vector<ScopeRef> out;
    out.reserve(zones_.size());
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        out.push_back(scope_of(i));
    }
    return out;
}

std::int32_t SyntheticFacility::temperature(std::size_t index, BasisPoints derate) const {
    const SyntheticZoneSpec& spec = zones_.at(index);
    if (overrides_[index].has_value()) {
        return overrides_[index].value();
    }
    const std::int64_t relief =
        (static_cast<std::int64_t>(spec.derate_relief_milli_c) * static_cast<std::int64_t>(derate.value())) /
        10000;
    const std::int64_t value = static_cast<std::int64_t>(spec.baseline_milli_c) +
                               static_cast<std::int64_t>(spec.load_rise_milli_c) - relief;
    if (value < Temperature::kAbsoluteZeroMilliC) {
        return Temperature::kAbsoluteZeroMilliC;
    }
    if (value > Temperature::kMaxMilliC) {
        return Temperature::kMaxMilliC;
    }
    return static_cast<std::int32_t>(value);
}

void SyntheticFacility::override_temperature(std::size_t index, std::optional<std::int32_t> milli_celsius) {
    overrides_.at(index) = milli_celsius;
}

void SyntheticFacility::set_fault(std::size_t index, ObservationQuality quality, EvidenceReason reason) {
    faults_.at(index) = {quality, reason};
    overrides_.at(index) = std::nullopt;
}

ThermalLimitSet SyntheticFacility::limits_for(std::size_t index, LimitGeneration generation) const {
    const SyntheticZoneSpec& spec = zones_.at(index);
    ThermalLimitSet limits;
    limits.id = LimitSetId{static_cast<std::uint64_t>(index) + 1};
    limits.generation = generation;
    limits.scope = scope_of(index);
    limits.scope_generation = spec.scope_generation;
    limits.topology_generation = topology_generation_;
    limits.max_derate = spec.max_derate;
    for (std::size_t kind = 0; kind < kLimitKindCount; ++kind) {
        Threshold threshold;
        threshold.defined = true;
        threshold.milli_celsius = spec.thresholds[kind];
        limits.thresholds[kind] = threshold;
    }
    return limits;
}

std::vector<ThermalLimitSet> SyntheticFacility::all_limits(LimitGeneration generation) const {
    std::vector<ThermalLimitSet> out;
    out.reserve(zones_.size());
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        out.push_back(limits_for(i, generation));
    }
    return out;
}

ObservationSequence SyntheticFacility::sequence_of(std::size_t index) const {
    return ObservationSequence{sequences_.at(index)};
}

ObservationBatch SyntheticFacility::observe(Tick at,
                                            BasisPoints derate,
                                            ProcessIncarnation source,
                                            EvidenceGeneration generation) const {
    ObservationBatch batch;
    batch.generation = generation;
    batch.topology_generation = topology_generation_;
    batch.source = source;
    batch.observations.reserve(zones_.size());
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        const SyntheticZoneSpec& spec = zones_.at(i);
        sequences_[i] += 1;
        TemperatureObservation observation;
        observation.id = ObservationId{sequences_[i]};
        observation.sensor = spec.sensor;
        observation.scope = scope_of(i);
        observation.scope_generation = spec.scope_generation;
        observation.generation = generation;
        observation.sequence = ObservationSequence{sequences_[i]};
        observation.source = source;
        observation.observed_at = at;
        observation.quality = faults_[i].first;
        observation.reason = faults_[i].second;
        if (observation.quality == ObservationQuality::Measured ||
            observation.quality == ObservationQuality::Derived) {
            const auto value = Temperature::from_milli_celsius(temperature(i, derate));
            if (value.has_value()) {
                observation.temperature = value.value();
            }
        }
        batch.observations.push_back(observation);
    }
    return batch;
}

}  // namespace thermal_control_plane
