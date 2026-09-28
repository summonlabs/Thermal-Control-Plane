// Thermal Control Plane — evidence store implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/evidence.hpp"

#include <map>

namespace thermal_control_plane {
namespace {

std::size_t clamp_min(std::size_t value, std::size_t minimum) { return value < minimum ? minimum : value; }

/// True when two observations carry the same evidence payload.
///
/// The observation id, the producer incarnation and the advisory wall-clock
/// stamp are deliberately excluded: re-delivering the same measurement under a
/// fresh envelope is a retry, not new evidence.
bool same_payload(const TemperatureObservation& a, const TemperatureObservation& b) {
    return a.sensor == b.sensor && a.scope == b.scope && a.scope_generation == b.scope_generation &&
           a.sequence == b.sequence && a.observed_at == b.observed_at && a.temperature == b.temperature &&
           a.quality == b.quality && a.reason == b.reason;
}

bool is_measured(const TemperatureObservation& obs) {
    return obs.quality == ObservationQuality::Measured;
}

}  // namespace

EvidenceStore::EvidenceStore(EvidenceBounds bounds)
    : bounds_{clamp_min(bounds.max_sensors, 1),
              clamp_min(bounds.max_scopes, 1),
              clamp_min(bounds.max_observations_per_batch, 1)} {}

Status EvidenceStore::reset(EvidenceGeneration generation, TopologyGeneration topology_generation) {
    if (generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "evidence generation must be non-zero");
    }
    if (topology_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "topology generation must be non-zero");
    }
    generation_ = generation;
    topology_generation_ = topology_generation;
    slots_.clear();
    scope_index_.clear();
    revalidation_required_ = false;
    revalidation_tick_ = Tick{};
    return Status::success();
}

Status EvidenceStore::advance_generation(EvidenceGeneration expected_current,
                                         EvidenceGeneration next,
                                         TopologyGeneration topology_generation) {
    if (generation_.is_zero()) {
        return Status::failure(ErrorCode::EVIDENCE_REQUIRED, "no evidence generation has been established");
    }
    if (expected_current != generation_) {
        if (expected_current < generation_) {
            return Status::failure(ErrorCode::STALE_EVIDENCE_GENERATION,
                                   "expected evidence generation is behind the current generation");
        }
        return Status::failure(ErrorCode::FUTURE_EVIDENCE_GENERATION,
                               "expected evidence generation is ahead of the current generation");
    }
    if (topology_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "topology generation must be non-zero");
    }
    if (next < generation_) {
        return Status::failure(ErrorCode::STALE_EVIDENCE_GENERATION,
                               "evidence generation must advance monotonically");
    }
    if (next == generation_) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT,
                               "evidence generation advance must change the generation");
    }
    slots_.clear();
    scope_index_.clear();
    generation_ = next;
    topology_generation_ = topology_generation;
    revalidation_required_ = false;
    revalidation_tick_ = Tick{};
    return Status::success();
}

