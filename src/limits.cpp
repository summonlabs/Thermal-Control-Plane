// Thermal Control Plane — thermal limit validation and registry.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/limits.hpp"

namespace thermal_control_plane {
namespace {

bool same_thresholds(const ThermalLimitSet& a, const ThermalLimitSet& b) {
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        if (!(a.thresholds[i] == b.thresholds[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool ThermalLimitSet::any_defined() const noexcept { return defined_count() != 0; }

std::size_t ThermalLimitSet::defined_count() const noexcept {
    std::size_t count = 0;
    for (const Threshold& threshold : thresholds) {
        if (threshold.defined) {
            ++count;
        }
    }
    return count;
}

Status validate(const ThermalLimitSet& limits) {
    if (limits.id.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "limit set has no identity");
    }
    if (limits.generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "limit set has no generation");
    }
    if (limits.scope.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "limit set has no scope");
    }
    if (limits.scope_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "limit set has no scope generation");
    }
    if (limits.topology_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "limit set has no topology generation");
    }
    if (!limits.any_defined()) {
        return Status::failure(ErrorCode::LIMIT_UNDEFINED, "limit set defines no thresholds");
    }
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        const Threshold& threshold = limits.thresholds[i];
        if (!threshold.defined) {
            if (threshold.milli_celsius != 0) {
                return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                       "undefined threshold must not carry a temperature");
            }
            continue;
        }
        if (threshold.milli_celsius < Temperature::kAbsoluteZeroMilliC ||
            threshold.milli_celsius > Temperature::kMaxMilliC) {
            return Status::failure(ErrorCode::OUT_OF_RANGE, "threshold outside representable temperature range");
        }
    }
    for (std::size_t i = 1; i < kLimitKindCount; ++i) {
        const Threshold& lower = limits.thresholds[i - 1];
        const Threshold& upper = limits.thresholds[i];
        if (!lower.defined || !upper.defined) {
            continue;
        }
        if (upper.milli_celsius < lower.milli_celsius) {
            return Status::failure(ErrorCode::LIMIT_ORDER_INVALID,
                                   "limit thresholds are not ordered by severity");
        }
    }
    return Status::success();
}

LimitRegistry::LimitRegistry(std::size_t max_entries)
    : max_entries_(max_entries < 1 ? 1 : max_entries) {}

Status LimitRegistry::put(const ThermalLimitSet& limits) {
    const Status validation = validate(limits);
    if (!validation.ok()) {
        return validation;
    }
    const auto existing = limits_.find(limits.scope);
    if (existing == limits_.end()) {
        if (limits_.size() >= max_entries_) {
            return Status::failure(ErrorCode::RESOURCE_EXHAUSTED, "limit registry bound exceeded");
        }
        limits_.emplace(limits.scope, limits);
        return Status::success();
    }
    if (limits.generation < existing->second.generation) {
        return Status::failure(ErrorCode::STALE_OBJECT_GENERATION,
                               "limit generation is behind the published generation");
    }
    if (limits.generation == existing->second.generation) {
        if (limits.id == existing->second.id && limits.scope_generation == existing->second.scope_generation &&
            limits.topology_generation == existing->second.topology_generation &&
            limits.max_derate == existing->second.max_derate && same_thresholds(limits, existing->second)) {
            return Status::success();
        }
        return Status::failure(ErrorCode::LIMIT_SET_CONFLICT,
                               "different limits published at the same limit generation");
    }
    existing->second = limits;
    return Status::success();
}

Status LimitRegistry::remove(ScopeRef scope, LimitGeneration generation) {
    const auto existing = limits_.find(scope);
    if (existing == limits_.end()) {
        return Status::failure(ErrorCode::UNKNOWN_LIMIT_SET, "no limits are published for this scope");
    }
    if (generation <= existing->second.generation) {
        return Status::failure(ErrorCode::STALE_OBJECT_GENERATION,
                               "removal must advance the limit generation");
    }
    limits_.erase(existing);
    return Status::success();
}

const ThermalLimitSet* LimitRegistry::find(ScopeRef scope) const {
    const auto it = limits_.find(scope);
    return it == limits_.end() ? nullptr : &it->second;
}

std::vector<ScopeRef> LimitRegistry::scopes() const {
    std::vector<ScopeRef> out;
    out.reserve(limits_.size());
    for (const auto& entry : limits_) {
        out.push_back(entry.first);
    }
    return out;
}

}  // namespace thermal_control_plane