Result<IngestReport> EvidenceStore::ingest(const ObservationBatch& batch, Tick now) {
    if (generation_.is_zero()) {
        return Error{ErrorCode::EVIDENCE_REQUIRED, "no evidence generation has been established"};
    }
    if (batch.observations.size() > bounds_.max_observations_per_batch) {
        return Error{ErrorCode::RESOURCE_EXHAUSTED, "observation batch exceeds configured bound"};
    }
    if (batch.generation != generation_) {
        if (batch.generation < generation_) {
            return Error{ErrorCode::EVIDENCE_SUPERSEDED, "batch belongs to a superseded evidence generation"};
        }
        return Error{ErrorCode::FUTURE_EVIDENCE_GENERATION,
                     "batch belongs to a generation that has not been established"};
    }
    if (batch.topology_generation != topology_generation_) {
        if (batch.topology_generation < topology_generation_) {
            return Error{ErrorCode::STALE_TOPOLOGY_GENERATION,
                         "batch was produced against a superseded topology generation"};
        }
        return Error{ErrorCode::FUTURE_TOPOLOGY_GENERATION,
                     "batch was produced against an unknown topology generation"};
    }

    // Phase one: validate the batch in isolation. Nothing is stored until the
    // whole batch has been proven consistent.
    std::map<SensorRef, const TemperatureObservation*> unique;
    for (const TemperatureObservation& obs : batch.observations) {
        if (obs.sensor.is_zero()) {
            return Error{ErrorCode::INVALID_ARGUMENT, "observation has no sensor reference"};
        }
        if (obs.scope.is_zero()) {
            return Error{ErrorCode::INVALID_ARGUMENT, "observation has no scope reference"};
        }
        if (obs.scope_generation.is_zero()) {
            return Error{ErrorCode::INVALID_ARGUMENT, "observation has no scope generation"};
        }
        if (obs.id.is_zero()) {
            return Error{ErrorCode::INVALID_ARGUMENT, "observation has no observation id"};
        }
        if (obs.observed_at.is_zero()) {
            return Error{ErrorCode::INVALID_ARGUMENT, "observation has no monotonic stamp"};
        }
        if (obs.generation != generation_) {
            if (obs.generation < generation_) {
                return Error{ErrorCode::EVIDENCE_SUPERSEDED,
                             "observation belongs to a superseded evidence generation"};
            }
            return Error{ErrorCode::FUTURE_EVIDENCE_GENERATION,
                         "observation belongs to an unestablished evidence generation"};
        }
        if (obs.observed_at > now) {
            return Error{ErrorCode::EVIDENCE_FUTURE, "observation is stamped ahead of the clock"};
        }
        if (is_measured(obs)) {
            if (obs.reason != EvidenceReason::None) {
                return Error{ErrorCode::INVALID_ARGUMENT, "measured observation carries a failure reason"};
            }
        } else {
            if (obs.reason == EvidenceReason::None) {
                return Error{ErrorCode::INVALID_ARGUMENT, "non-measured observation must carry a reason"};
            }
            if (obs.quality != ObservationQuality::Derived && obs.temperature.milli_celsius() != 0) {
                return Error{ErrorCode::INVALID_ARGUMENT,
                             "unusable observation must not carry a temperature"};
            }
        }

        const auto existing = unique.find(obs.sensor);
        if (existing == unique.end()) {
            unique.emplace(obs.sensor, &obs);
            continue;
        }
        if (!same_payload(*existing->second, obs)) {
            return Error{ErrorCode::EVIDENCE_CONFLICT,
                         "batch carries conflicting observations for one sensor"};
        }
    }

    // Phase two: validate against stored state. New sensors and new scopes are
    // projected through local structures so the bound check cannot over-count.
    std::map<ScopeRef, bool> new_scopes;
    std::size_t projected_sensors = slots_.size();
    for (const auto& entry : unique) {
        const TemperatureObservation& obs = *entry.second;
        const auto slot = slots_.find(obs.sensor);
        if (slot != slots_.end()) {
            if (slot->second.latest.scope != obs.scope ||
                slot->second.latest.scope_generation != obs.scope_generation) {
                return Error{ErrorCode::EVIDENCE_CONFLICT,
                             "sensor is bound to a different scope than its stored binding"};
            }
            if (obs.sequence < slot->second.accepted_sequence) {
                return Error{ErrorCode::EVIDENCE_SUPERSEDED,
                             "observation sequence is behind the accepted sequence for this sensor"};
            }
            if (obs.sequence == slot->second.accepted_sequence && !same_payload(slot->second.latest, obs)) {
                return Error{ErrorCode::EVIDENCE_CONFLICT,
                             "observation reuses an accepted sequence with a different payload"};
            }
            continue;
        }
        ++projected_sensors;
        if (scope_index_.find(obs.scope) == scope_index_.end()) {
            new_scopes.emplace(obs.scope, true);
        }
    }
    if (projected_sensors > bounds_.max_sensors) {
        return Error{ErrorCode::RESOURCE_EXHAUSTED, "sensor bound exceeded"};
    }
    if (scope_index_.size() + new_scopes.size() > bounds_.max_scopes) {
        return Error{ErrorCode::RESOURCE_EXHAUSTED, "scope bound exceeded"};
    }

    // Phase three: apply. Every observation has already been accepted.
    IngestReport report;
    report.generation = generation_;
    for (const auto& entry : unique) {
        const TemperatureObservation& obs = *entry.second;
        auto slot = slots_.find(obs.sensor);
        if (slot == slots_.end()) {
            SensorSlot fresh;
            fresh.latest = obs;
            fresh.present = true;
            fresh.accepted_sequence = obs.sequence;
            slots_.emplace(obs.sensor, fresh);
            scope_index_[obs.scope].sensors.emplace(obs.sensor, true);
            ++report.applied;
            continue;
        }
        if (obs.sequence == slot->second.accepted_sequence) {
            ++report.duplicates;
            continue;
        }
        slot->second.latest = obs;
        slot->second.accepted_sequence = obs.sequence;
        ++report.applied;
    }

    if (report.applied > 0 && revalidation_required_) {
        for (const auto& entry : unique) {
            if (entry.second->observed_at >= revalidation_tick_) {
                revalidation_required_ = false;
                revalidation_tick_ = Tick{};
                break;
            }
        }
    }

    report.disposition = report.applied > 0 ? IngestDisposition::Applied : IngestDisposition::Duplicate;
    return report;
}

const SensorSlot* EvidenceStore::find(SensorRef sensor) const {
    const auto it = slots_.find(sensor);
    return it == slots_.end() ? nullptr : &it->second;
}

ScopeEvidenceView EvidenceStore::view(ScopeRef scope, Tick now, FreshnessWindow window) const {
    ScopeEvidenceView out;
    out.scope = scope;
    const auto index = scope_index_.find(scope);
    if (index == scope_index_.end()) {
        return out;
    }
    out.empty = index->second.sensors.empty();
    for (const auto& entry : index->second.sensors) {
        const auto slot = slots_.find(entry.first);
        if (slot == slots_.end() || !slot->second.present) {
            continue;
        }
        const TemperatureObservation& obs = slot->second.latest;
        ++out.sensors;
        if (!out.has_scope_generation) {
            out.has_scope_generation = true;
            out.scope_generation = obs.scope_generation;
        } else if (out.scope_generation != obs.scope_generation) {
            out.scope_generation_mixed = true;
        }
        const FreshnessState freshness = classify_freshness(now, obs.observed_at, window);
        const bool pre_recovery = revalidation_required_ && obs.observed_at < revalidation_tick_;

        if (is_measured(obs) && freshness == FreshnessState::Fresh && !pre_recovery) {
            ++out.fresh_measured;
            if (!out.has_hottest || out.hottest < obs.temperature ||
                (out.hottest == obs.temperature && obs.sensor < out.hottest_sensor)) {
                out.has_hottest = true;
                out.hottest = obs.temperature;
                out.hottest_sensor = obs.sensor;
                out.hottest_sequence = obs.sequence;
                out.hottest_at = obs.observed_at;
            }
            continue;
        }

        EvidenceReason reason = EvidenceReason::None;
        if (pre_recovery) {
            ++out.awaiting_revalidation;
            reason = EvidenceReason::AwaitingRevalidation;
        } else if (!is_measured(obs)) {
            ++out.not_measured;
            reason = obs.reason;
        } else if (freshness == FreshnessState::Future) {
            ++out.future_measured;
            reason = EvidenceReason::FutureObservation;
        } else {
            ++out.stale_measured;
            reason = EvidenceReason::StaleObservation;
        }

        if (out.primary_reason == EvidenceReason::None ||
            reason_precedence(reason) < reason_precedence(out.primary_reason) ||
            (reason_precedence(reason) == reason_precedence(out.primary_reason) &&
             obs.sensor < out.primary_reason_sensor)) {
            out.primary_reason = reason;
            out.primary_reason_sensor = obs.sensor;
        }
    }
    out.all_fresh_measured = !out.empty && out.fresh_measured == out.sensors;
    return out;
}

std::vector<ScopeRef> EvidenceStore::scopes() const {
    std::vector<ScopeRef> out;
    out.reserve(scope_index_.size());
    for (const auto& entry : scope_index_) {
        out.push_back(entry.first);
    }
    return out;
}

void EvidenceStore::require_revalidation(Tick at) noexcept {
    // Only the earliest outstanding recovery point survives: a later request
    // must not shrink the window of evidence that is considered suspect.
    if (!revalidation_required_ || at < revalidation_tick_) {
        revalidation_tick_ = at;
    }
    revalidation_required_ = true;
}

}  // namespace thermal_control_plane
